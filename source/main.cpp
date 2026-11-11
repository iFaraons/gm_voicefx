// gm_voicefx — серверная обработка голоса для рации.
//
// Идея: перехватываем SV_BroadcastVoiceData и вызываем оригинал дважды:
//   1) с исходным пакетом — всем, кроме радиослушателей;
//   2) с обработанным («рация») пакетом — только радиослушателям.
// Кто кого слышит, движок решает по маскам IVoiceServer (их заполняет хук
// PlayerCanHearPlayersVoice). Мы временно правим эти маски на время двух вызовов
// и сразу возвращаем как было. Кто из слушателей «по рации», сообщает Lua: voicefx.SetRoute.
//
// Перехват и кодек основаны на gm_8bit (https://github.com/Meachamp/gm_8bit, LGPL-2.1).

#define NO_MALLOC_OVERRIDE

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <GarrysMod/Symbol.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include <iclient.h>
#include <ivoiceserver.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "opus_codec.h"
#include "radio_dsp.h"
#include "voice_packet.h"

using namespace VoiceFX;

namespace {

#if defined SYSTEM_WINDOWS
const std::vector<Symbol> kBroadcastVoiceSyms = {
#if defined ARCHITECTURE_X86
	Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50\x83\x78\x30\x00\x0F\x84****\x53\x8D\x4D\xD8\xC6\x45\xB4\x01\xC7\x45*****"),
	Symbol::FromSignature("\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****"),
	Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50"),
#elif defined ARCHITECTURE_X86_64
	Symbol::FromSignature("\x48\x89\x5C\x24*\x56\x57\x41\x56\x48\x81\xEC****\x8B\xF2\x4C\x8B\xF1"),
#endif
};
#elif defined SYSTEM_LINUX
const std::vector<Symbol> kBroadcastVoiceSyms = {
	Symbol::FromName("_Z21SV_BroadcastVoiceDataP7IClientiPcx"),
	Symbol::FromName("_Z21SV_BroadcastVoiceDataP7IClientiPcl"),
#if defined ARCHITECTURE_X86_64
	Symbol::FromSignature("\x55\x48\x8D\x05****\x48\x89\xE5\x41\x57\x41\x56\x41\x89\xF6\x41\x55\x49\x89\xFD\x41\x54\x49\x89\xD4\x53\x48\x89\xCB\x48\x81\xEC****\x48\x8B\x3D****\x48\x39\xC7\x74\x25"),
#endif
};
#endif

constexpr int kMaxPlayers = 128;
constexpr int kMaxSamples = 24000;     // до 1 с звука в одном пакете — с большим запасом
constexpr int kMaxPacket = 16 * 1024;

using BroadcastVoiceData_t = void (*)(IClient* cl, int nBytes, char* data, int64 xuid);

struct TalkerState {
	std::unique_ptr<SteamOpusCodec> codec;
	RadioChain dsp;
	int userId = -1;
	bool streaming = false; // предыдущий пакет этого игрока уже шёл через эффект
};

struct State {
	bool enabled = true;
	int maxClients = kMaxPlayers;
	RadioParams params;
	std::array<std::bitset<kMaxPlayers + 1>, kMaxPlayers + 1> radio{}; // [говорящий][слушатель], ent-индексы
	std::array<TalkerState, kMaxPlayers + 1> talkers{};
	uint64_t packetsProcessed = 0;
	uint64_t packetsFailed = 0;
};

Detouring::Hook g_detour;
IVoiceServer* g_voice = nullptr;
std::unique_ptr<State> g_state;

int16_t g_pcm[kMaxSamples];
char g_packet[kMaxPacket];

void CallOriginal(IClient* cl, int nBytes, char* data, int64 xuid) {
	g_detour.GetTrampoline<BroadcastVoiceData_t>()(cl, nBytes, data, xuid);
}

void ResetTalker(TalkerState& t) {
	t.codec.reset();
	t.dsp.Reset();
	t.streaming = false;
	t.userId = -1;
}

// Пакет -> «рация». Возвращает размер нового пакета или -1.
int ProcessRadio(TalkerState& t, const char* data, int nBytes) {
	if (!t.codec) {
		t.codec = std::make_unique<SteamOpusCodec>();
		if (!t.codec->IsValid()) {
			t.codec.reset();
			return -1;
		}
	}

	if (!t.streaming) {
		// Пока игрок говорил не в рацию, кадры шли мимо декодера — начинаем поток заново.
		t.codec->Resync();
		t.dsp.Reset();
		t.streaming = true;
	}

	bool endOfStream = false;
	int samples = DecodePacket(*t.codec, data, nBytes, g_pcm, kMaxSamples, endOfStream);
	if (samples < 0) {
		t.streaming = false;
		return -1;
	}

	t.dsp.Process(g_pcm, samples, g_state->params, static_cast<float>(kSampleRate));

	uint64_t steamid;
	std::memcpy(&steamid, data, sizeof(steamid));

	int out = EncodePacket(*t.codec, steamid, g_pcm, samples, endOfStream, g_packet, kMaxPacket);
	if (endOfStream) t.streaming = false;
	return out;
}

void Hook_BroadcastVoiceData(IClient* cl, int nBytes, char* data, int64 xuid) {
	State* s = g_state.get();
	if (!s || !s->enabled || !g_voice || !cl || !data || nBytes < kPacketOverhead) {
		return CallOriginal(cl, nBytes, data, xuid);
	}

	const int sender = cl->GetPlayerSlot() + 1; // ent-индекс говорящего
	if (sender < 1 || sender > s->maxClients) {
		return CallOriginal(cl, nBytes, data, xuid);
	}

	TalkerState& talker = s->talkers[sender];
	const int userId = cl->GetUserID();
	if (talker.userId != userId) {
		ResetTalker(talker); // слот занял другой игрок
		talker.userId = userId;
	}

	// Радиослушатели, которым движок и так собирается отдать этот голос.
	int radioList[kMaxPlayers];
	int radioCount = 0;
	const auto& routes = s->radio[sender];
	if (routes.any()) {
		for (int r = 1; r <= s->maxClients; r++) {
			if (r != sender && routes.test(r) && g_voice->GetClientListening(r, sender)) {
				radioList[radioCount++] = r;
			}
		}
	}

	if (radioCount == 0) {
		talker.streaming = false;
		return CallOriginal(cl, nBytes, data, xuid);
	}

	const int processedBytes = ProcessRadio(talker, data, nBytes);
	if (processedBytes < 0) {
		s->packetsFailed++;
		return CallOriginal(cl, nBytes, data, xuid); // лучше чистый голос, чем тишина
	}
	s->packetsProcessed++;

	// Проход 1: исходный голос — всем, кроме радиослушателей.
	for (int i = 0; i < radioCount; i++) {
		g_voice->SetClientListening(radioList[i], sender, false);
	}
	CallOriginal(cl, nBytes, data, xuid);

	// Проход 2: «рация» — только радиослушателям.
	int others[kMaxPlayers];
	int othersCount = 0;
	for (int r = 1; r <= s->maxClients; r++) {
		if (r != sender && g_voice->GetClientListening(r, sender)) {
			others[othersCount++] = r;
			g_voice->SetClientListening(r, sender, false);
		}
	}
	for (int i = 0; i < radioCount; i++) {
		g_voice->SetClientListening(radioList[i], sender, true);
	}

	if (processedBytes > 0) {
		CallOriginal(cl, processedBytes, g_packet, xuid);
	}

	// Возвращаем маски как были (радиослушатели уже снова true).
	for (int i = 0; i < othersCount; i++) {
		g_voice->SetClientListening(others[i], sender, true);
	}
}

bool ValidIndex(int i) {
	return i >= 1 && i <= kMaxPlayers;
}

// voicefx.SetRoute(talkerEntIndex, listenerEntIndex, viaRadio)
LUA_FUNCTION_STATIC(voicefx_SetRoute) {
	int talker = static_cast<int>(LUA->CheckNumber(1));
	int listener = static_cast<int>(LUA->CheckNumber(2));
	bool radio = LUA->GetBool(3);

	if (ValidIndex(talker) && ValidIndex(listener)) {
		g_state->radio[talker].set(listener, radio);
	}
	return 0;
}

// voicefx.ClearPlayer(entIndex) — при входе/выходе игрока.
LUA_FUNCTION_STATIC(voicefx_ClearPlayer) {
	int idx = static_cast<int>(LUA->CheckNumber(1));
	if (!ValidIndex(idx)) return 0;

	g_state->radio[idx].reset();
	for (auto& row : g_state->radio) row.reset(idx);
	ResetTalker(g_state->talkers[idx]);
	return 0;
}

// voicefx.ClearAll()
LUA_FUNCTION_STATIC(voicefx_ClearAll) {
	for (auto& row : g_state->radio) row.reset();
	for (auto& t : g_state->talkers) ResetTalker(t);
	return 0;
}

LUA_FUNCTION_STATIC(voicefx_SetEnabled) {
	LUA->CheckType(1, GarrysMod::Lua::Type::Bool);
	g_state->enabled = LUA->GetBool(1);
	return 0;
}

LUA_FUNCTION_STATIC(voicefx_SetMaxClients) {
	int n = static_cast<int>(LUA->CheckNumber(1));
	g_state->maxClients = n < 1 ? 1 : (n > kMaxPlayers ? kMaxPlayers : n);
	return 0;
}

// voicefx.SetParam(name, value): highpass, lowpass, mid_freq, mid_gain, mid_q, drive, crush_bits, noise, gain
LUA_FUNCTION_STATIC(voicefx_SetParam) {
	std::string name = LUA->CheckString(1);
	float value = static_cast<float>(LUA->CheckNumber(2));
	RadioParams& p = g_state->params;

	float* field = nullptr;
	if (name == "highpass") field = &p.highpassHz;
	else if (name == "lowpass") field = &p.lowpassHz;
	else if (name == "mid_freq") field = &p.midHz;
	else if (name == "mid_gain") field = &p.midGainDb;
	else if (name == "mid_q") field = &p.midQ;
	else if (name == "drive") field = &p.drive;
	else if (name == "crush_bits") field = &p.crushBits;
	else if (name == "noise") field = &p.noise;
	else if (name == "gain") field = &p.gain;

	if (!field) {
		LUA->ArgError(1, "unknown parameter");
		return 0;
	}

	*field = value;
	p.revision++;
	return 0;
}

// voicefx.GetListening(receiverEntIndex, senderEntIndex) — для отладки масок движка.
LUA_FUNCTION_STATIC(voicefx_GetListening) {
	int receiver = static_cast<int>(LUA->CheckNumber(1));
	int sender = static_cast<int>(LUA->CheckNumber(2));
	LUA->PushBool(g_voice && g_voice->GetClientListening(receiver, sender));
	return 1;
}

// voicefx.GetStats() -> processed, failed
LUA_FUNCTION_STATIC(voicefx_GetStats) {
	LUA->PushNumber(static_cast<double>(g_state->packetsProcessed));
	LUA->PushNumber(static_cast<double>(g_state->packetsFailed));
	return 2;
}

void SetFunction(GarrysMod::Lua::ILuaBase* LUA, const char* name, GarrysMod::Lua::CFunc fn) {
	LUA->PushCFunction(fn);
	LUA->SetField(-2, name);
}

} // namespace

GMOD_MODULE_OPEN() {
	SourceSDK::FactoryLoader engine("engine");

	g_voice = engine.GetInterface<IVoiceServer>(INTERFACEVERSION_VOICESERVER);
	if (!g_voice) {
		LUA->ThrowError("voicefx: IVoiceServer (" INTERFACEVERSION_VOICESERVER ") not found");
	}

	void* target = nullptr;
	SymbolFinder finder;
	for (const auto& sym : kBroadcastVoiceSyms) {
		target = finder.Resolve(engine.GetModule(), sym.name.c_str(), sym.length);
		if (target) break;
	}
	if (!target) {
		LUA->ThrowError("voicefx: SV_BroadcastVoiceData not found (engine updated? signatures need refresh)");
	}

	g_state = std::make_unique<State>();

	if (!g_detour.Create(Detouring::Hook::Target(target), reinterpret_cast<void*>(&Hook_BroadcastVoiceData)) || !g_detour.Enable()) {
		g_state.reset();
		LUA->ThrowError("voicefx: failed to detour SV_BroadcastVoiceData");
	}

	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
	LUA->CreateTable();
		SetFunction(LUA, "SetRoute", voicefx_SetRoute);
		SetFunction(LUA, "ClearPlayer", voicefx_ClearPlayer);
		SetFunction(LUA, "ClearAll", voicefx_ClearAll);
		SetFunction(LUA, "SetEnabled", voicefx_SetEnabled);
		SetFunction(LUA, "SetMaxClients", voicefx_SetMaxClients);
		SetFunction(LUA, "SetParam", voicefx_SetParam);
		SetFunction(LUA, "GetListening", voicefx_GetListening);
		SetFunction(LUA, "GetStats", voicefx_GetStats);
		LUA->PushString("1.0.0");
		LUA->SetField(-2, "Version");
	LUA->SetField(-2, "voicefx");
	LUA->Pop();

	return 0;
}

GMOD_MODULE_CLOSE() {
	g_detour.Disable();
	g_detour.Destroy();
	g_state.reset();
	g_voice = nullptr;
	return 0;
}
