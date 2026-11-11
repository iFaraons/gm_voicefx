#pragma once
// Разбор и сборка голосового пакета Steam:
//   [steamid64:u64] { [op:u8] payload }* [crc32:u32]
// Основано на steam_voice.h из gm_8bit (Meachamp, LGPL-2.1).

#include "opus_codec.h"
#include <checksum_crc.h>
#include <cstdint>
#include <cstring>

namespace VoiceFX {

enum : uint8_t {
	OP_SILENCE = 0,       // u16 — число сэмплов тишины
	OP_CODEC_OPUSPLC = 6, // u16 длина + кадры Opus
	OP_SAMPLERATE = 11,   // u16 — частота дискретизации
};

constexpr int kPacketOverhead = sizeof(uint64_t) + sizeof(uint32_t);

inline uint16_t ReadU16(const char* p) {
	return static_cast<uint8_t>(p[0]) | (static_cast<uint8_t>(p[1]) << 8);
}

inline void WriteU16(char* p, uint16_t v) {
	p[0] = static_cast<char>(v & 0xFF);
	p[1] = static_cast<char>(v >> 8);
}

// Пакет -> PCM. Возвращает число сэмплов или -1, если пакет битый.
inline int DecodePacket(SteamOpusCodec& codec, const char* data, int bytes, int16_t* out, int maxSamples, bool& endOfStream) {
	endOfStream = false;
	if (bytes < kPacketOverhead) return -1;

	const char* p = data + sizeof(uint64_t);
	const char* const end = data + bytes - sizeof(uint32_t);
	int written = 0;

	while (p < end) {
		uint8_t op = static_cast<uint8_t>(*p++);
		if (p + 2 > end) return -1;
		uint16_t arg = ReadU16(p);
		p += 2;

		switch (op) {
		case OP_SILENCE: {
			int n = std::min<int>(arg, maxSamples - written);
			std::memset(out + written, 0, n * sizeof(int16_t));
			written += n;
			break;
		}
		case OP_SAMPLERATE:
			if (arg != kSampleRate) return -1; // другой поток мы не перекодируем
			break;
		case OP_CODEC_OPUSPLC: {
			if (p + arg > end) return -1;
			int n = codec.Decompress(p, arg, out + written, maxSamples - written);
			if (n < 0) return -1;
			written += n;
			p += arg;
			if (codec.TakeEndOfStream()) endOfStream = true;
			break;
		}
		default:
			return -1;
		}
	}

	return written;
}

// PCM -> пакет. Возвращает число байт или -1.
inline int EncodePacket(SteamOpusCodec& codec, uint64_t steamid, const int16_t* pcm, int samples, bool endOfStream, char* out, int maxBytes) {
	// steamid + samplerate op + opus op header + crc
	constexpr int kHeader = sizeof(uint64_t) + 3 + 3;
	if (maxBytes < kHeader + static_cast<int>(sizeof(uint32_t))) return -1;

	char* p = out;
	std::memcpy(p, &steamid, sizeof(steamid));
	p += sizeof(steamid);

	*p++ = static_cast<char>(OP_SAMPLERATE);
	WriteU16(p, kSampleRate);
	p += 2;

	*p++ = static_cast<char>(OP_CODEC_OPUSPLC);
	char* lenPos = p;
	p += 2;

	int room = maxBytes - static_cast<int>(p - out) - static_cast<int>(sizeof(uint32_t));
	int n = codec.Compress(pcm, samples, endOfStream, p, room);
	if (n < 0) return -1;

	WriteU16(lenPos, static_cast<uint16_t>(n));
	p += n;

	CRC32_t crc = CRC32_ProcessSingleBuffer(out, static_cast<int>(p - out));
	std::memcpy(p, &crc, sizeof(crc));
	p += sizeof(crc);

	return static_cast<int>(p - out);
}

} // namespace VoiceFX
