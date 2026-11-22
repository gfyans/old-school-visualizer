#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <vector>

#include "audio/Fft.h"

// Tempo and beat-phase tracker. Fed the continuous sample stream, it:
//   1. computes an onset-strength envelope at 100 Hz (log spectral flux),
//   2. scores every chain of beats with dynamic programming over a joint (tempo, position in
//      the beat) state space, so the best chain over the whole history decides the tempo and
//      where the beats fall, and changing tempo (above all to another metrical level) costs,
//   3. refines that tempo on the autocorrelation of ~5 s of the envelope,
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
        // Diagnostics behind the confidence (for tuning tools).
        float periodicity = 0.0f;  // normalised autocorrelation at the beat period
        float contrast = 0.0f;     // how sharply the best beat position beats the others
        float phaseError = 0.0f;   // average |phase correction| per estimate (0 = rock steady)
        float percussive = 0.0f;   // 0..1: how broadband the onsets are (drums high, piano/pads low)
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
    void ChainsStep(float onset);   // advance the beat-chain DP by one envelope tick
    struct Chain { int period = 0; int position = 0; };
    Chain BestChain();              // the chain to follow now (sticky), and renormalise
    float EnvelopeAt(float ticksAgo) const;   // linear interpolation into the history

    float m_rate = 0.0f;
    int m_hop = 0;
    Fft m_fft;
    std::vector<float> m_window;
    std::vector<float> m_input;        // sliding analysis window
    int m_inputFill = 0;               // samples accumulated toward the next hop
    std::vector<std::complex<float>> m_spectrum;
    static constexpr int kBands = 40;
    std::array<int, kBands + 1> m_bandEdge{};
    std::array<float, kBands> m_prevBand{};
    std::array<float, kBands> m_prevFine{};
    float m_bandPeak = 0.0f;

    float m_envelope[kEnvelopeSize] = {};      // onset strength history
    int m_envWrite = 0;
    uint64_t m_hopCount = 0;
    float m_lastOnset = 0.0f;
    float m_onsetMean = 0.0f;

    // Tempo
    float m_period = 0.0f;             // in envelope ticks (100 Hz)
    float m_rawConfidence = 0.0f;
    float m_phaseErrorAvg = 0.25f;     // start sceptical
    float m_broadestJump = 0.0f, m_percussive = 0.0f;
    int m_sinceEstimate = 0;

    // Beat chains: one log score per (period, position) state, a ring per period so moving
    // every state one position on is just a head step.
    static constexpr int kMinChainPeriod = 33, kMaxChainPeriod = 92;   // ticks: 182-65 BPM
    static constexpr int kChainTempi = kMaxChainPeriod - kMinChainPeriod + 1;
    std::vector<float> m_chain;                      // all states, period by period
    std::array<int, kChainTempi> m_chainStart{};     // first state of each period in m_chain
    std::array<int, kChainTempi> m_chainHead{};      // ring index of position 0
    std::vector<float> m_chainTrans;                 // log P(next period | period), kChainTempi^2
    std::array<float, kChainTempi> m_chainPrior{};   // log tempo preference, per beat
    float m_chainOnsetMean = 0.0f;
    float m_chainRecent[3] = {};      // the last three onsets, newest first
    int m_chainSwitchVotes = 0;
    Chain m_followed;                 // the chain followed at the last estimate (period 0: none yet)
    int m_lockedEstimates = 0;        // estimates since the grid first locked (level switches get sticky)

    // Phase-locked oscillator
    float m_phase = 0.0f;              // 0..1
    int m_barBeat = 0;                 // 0..3
    float m_accent[4] = {};            // onset strength seen at each beat-of-bar
    float m_peakSinceBeat = 0.0f;

    State m_state;
};
