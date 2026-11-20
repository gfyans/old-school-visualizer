#include "audio/BeatTracker.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMinBpm = 65.0f, kMaxBpm = 185.0f;
constexpr int kTempoWindow = 500;        // 5 s of envelope for autocorrelation
constexpr int kEstimateEvery = 25;       // re-estimate tempo/phase every 0.25 s
constexpr int kCombBeats = 8;            // beats looked back over when locating phase

float Wrap01(float x) { return x - std::floor(x); }
float WrapHalf(float x) { return x - std::floor(x + 0.5f); }   // to [-0.5, 0.5)
}

void BeatTracker::Configure(float sampleRate) {
    m_rate = sampleRate;
    m_hop = std::max(1, static_cast<int>(std::lround(sampleRate / kEnvelopeRate)));
    const int windowSize = sampleRate > 60000.0f ? 2048 : 1024;   // ~21 ms either way
    m_fft.Resize(windowSize);
    m_window = Fft::Hann(windowSize);
    m_input.assign(windowSize, 0.0f);
    m_inputFill = 0;
    m_spectrum.assign(windowSize, {});
    m_maxBin = std::min(windowSize / 2, static_cast<int>(6000.0f * windowSize / sampleRate));
    m_prevLogMag.assign(m_maxBin + 1, 0.0f);
}

void BeatTracker::Process(const float* samples, size_t count, float sampleRate) {
    if (sampleRate <= 0.0f) return;
    if (sampleRate != m_rate) Configure(sampleRate);
    const int n = static_cast<int>(m_input.size());
    for (size_t i = 0; i < count; ++i) {
        // Sliding window: shift is amortised by only rebuilding the frame once per hop.
        m_input[(m_hopCount * m_hop + m_inputFill) % n] = samples[i];
        if (++m_inputFill >= m_hop) {
            m_inputFill = 0;
            ProcessHop();
        }
    }
}

void BeatTracker::ProcessHop() {
    const int n = static_cast<int>(m_input.size());
    // Oldest sample sits right after the newest one in the ring.
    const size_t start = ((m_hopCount + 1) * m_hop) % n;
    for (int i = 0; i < n; ++i) m_spectrum[i] = { m_input[(start + i) % n] * m_window[i], 0.0f };
    m_fft.Transform(m_spectrum.data());

    // Log-compressed spectral flux: sum of magnitude increases, with the kick range weighted up.
    const float norm = 4.0f / n;
    const float binHz = m_rate / n;
    float flux = 0.0f, fluxLow = 0.0f;
    int lowBins = 0;
    for (int k = 1; k <= m_maxBin; ++k) {
        const float logMag = std::log1p(100.0f * std::abs(m_spectrum[k]) * norm);
        const float d = logMag - m_prevLogMag[k];
        m_prevLogMag[k] = logMag;
        const bool low = k * binHz < 200.0f;
        lowBins += low;
        if (d > 0.0f) {
            flux += d * (low ? 2.0f : 1.0f);
            if (low) fluxLow += d;
        }
    }
    flux /= m_maxBin;
    fluxLow /= std::max(lowBins, 1);

    // Remove the slowly varying baselines so only onsets remain.
    m_onsetMean += (flux - m_onsetMean) * 0.01f;   // ~1 s
    m_lowMean += (fluxLow - m_lowMean) * 0.01f;
    const float onset = std::max(0.0f, flux - m_onsetMean);
    m_lastOnset = onset;
    m_envelope[m_envWrite] = onset;
    // Phase is located mostly from low-frequency onsets: the beat is felt in the kick/bass,
    // and broadband hats on the off-beat would otherwise pull the grid half a beat out.
    m_lowEnvelope[m_envWrite] = std::max(0.0f, fluxLow - m_lowMean) + 0.15f * onset;
    m_envWrite = (m_envWrite + 1) % kEnvelopeSize;
    ++m_hopCount;

    // --- Oscillator: advance one hop, count beats, learn which beat of the bar is accented ---
    if (m_period > 0.0f) {
        if (m_phase < 0.12f || m_phase > 0.9f) m_peakSinceBeat = std::max(m_peakSinceBeat, onset);
        const float before = m_phase;
        m_phase += 1.0f / m_period;
        if (before < 0.12f && m_phase >= 0.12f) {   // window around the beat has closed
            m_accent[m_barBeat] = 0.92f * m_accent[m_barBeat] + 0.08f * m_peakSinceBeat;
            m_peakSinceBeat = 0.0f;
        }
        if (m_phase >= 1.0f) {
            m_phase -= 1.0f;
            ++m_state.beatIndex;
            m_barBeat = (m_barBeat + 1) & 3;
        }
    }

    if (++m_sinceEstimate >= kEstimateEvery) {
        m_sinceEstimate = 0;
        EstimateTempo();
    }

    // --- Publish ---
    const float stability = m_candidateVotes == 0 ? 1.0f : 0.7f;
    m_state.confidence += (m_rawConfidence * stability - m_state.confidence) * 0.02f;   // ~0.5 s
    m_state.bpm = m_period > 0.0f ? 60.0f * kEnvelopeRate / m_period : 0.0f;
    m_state.beatPhase = m_phase;

    // Downbeat = the most accented beat position, if it clearly stands out.
    int downbeat = 0;
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        sum += m_accent[i];
        if (m_accent[i] > m_accent[downbeat]) downbeat = i;
    }
    if (m_accent[downbeat] < 1.15f * sum / 4.0f) downbeat = 0;
    m_state.barPhase = (static_cast<float>((m_barBeat - downbeat + 4) & 3) + m_phase) / 4.0f;
}

float BeatTracker::EnvelopeAt(float ticksAgo, const float* envelope) const {
    if (!envelope) envelope = m_envelope;
    if (ticksAgo < 0.0f) ticksAgo = 0.0f;
    const int i0 = static_cast<int>(ticksAgo);
    const float f = ticksAgo - i0;
    auto at = [&](int ago) { return envelope[((m_envWrite - 1 - ago) % kEnvelopeSize + kEnvelopeSize) % kEnvelopeSize]; };
    return at(i0) * (1.0f - f) + at(i0 + 1) * f;
}

void BeatTracker::EstimateTempo() {
    if (m_hopCount < static_cast<uint64_t>(kTempoWindow)) return;

    // Recent envelope, oldest first, mean removed.
    float e[kTempoWindow];
    float mean = 0.0f;
    for (int i = 0; i < kTempoWindow; ++i) {
        e[i] = EnvelopeAt(static_cast<float>(kTempoWindow - 1 - i));
        mean += e[i];
    }
    mean /= kTempoWindow;
    for (float& v : e) v -= mean;

    const int lagMin = static_cast<int>(60.0f * kEnvelopeRate / kMaxBpm);
    const int lagMax = static_cast<int>(60.0f * kEnvelopeRate / kMinBpm) + 1;
    const int lagTop = std::min(2 * lagMax + 1, kTempoWindow - 1);
    float ac[2 * 100 + 4] = {};
    for (int lag = 0; lag <= lagTop; ++lag) {
        float s = 0.0f;
        for (int i = lag; i < kTempoWindow; ++i) s += e[i] * e[i - lag];
        ac[lag] = s;
    }
    if (ac[0] < 1e-9f) {   // silence / no onsets
        m_rawConfidence = 0.0f;
        return;
    }

    // Score each candidate period: its autocorrelation plus its double (a true beat period
    // also repeats at 2x), weighted by a broad preference for ~120 BPM.
    float score[2 * 100 + 4] = {};
    int best = lagMin;
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        const float bpm = 60.0f * kEnvelopeRate / lag;
        const float octaves = std::log2(bpm / 120.0f);
        const float prior = std::exp(-0.5f * octaves * octaves);
        const float twice = 2 * lag <= lagTop ? ac[2 * lag] : 0.0f;
        score[lag] = (ac[lag] + 0.5f * twice) / ac[0] * prior;
        if (score[lag] > score[best]) best = lag;
    }
    // Parabolic interpolation for a fractional period.
    float period = static_cast<float>(best);
    if (best > lagMin && best < lagMax) {
        const float a = score[best - 1], b = score[best], c = score[best + 1];
        const float denom = a - 2.0f * b + c;
        if (denom < 0.0f) period += 0.5f * (a - c) / denom;
    }
    m_rawConfidence = std::clamp(ac[best] / ac[0] * 1.6f, 0.0f, 1.0f);
    if (m_rawConfidence < 0.08f) return;   // too aperiodic to trust

    // Tempo hysteresis: small drifts are followed smoothly; jumps need repeated agreement.
    if (m_period <= 0.0f) {
        m_period = period;
    } else if (std::fabs(period / m_period - 1.0f) < 0.05f) {
        m_period += 0.25f * (period - m_period);
        m_candidateVotes = 0;
    } else {
        if (m_candidateVotes > 0 && std::fabs(period / m_candidatePeriod - 1.0f) < 0.05f) ++m_candidateVotes;
        else { m_candidatePeriod = period; m_candidateVotes = 1; }
        if (m_candidateVotes >= 3) {   // ~0.75 s of consistent evidence
            m_period = m_candidatePeriod;
            m_candidateVotes = 0;
        }
    }

    // Phase: which offset (ticks since the last beat) lines up best with recent onsets?
    float bestOffset = 0.0f, bestScore = -1.0f;
    for (float offset = 0.0f; offset < m_period; offset += 0.5f) {
        float s = 0.0f, w = 1.0f;
        for (int k = 0; k < kCombBeats; ++k, w *= 0.85f) s += w * EnvelopeAt(offset + k * m_period, m_lowEnvelope);
        if (s > bestScore) { bestScore = s; bestOffset = offset; }
    }
    const float measured = Wrap01(bestOffset / m_period);
    const float error = WrapHalf(measured - m_phase);
    m_phase += 0.35f * error;   // pull the oscillator towards the measured phase
    if (m_phase < 0.0f) m_phase += 1.0f;
    if (m_phase >= 1.0f) { m_phase -= 1.0f; ++m_state.beatIndex; m_barBeat = (m_barBeat + 1) & 3; }
}
