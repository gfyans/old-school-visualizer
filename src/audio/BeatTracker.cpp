#include "audio/BeatTracker.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMinBpm = 65.0f, kMaxBpm = 185.0f;
constexpr int kTempoWindow = 500;        // 5 s of envelope for autocorrelation
constexpr int kEstimateEvery = 25;       // re-estimate tempo/phase every 0.25 s
constexpr int kCombBeats = 8;            // beats looked back over when locating phase
// Confidence comes mainly from phase consistency (average |phase error| per estimate),
// calibrated on real recordings with tools/analysis: rhythmic songs well above 0.4,
// drumless / rubato music below.
constexpr float kPhaseErrorGood = 0.06f;   // at or below: fully confident
constexpr float kPhaseErrorBad = 0.20f;    // at or above: no confidence (random is ~0.25)
constexpr float kPeriodicityFloor = 0.08f; // below this the onsets barely repeat at all
constexpr float kLatencyTicks = 2.0f;      // onset detection delay, in 10 ms envelope ticks

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
    // Log-spaced bands (30 Hz - 11 kHz), each at least one FFT bin wide.
    const float binHz = sampleRate / windowSize;
    const float top = std::min(11000.0f, sampleRate * 0.45f);
    int prevHi = 1;
    for (int b = 0; b <= kBands; ++b) {
        const float f = 30.0f * std::pow(top / 30.0f, float(b) / kBands);
        const int bin = std::clamp(static_cast<int>(f / binHz), 1, windowSize / 2);
        m_bandEdge[b] = b == 0 ? bin : std::max(bin, prevHi + 1);
        prevHi = m_bandEdge[b];
    }
    m_prevBand.fill(0.0f);
    m_prevFine.fill(0.0f);
    m_bandPeak = 0.0f;
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

    // Onset strength pooled over log-spaced bands (like a mel onset envelope): each band
    // counts once however many FFT bins it spans, so broadband hats can't swamp the kick,
    // and a bassline alone can't drag the grid onto the off-beats.
    const float norm = 4.0f / n;
    float bands[kBands];
    float loudest = 0.0f;
    for (int b = 0; b < kBands; ++b) {
        float sum = 0.0f;
        for (int k = m_bandEdge[b]; k < m_bandEdge[b + 1]; ++k) sum += std::abs(m_spectrum[k]);
        bands[b] = sum * norm / (m_bandEdge[b + 1] - m_bandEdge[b]);
        loudest = std::max(loudest, bands[b]);
    }
    m_bandPeak = std::max(loudest, m_bandPeak * 0.9995f);   // slow-decaying reference level
    // Floor at -40 dB below the loudest band: a sound rising out of near-silence (a hat after a
    // gap) then counts as a bounded jump instead of an enormous one that outweighs the kick.
    const float floor = 1e-2f * m_bandPeak + 1e-9f;
    const float fineFloor = 1e-3f * m_bandPeak + 1e-9f;      // finer floor for the percussiveness count
    float flux = 0.0f;
    int jumping = 0;
    for (int b = 0; b < kBands; ++b) {
        const float logBand = std::log(bands[b] + floor);
        const float d = logBand - m_prevBand[b];
        m_prevBand[b] = logBand;
        if (d > 0.0f) flux += d;   // equal weight: snares and hats mark the beat as much as kicks
        const float fine = std::log(bands[b] + fineFloor);
        if (fine - m_prevFine[b] > 0.4f) ++jumping;   // band got ~50% louder in 10 ms
        m_prevFine[b] = fine;
    }
    flux /= kBands;

    // Percussiveness: drum hits make many bands jump at once; tonal notes light up a few.
    // Take the broadest jump in each 100 ms, averaged over a few seconds.
    m_broadestJump = std::max(m_broadestJump, float(jumping) / kBands);
    if (m_hopCount % 10 == 9) {
        m_percussive += (m_broadestJump - m_percussive) * 0.03f;   // ~3 s
        m_broadestJump = 0.0f;
    }
    m_state.percussive = m_percussive;

    // Remove the slowly varying baseline so only onsets remain.
    m_onsetMean += (flux - m_onsetMean) * 0.01f;   // ~1 s
    const float onset = std::max(0.0f, flux - m_onsetMean);
    m_lastOnset = onset;
    m_envelope[m_envWrite] = onset;
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

float BeatTracker::EnvelopeAt(float ticksAgo) const {
    if (ticksAgo < 0.0f) ticksAgo = 0.0f;
    const int i0 = static_cast<int>(ticksAgo);
    const float f = ticksAgo - i0;
    auto at = [&](int ago) { return m_envelope[((m_envWrite - 1 - ago) % kEnvelopeSize + kEnvelopeSize) % kEnvelopeSize]; };
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
    // Parabolic interpolation for a fractional period, on the unweighted autocorrelation (the
    // tempo preference picks the peak, but would pull its exact position towards 120 BPM).
    float period = static_cast<float>(best);
    if (best > lagMin && best < lagMax) {
        auto raw = [&](int lag) { return ac[lag] + 0.5f * (2 * lag <= lagTop ? ac[2 * lag] : 0.0f); };
        const float a = raw(best - 1), b = raw(best), c = raw(best + 1);
        const float denom = a - 2.0f * b + c;
        if (denom < 0.0f) period += 0.5f * (a - c) / denom;
    }
    const float periodicity = std::max(0.0f, ac[best] / ac[0]);
    m_state.periodicity = periodicity;
    if (periodicity < 0.05f) { m_rawConfidence = 0.0f; return; }   // too aperiodic to trust

    // Tempo hysteresis: small drifts are followed smoothly. A different tempo must win for
    // ~1.5 s *and* clearly outscore the current one (real music briefly favours 4/3, 3/2...).
    const int currentLag = static_cast<int>(std::lround(m_period));
    const float currentScore = (currentLag >= lagMin && currentLag <= lagMax) ? score[currentLag] : 0.0f;
    if (m_period <= 0.0f) {
        m_period = period;
    } else if (std::fabs(period / m_period - 1.0f) < 0.05f) {
        m_period += 0.25f * (period - m_period);
        m_candidateVotes = 0;
    } else if (score[best] > (m_state.confidence > 0.5f ? 1.3f : 1.1f) * currentScore) {
        // (A confidently locked tempo needs a much clearer winner before it moves.)
        if (m_candidateVotes > 0 && std::fabs(period / m_candidatePeriod - 1.0f) < 0.05f) ++m_candidateVotes;
        else { m_candidatePeriod = period; m_candidateVotes = 1; }
        if (m_candidateVotes >= 6) {
            m_period = m_candidatePeriod;
            m_candidateVotes = 0;
        }
    } else {
        m_candidateVotes = 0;   // the current tempo is still competitive: keep it
    }

    // Phase: which offset (ticks since the last beat) lines up best with recent onsets?
    float bestOffset = 0.0f, bestScore = -1.0f, scoreSum = 0.0f;
    int offsets = 0;
    for (float offset = 0.0f; offset < m_period; offset += 0.5f) {
        float s = 0.0f, w = 1.0f;
        for (int k = 0; k < kCombBeats; ++k, w *= 0.85f) s += w * EnvelopeAt(offset + k * m_period);
        scoreSum += s;
        ++offsets;
        if (s > bestScore) { bestScore = s; bestOffset = offset; }
    }
    // Comb contrast (diagnostic): how much the best beat position stands out from the others.
    const float meanScore = scoreSum / std::max(offsets, 1);
    m_state.contrast = meanScore > 1e-9f ? bestScore / meanScore - 1.0f : 0.0f;

    // An onset only shows up in the envelope a little after it happened (it has to get well
    // inside the analysis window), so beats are really kLatencyTicks further back than they look.
    // Measured on real recordings with tools/analysis: uncorrected, grids ran 30-40 ms late.
    const float measured = Wrap01((bestOffset + kLatencyTicks) / m_period);
    const float error = WrapHalf(measured - m_phase);

    // Confidence from phase consistency: with a real beat, each new measurement agrees with
    // where the oscillator already is; without one (rubato piano, ambient) the "best" beat
    // position wanders randomly and the average error tends towards 0.25 of a beat.
    // Falls faster than it rises: once the beats start agreeing (e.g. drums arrive after an
    // intro) it locks within a couple of seconds, but a wobbly grid still loses trust at once.
    const float errorRate = std::fabs(error) < m_phaseErrorAvg ? 0.18f : 0.1f;
    m_phaseErrorAvg += (std::fabs(error) - m_phaseErrorAvg) * errorRate;
    m_state.phaseError = m_phaseErrorAvg;
    m_rawConfidence = std::clamp((kPhaseErrorBad - m_phaseErrorAvg) / (kPhaseErrorBad - kPhaseErrorGood), 0.0f, 1.0f) *
                      std::clamp(periodicity / kPeriodicityFloor, 0.0f, 1.0f);
    m_phase += 0.35f * error;   // pull the oscillator towards the measured phase
    if (m_phase < 0.0f) m_phase += 1.0f;
    if (m_phase >= 1.0f) { m_phase -= 1.0f; ++m_state.beatIndex; m_barBeat = (m_barBeat + 1) & 3; }
}
