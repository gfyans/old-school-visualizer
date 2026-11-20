#pragma once
#include "audio/AudioFeatures.h"
#include "audio/BeatTracker.h"
#include "audio/Fft.h"
#include <complex>

// Turns recent mono audio into AudioFeatures: FFT, log-spaced bands, automatic
// gain, attack/release smoothing, onset beats, the beat grid (BeatTracker) and
// song-structure cues (build-ups, drops). Pure computation, no Windows/audio
// APIs, so it can be tested with synthetic signals.
class AudioAnalyzer {
public:
    static constexpr int kFftSize = 2048;

    AudioAnalyzer();

    // 1. Every frame, all *new* samples since the last frame (feeds the beat grid).
    void ProcessStream(const float* samples, size_t count, float sampleRate);
    // 2. Then the latest kFftSize samples, oldest first. newData: false if nothing arrived.
    void Update(const float* samples, float sampleRate, bool newData, float dt);

    const AudioFeatures& Features() const { return m_features; }
    const BeatTracker::State& TrackerState() const { return m_beats.GetState(); }   // diagnostics

private:
    static constexpr int kBins = kFftSize / 2;
    static constexpr int kBands = AudioFeatures::kSpectrumBands;

    void ConfigureBands(float sampleRate);
    void UpdateWaveform(const float* samples, float sampleRate, float dt);
    void UpdateStructure(float bassPower, float highPower, float totalPower, float dt);

    Fft m_fftEngine{ kFftSize };
    float m_window[kFftSize];
    std::complex<float> m_fft[kFftSize];
    float m_power[kBins];

    // Band layout (depends on sample rate)
    float m_configuredRate = 0;
    int m_bandLo[kBands], m_bandHi[kBands];
    float m_bandCenter[kBands], m_bandTilt[kBands];
    int m_fluxLo = 1, m_fluxHi = 2;
    int m_bassBinHi = 2, m_highBinLo = 3, m_highBinHi = 4;
    float m_prevLogMag[kBins] = {};

    // Adaptive state
    float m_referenceDb = -50.0f;
    float m_fluxMean = 0, m_fluxVar = 0;
    float m_timeSinceBeat = 10.0f;
    float m_wavePeak = 0.01f;

    // Structure detection (raw dB, before auto-gain)
    bool m_structureInit = false;
    float m_bassShort = 0, m_bassRef = 0;
    float m_highShort = 0;
    float m_levelFast = 0, m_levelSlow = 0;
    float m_highHistory[40] = {};   // 4 s of m_highShort at 10 Hz
    int m_highWrite = 0, m_highCount = 0;
    float m_historyTimer = 0;
    float m_bassAbsentTime = 0;
    float m_lastBassAbsent = 0;     // previous frame's m_bassAbsentTime
    float m_dropCooldown = 0;
    float m_activeTime = 0;         // seconds of continuous (non-silent) audio
    float m_rhythmHold = 0;         // > 0 while a confident beat was seen recently

    BeatTracker m_beats;
    AudioFeatures m_features;
};
