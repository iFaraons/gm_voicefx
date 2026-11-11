#pragma once
// Кодек голосового потока Steam (Opus, 24 кГц, моно, кадры по 480 сэмплов).
// Основан на Opus_FrameDecoder из gm_8bit (Meachamp, LGPL-2.1), переработан:
//   * PCM знаковый (int16_t) — нужно для фильтров;
//   * Resync(): начать поток заново без «заполнения потерь» PLC-кадрами;
//   * маркер конца передачи (0xFFFF) пробрасывается из декодера в кодер.

#include "opus.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace VoiceFX {

constexpr int kSampleRate = 24000;
constexpr int kFrameSize = 480;       // 20 мс
constexpr uint16_t kEndOfStream = 0xFFFF;
constexpr int kMaxConcealFrames = 10; // не больше 200 мс PLC на одну дыру

class SteamOpusCodec {
public:
	SteamOpusCodec() {
		int err = 0;
		m_dec = opus_decoder_create(kSampleRate, 1, &err);
		m_enc = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
	}

	~SteamOpusCodec() {
		if (m_dec) opus_decoder_destroy(m_dec);
		if (m_enc) opus_encoder_destroy(m_enc);
	}

	SteamOpusCodec(const SteamOpusCodec&) = delete;
	SteamOpusCodec& operator=(const SteamOpusCodec&) = delete;

	bool IsValid() const { return m_dec && m_enc; }

	// Поток прерывался (пакеты шли мимо нас): начинаем с чистого листа.
	void Resync() {
		opus_decoder_ctl(m_dec, OPUS_RESET_STATE);
		opus_encoder_ctl(m_enc, OPUS_RESET_STATE);
		m_resync = true;
		m_encodeSeq = 0;
		m_pending.clear();
	}

	// Был ли в последнем декодированном блоке маркер конца передачи.
	bool TakeEndOfStream() {
		bool v = m_sawEnd;
		m_sawEnd = false;
		return v;
	}

	// Блок кадров [len:u16][seq:u16][opus...]... -> PCM. Возвращает число сэмплов или -1.
	int Decompress(const char* in, int inBytes, int16_t* out, int maxSamples) {
		const char* p = in;
		const char* const end = in + inBytes;
		int written = 0;

		while (p + 2 <= end) {
			uint16_t len = Read16(p);

			if (len == kEndOfStream) {
				opus_decoder_ctl(m_dec, OPUS_RESET_STATE);
				m_seq = 0;
				m_sawEnd = true;
				continue;
			}

			if (p + 2 > end) return -1;
			uint16_t seq = Read16(p);

			if (m_resync) {
				m_resync = false;
				m_seq = seq;
			}

			if (seq < m_seq) {
				opus_decoder_ctl(m_dec, OPUS_RESET_STATE);
			} else if (seq > m_seq) {
				int lost = std::min<int>(seq - m_seq, kMaxConcealFrames);
				for (int i = 0; i < lost && written < maxSamples; i++) {
					int n = opus_decode(m_dec, nullptr, 0, out + written, maxSamples - written, 0);
					if (n < 0) break;
					written += n;
				}
			}
			m_seq = seq + 1;

			if (len == 0 || p + len > end) return -1;
			if (written >= maxSamples) return -1;

			int n = opus_decode(m_dec, reinterpret_cast<const unsigned char*>(p), len, out + written, maxSamples - written, 0);
			if (n < 0) return -1;

			written += n;
			p += len;
		}

		return written;
	}

	// PCM -> блок кадров. Неполный кадр копится до следующего вызова;
	// при endOfStream добивается тишиной и дописывается маркер 0xFFFF.
	// Возвращает число байт или -1.
	int Compress(const int16_t* in, int samples, bool endOfStream, char* out, int maxBytes) {
		m_pending.insert(m_pending.end(), in, in + samples);

		if (endOfStream && !m_pending.empty()) {
			size_t rem = m_pending.size() % kFrameSize;
			if (rem) m_pending.resize(m_pending.size() + (kFrameSize - rem), 0);
		}

		char* p = out;
		char* const end = out + maxBytes;
		size_t consumed = 0;

		while (m_pending.size() - consumed >= static_cast<size_t>(kFrameSize)) {
			if (p + 4 > end) return -1;

			char* lenPos = p;
			p += 2;
			Write16(p, m_encodeSeq++);

			int maxFrame = static_cast<int>(std::min<ptrdiff_t>(0x7FFF, end - p));
			int n = opus_encode(m_enc, m_pending.data() + consumed, kFrameSize, reinterpret_cast<unsigned char*>(p), maxFrame);
			if (n < 0) return -1;

			char* tmp = lenPos;
			Write16(tmp, static_cast<uint16_t>(n));
			p += n;
			consumed += kFrameSize;
		}
		m_pending.erase(m_pending.begin(), m_pending.begin() + consumed);

		if (endOfStream) {
			if (p + 2 > end) return -1;
			Write16(p, kEndOfStream);
			opus_encoder_ctl(m_enc, OPUS_RESET_STATE);
			m_encodeSeq = 0;
			m_pending.clear();
		}

		return static_cast<int>(p - out);
	}

private:
	static uint16_t Read16(const char*& p) {
		uint16_t v = static_cast<uint8_t>(p[0]) | (static_cast<uint8_t>(p[1]) << 8);
		p += 2;
		return v;
	}

	static void Write16(char*& p, uint16_t v) {
		p[0] = static_cast<char>(v & 0xFF);
		p[1] = static_cast<char>(v >> 8);
		p += 2;
	}

	OpusDecoder* m_dec = nullptr;
	OpusEncoder* m_enc = nullptr;
	uint16_t m_seq = 0;
	uint16_t m_encodeSeq = 0;
	bool m_resync = true;
	bool m_sawEnd = false;
	std::vector<int16_t> m_pending;
};

} // namespace VoiceFX
