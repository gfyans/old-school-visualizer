#include "audio/AudioAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMinFreq = 30.0f, kMaxFreq = 16000.0f;
constexpr float kRangeDb = 45.0f;      // dynamic range shown below the auto-gain reference
constexpr float kFloorDb = -50.0f;     // auto-gain never boosts beyond this (keeps silence dark)

// Exponential approach with separate rise/fall time constants (seconds); frame-rate independent.
float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

} // namespace

AudioAnalyzer::AudioAnalyzer() {
    for (int i = 0; i < kFftSize; ++i)
        m_window[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * i / (kFftSize - 1));   // Hann
    for (int k = 0; k < kFftSize / 2; ++k)
        m_twiddle[k] = std::polar(1.0f, -2.0f * kPi * k / kFftSize);
    int bits = 0;
    while ((1 << bits) < kFftSize) ++bits;
    for (int i = 0; i < kFftSize; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        m_bitReverse[i] = r;
    }
}

void AudioAnalyzer::ConfigureBands(float sampleRate) {
    m_configuredRate = sampleRate;
    const float binHz = sampleRate / kFftSize;
    const float ratio = std::pow(kMaxFreq / kMinFreq, 1.0f / kBands);
    for (int b = 0; b < kBands; ++b) {
        const float f0 = kMinFreq * std::pow(ratio, float(b));
        const float f1 = f0 * ratio;
        int lo = std::clamp(int(f0 / binHz), 1, kBins - 1);
        int hi = std::clamp(int(f1 / binHz), lo + 1, kBins);
        m_bandLo[b] = lo;
        m_bandHi[b] = hi;
        m_bandCenter[b] = std::sqrt(f0 * f1);
        // Music has far more energy in the bass; tilt +3 dB/octave around 1 kHz so bars look even.
        m_bandTilt[b] = 3.0f * std::log2(m_bandCenter[b] / 1000.0f);
    }
    // Beat detection looks at onsets in the kick-drum range.
    m_fluxLo = std::max(1, int(40.0f / binHz));
    m_fluxHi = std::max(m_fluxLo + 1, int(180.0f / binHz) + 1);
}

void AudioAnalyzer::Fft() {
    for (int i = 0; i < kFftSize; ++i)
        if (i < m_bitReverse[i]) std::swap(m_fft[i], m_fft[m_bitReverse[i]]);
    for (int len = 2; len <= kFftSize; len <<= 1) {
        const int half = len >> 1, step = kFftSize / len;
        for (int i = 0; i < kFftSize; i += len) {
            for (int j = 0; j < half; ++j) {
                const std::complex<float> v = m_fft[i + j + half] * m_twiddle[j * step];
                m_fft[i + j + half] = m_fft[i + j] - v;
                m_fft[i + j] += v;
            }
        }
    }
}

void AudioAnalyzer::Update(const float* samples, float sampleRate, bool newData, float dt) {
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (sampleRate != m_configuredRate) ConfigureBands(sampleRate);
    AudioFeatures& f = m_features;

    // --- Spectrum ---------------------------------------------------------
    double sumSq = 0.0;
    for (int i = 0; i < kFftSize; ++i) {
        sumSq += double(samples[i]) * samples[i];
        m_fft[i] = { samples[i] * m_window[i], 0.0f };
    }
    const float rms = float(std::sqrt(sumSq / kFftSize));
    Fft();

    const float norm = 4.0f / kFftSize;   // full-scale sine -> magnitude 1 with a Hann window
    for (int k = 0; k < kBins; ++k) m_power[k] = std::norm(m_fft[k]) * norm * norm;

    float bandDb[kBands];
    float peakDb = -200.0f;
    for (int b = 0; b < kBands; ++b) {
        double p = 0.0;
        for (int k = m_bandLo[b]; k < m_bandHi[b]; ++k) p += m_power[k];
        p /= (m_bandHi[b] - m_bandLo[b]);
        bandDb[b] = 10.0f * std::log10(float(p) + 1e-12f) + m_bandTilt[b];
        peakDb = std::max(peakDb, bandDb[b]);
    }

    // --- Automatic gain: follow the loudest band, rise fast, fall slowly ---
    m_referenceDb = std::max(Smooth(m_referenceDb, peakDb, dt, 0.05f, 6.0f), kFloorDb);
    const float bottomDb = m_referenceDb - kRangeDb;

    float raw[kBands];
    float groups[4] = {}, groupCount[4] = {};
    float rawLevel = 0.0f;
    for (int b = 0; b < kBands; ++b) {
        raw[b] = std::clamp((bandDb[b] - bottomDb) / kRangeDb, 0.0f, 1.0f);
        f.spectrum[b] = Smooth(f.spectrum[b], raw[b], dt, 0.015f, 0.12f);
        rawLevel += raw[b];

        const float fc = m_bandCenter[b];
        const int g = fc < 250.0f ? 0 : fc < 1000.0f ? 1 : fc < 4000.0f ? 2 : 3;
        groups[g] += raw[b];
        groupCount[g] += 1.0f;
    }
    rawLevel /= kBands;
    for (int g = 0; g < 4; ++g) groups[g] /= std::max(groupCount[g], 1.0f);

    f.bass    = Smooth(f.bass,    groups[0], dt, 0.02f, 0.25f);
    f.lowMid  = Smooth(f.lowMid,  groups[1], dt, 0.02f, 0.25f);
    f.highMid = Smooth(f.highMid, groups[2], dt, 0.02f, 0.20f);
    f.treble  = Smooth(f.treble,  groups[3], dt, 0.01f, 0.15f);
    f.level   = Smooth(f.level,   rawLevel,  dt, 0.05f, 0.30f);
    f.intensity = Smooth(f.intensity, rawLevel, dt, 5.0f, 5.0f);
    f.silent = rms < 1e-4f;   // about -80 dBFS

    // --- Beat detection: spectral flux in the kick range vs adaptive threshold ---
    m_timeSinceBeat += dt;
    f.beat *= std::exp(-dt / 0.12f);
    if (newData) {
        float flux = 0.0f;
        for (int k = m_fluxLo; k < m_fluxHi; ++k) {
            const float logMag = 0.5f * std::log(m_power[k] + 1e-10f);
            flux += std::max(0.0f, logMag - m_prevLogMag[k]);
            m_prevLogMag[k] = logMag;
        }
        flux /= float(m_fluxHi - m_fluxLo);

        const float threshold = m_fluxMean + 1.5f * std::sqrt(m_fluxVar) + 0.1f;
        if (flux > threshold && groups[0] > 0.3f && m_timeSinceBeat > 0.28f) {
            f.beat = 1.0f;
            ++f.beatCount;
            m_timeSinceBeat = 0.0f;
        }

        const float a = 1.0f - std::exp(-dt / 1.0f);   // ~1 s memory
        const float d = flux - m_fluxMean;
        m_fluxMean += a * d;
        m_fluxVar += a * (d * d - m_fluxVar);
    }
}
