#pragma once
#include "audio/AudioFeatures.h"
#include "audio/BeatTracker.h"
#include "audio/Fft.h"
#include <cmath>
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
    // 3. Optionally, a recent stereo window (mono + side, oldest first, count >= 2 * 25 ms)
    //    for the oscilloscope data.
    void UpdateScope(const float* mono, const float* side, size_t count, float sampleRate, float dt);

    const AudioFeatures& Features() const { return m_features; }
    const BeatTracker::State& TrackerState() const { return m_beats.GetState(); }   // diagnostics

private:
    static constexpr int kBins = kFftSize / 2;
    static constexpr int kBands = AudioFeatures::kSpectrumBands;

    void ConfigureBands(float sampleRate);
    void UpdateWaveform(const float* samples, float sampleRate, float dt);
    void UpdateStructure(float bassPower, float highPower, float totalPower, float dt);
    void UpdateInstruments(bool newData, float dt);
    void UpdateHarmony(float dt);

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

    // Instrument detectors: band-limited spectral flux with adaptive thresholds.
    struct FluxBand {
        int lo = 1, hi = 2;
        float mean = 0, var = 0, value = 0;
        bool Spike(float k) const { return value > mean + k * std::sqrt(var) + 0.04f; }
    };
    FluxBand m_snareBody, m_snareNoise, m_hatBand;
    float m_prevInstLog[kBins] = {};
    int m_vocalLo = 1, m_vocalHi = 2;

    // Harmony: pitch-class profile (chroma) per bin, long (key) and short (chord) averages.
    signed char m_binPitch[kBins] = {};   // pitch class 0..11, or -1 outside 150 Hz - 5 kHz
    float m_chromaLong[12] = {}, m_chromaShort[12] = {};
    int m_chordCandidate = -1;
    float m_chordHold = 0;
    float m_brightness = 0;
    float m_sinceSnare = 1, m_sinceHat = 1;
    float m_vocalFast = -100, m_vocalSlow = -100, m_vocalMod = 0, m_tonal = 0, m_vocalPresence = 0;

    // Adaptive state
    float m_referenceDb = -50.0f;
    float m_fluxMean = 0, m_fluxVar = 0;
    float m_timeSinceBeat = 10.0f;
    float m_onsetInterval = 0.5f;   // smoothed seconds between heard onset beats
    float m_wavePeak = 0.01f;
    float m_scopePeak = 0.01f;

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
