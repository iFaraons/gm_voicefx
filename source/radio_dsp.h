#pragma once
// Эффект «голос из рации»:
//   ФВЧ 4-го порядка -> ФНЧ 4-го порядка -> пик на середине («гнусавость»)
//   -> мягкое ограничение (перегруз динамика) -> понижение разрядности -> шум эфира -> громкость.
// Состояние фильтров хранится на каждого говорящего, чтобы между пакетами не было щелчков.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VoiceFX {

struct RadioParams {
	float highpassHz = 380.0f;  // срез низов
	float lowpassHz = 3000.0f;  // срез верхов
	float midHz = 1800.0f;      // центр «гнусавого» подъёма
	float midGainDb = 6.0f;     // величина подъёма, дБ (0 — выкл.)
	float midQ = 0.9f;
	float drive = 2.5f;         // перегруз: 1 — почти чисто, 5+ — сильно рвёт
	float crushBits = 0.0f;     // разрядность 4..15, 0 — выкл.
	float noise = 0.012f;       // уровень шипения (доля полной шкалы)
	float gain = 1.0f;          // итоговая громкость
	uint32_t revision = 1;      // растёт при каждом изменении — фильтры пересчитают коэффициенты
};

class Biquad {
public:
	void Reset() { m_z1 = m_z2 = 0.0f; }

	float Process(float x) {
		float y = m_b0 * x + m_z1;
		m_z1 = m_b1 * x - m_a1 * y + m_z2;
		m_z2 = m_b2 * x - m_a2 * y;
		return y;
	}

	// Формулы RBJ Audio EQ Cookbook.
	void SetLowpass(float fs, float f, float q) {
		float w = Omega(fs, f), c = std::cos(w), a = std::sin(w) / (2.0f * q);
		Set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a);
	}

	void SetHighpass(float fs, float f, float q) {
		float w = Omega(fs, f), c = std::cos(w), a = std::sin(w) / (2.0f * q);
		Set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a);
	}

	void SetPeak(float fs, float f, float q, float db) {
		float A = std::pow(10.0f, db / 40.0f);
		float w = Omega(fs, f), c = std::cos(w), a = std::sin(w) / (2.0f * q);
		Set(1 + a * A, -2 * c, 1 - a * A, 1 + a / A, -2 * c, 1 - a / A);
	}

private:
	static float Omega(float fs, float f) {
		f = std::clamp(f, 10.0f, fs * 0.45f);
		return 2.0f * 3.14159265f * f / fs;
	}

	void Set(float b0, float b1, float b2, float a0, float a1, float a2) {
		m_b0 = b0 / a0; m_b1 = b1 / a0; m_b2 = b2 / a0;
		m_a1 = a1 / a0; m_a2 = a2 / a0;
	}

	float m_b0 = 1, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;
	float m_z1 = 0, m_z2 = 0;
};

class RadioChain {
public:
	void Reset() {
		for (Biquad* b : {&m_hp1, &m_hp2, &m_lp1, &m_lp2, &m_mid}) b->Reset();
	}

	void Process(int16_t* pcm, int samples, const RadioParams& p, float sampleRate) {
		if (m_revision != p.revision) {
			Configure(p, sampleRate);
			m_revision = p.revision;
		}

		const float drive = std::max(p.drive, 0.01f);
		const float driveNorm = 1.0f / std::tanh(drive);
		const bool crush = p.crushBits >= 1.0f && p.crushBits < 16.0f;
		const float levels = crush ? std::pow(2.0f, std::floor(p.crushBits) - 1.0f) : 0.0f;
		const bool mid = std::fabs(p.midGainDb) > 0.01f;

		for (int i = 0; i < samples; i++) {
			float x = pcm[i] * (1.0f / 32768.0f);

			x = m_hp2.Process(m_hp1.Process(x));
			x = m_lp2.Process(m_lp1.Process(x));
			if (mid) x = m_mid.Process(x);

			x = std::tanh(x * drive) * driveNorm;

			if (crush) x = std::round(x * levels) / levels;

			x += p.noise * NextNoise();
			x *= p.gain;

			x = std::clamp(x, -1.0f, 1.0f);
			pcm[i] = static_cast<int16_t>(x * 32767.0f);
		}
	}

private:
	void Configure(const RadioParams& p, float fs) {
		// Каскад двух звеньев с этими Q = фильтр Баттерворта 4-го порядка.
		constexpr float q1 = 0.5412f, q2 = 1.3066f;
		m_hp1.SetHighpass(fs, p.highpassHz, q1);
		m_hp2.SetHighpass(fs, p.highpassHz, q2);
		m_lp1.SetLowpass(fs, p.lowpassHz, q1);
		m_lp2.SetLowpass(fs, p.lowpassHz, q2);
		m_mid.SetPeak(fs, p.midHz, std::max(p.midQ, 0.1f), p.midGainDb);
	}

	// xorshift32 -> [-1, 1]
	float NextNoise() {
		m_rng ^= m_rng << 13;
		m_rng ^= m_rng >> 17;
		m_rng ^= m_rng << 5;
		return static_cast<float>(m_rng) * (2.0f / 4294967295.0f) - 1.0f;
	}

	Biquad m_hp1, m_hp2, m_lp1, m_lp2, m_mid;
	uint32_t m_revision = 0;
	uint32_t m_rng = 0x9E3779B9u;
};

} // namespace VoiceFX
