#pragma once
#include "audio/AudioFeatures.h"
#include <complex>

// Turns a window of recent mono samples into AudioFeatures: FFT, log-spaced
// bands, automatic gain, attack/release smoothing and bass-onset beat detection.
// Pure computation (no Windows/audio APIs) so it can be tested with synthetic signals.
class AudioAnalyzer {
public:
    static constexpr int kFftSize = 2048;

    AudioAnalyzer();

    // samples: the latest kFftSize mono samples, oldest first.
    // newData: false if no new audio arrived since the previous call.
    void Update(const float* samples, float sampleRate, bool newData, float dt);
    const AudioFeatures& Features() const { return m_features; }

private:
    static constexpr int kBins = kFftSize / 2;
    static constexpr int kBands = AudioFeatures::kSpectrumBands;

    void ConfigureBands(float sampleRate);
    void Fft();

    // FFT tables and buffers
    float m_window[kFftSize];
    std::complex<float> m_twiddle[kFftSize / 2];
    int m_bitReverse[kFftSize];
    std::complex<float> m_fft[kFftSize];
    float m_power[kBins];

    // Band layout (depends on sample rate)
    float m_configuredRate = 0;
    int m_bandLo[kBands], m_bandHi[kBands];
    float m_bandCenter[kBands], m_bandTilt[kBands];
    int m_fluxLo = 1, m_fluxHi = 2;
    float m_prevLogMag[kBins] = {};

    // Adaptive state
    float m_referenceDb = -50.0f;
    float m_fluxMean = 0, m_fluxVar = 0;
    float m_timeSinceBeat = 10.0f;

    AudioFeatures m_features;
};
