#pragma once
#include <complex>
#include <cstdint>
#include <vector>

#include "audio/Fft.h"

// Tempo and beat-phase tracker. Fed the continuous sample stream, it:
//   1. computes an onset-strength envelope at 100 Hz (log spectral flux),
//   2. estimates the beat period by autocorrelating ~6 s of that envelope,
//   3. finds where the beats fall with a comb filter over recent onsets,
//   4. runs a phase-locked oscillator so beats are *predicted*, not detected late.
// Also guesses the downbeat (which beat of 4 is accented) for bar phase.
class BeatTracker {
public:
    struct State {
        float bpm = 0.0f;          // 0 until a tempo is found
        float beatPhase = 0.0f;    // 0..1 through the current beat (0 = on the beat)
        float barPhase = 0.0f;     // 0..1 through the current 4-beat bar
        uint32_t beatIndex = 0;    // increments on every predicted beat
        float confidence = 0.0f;   // 0..1: how periodic / stable the music is
    };

    // All new mono samples since the previous call, in order.
    void Process(const float* samples, size_t count, float sampleRate);
    const State& GetState() const { return m_state; }

    // Latest onset strength (for other detectors), roughly 0..1+.
    float Onset() const { return m_lastOnset; }

private:
    static constexpr float kEnvelopeRate = 100.0f;   // Hz
    static constexpr int kEnvelopeSize = 1024;       // ~10 s history

    void Configure(float sampleRate);
    void ProcessHop();
    void EstimateTempo();
    // Linear interpolation into a history (default: the full-band onset envelope).
    float EnvelopeAt(float ticksAgo, const float* envelope = nullptr) const;

    float m_rate = 0.0f;
    int m_hop = 0;
    Fft m_fft;
    std::vector<float> m_window;
    std::vector<float> m_input;        // sliding analysis window
    int m_inputFill = 0;               // samples accumulated toward the next hop
    std::vector<std::complex<float>> m_spectrum;
    std::vector<float> m_prevLogMag;
    int m_maxBin = 0;

    float m_envelope[kEnvelopeSize] = {};      // full-band onsets: tempo
    float m_lowEnvelope[kEnvelopeSize] = {};   // low-frequency onsets: phase
    float m_lowMean = 0.0f;
    int m_envWrite = 0;
    uint64_t m_hopCount = 0;
    float m_lastOnset = 0.0f;
    float m_onsetMean = 0.0f;

    // Tempo
    float m_period = 0.0f;             // in envelope ticks (100 Hz)
    float m_candidatePeriod = 0.0f;
    int m_candidateVotes = 0;
    float m_rawConfidence = 0.0f;
    int m_sinceEstimate = 0;

    // Phase-locked oscillator
    float m_phase = 0.0f;              // 0..1
    int m_barBeat = 0;                 // 0..3
    float m_accent[4] = {};            // onset strength seen at each beat-of-bar
    float m_peakSinceBeat = 0.0f;

    State m_state;
};
