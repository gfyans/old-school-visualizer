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
// Beat chains (a dynamic Bayesian beat tracker, after Krebs, Boeck & Widmer 2015, decoded online).
constexpr float kChainTransLambda = 100.0f;  // tempo change cost per beat: exp(-lambda * |ratio - 1|)
constexpr float kChainObsLambda = 16.0f;     // onset odds on vs off the beat
constexpr float kChainOnsetScale = 8.0f;     // an onset this many times the recent mean reads as 0.5
constexpr float kChainObsWeight = 3.0f;      // weight of each beat's observation against the costs
// Each beat is observed through a 3-tick window (the beat itself is its centre, position 1),
// whatever the tempo: onsets here are 1-2 tick spikes, so a window that grew with the period
// (madmom's 1/16) made slow tempos pay for empty ticks.
constexpr int kChainBeatTicks = 3;
// Rewards and tempo preferences are per beat, so they're scaled by period / this: per second,
// every tempo whose beats land on onsets then earns the same, and faster levels win nothing just
// for having more beats.
constexpr float kChainRefPeriod = 50.0f;
constexpr float kChainPriorWeight = 0.5f;    // per-beat pull towards ~120 BPM, until the first estimate
// Per beat, log(the tempo's autocorrelation score / the best's) times this. Without it the chains
// favour the fastest level whose beats all land on pulses (they collect the most onsets); the
// autocorrelation, which also checks the beat repeats at 2x and prefers ~120 BPM, picks the level.
constexpr float kChainLevelWeight = 3.0f;
constexpr float kChainStickMargin = 4.0f;    // log score a rival chain must win by to move the grid...
constexpr int kChainStickVotes = 4;          // ...for this many estimates in a row (1 s)
// A rival at another tempo (usually another metrical level: 3/4, 3/2, 2x...) needs far more: in
// dense music the accents drift between readings for a few seconds at a time.
constexpr float kChainLevelMargin = 25.0f;   // (a real tempo change gains ~8 a beat, drift far less)
constexpr int kChainLevelVotes = 12;         // 3 s
constexpr int kChainSettleEstimates = 40;    // 10 s after the first lock before level switches get sticky

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

    // Beat chains: every chain starts equally likely.
    int states = 0;
    for (int i = 0; i < kChainTempi; ++i) {
        m_chainStart[i] = states;
        m_chainHead[i] = 0;
        states += kMinChainPeriod + i;
        const float bpm = 60.0f * kEnvelopeRate / (kMinChainPeriod + i);
        const float octaves = std::log2(bpm / 120.0f);
        m_chainPrior[i] = -kChainPriorWeight * 0.5f * octaves * octaves * (kMinChainPeriod + i) / kChainRefPeriod;
    }
    m_chain.assign(states, 0.0f);
    m_chainTrans.assign(kChainTempi * kChainTempi, 0.0f);
    for (int from = 0; from < kChainTempi; ++from) {
        float* row = &m_chainTrans[from * kChainTempi];
        float sum = 0.0f;
        for (int to = 0; to < kChainTempi; ++to) {
            const float ratio = float(kMinChainPeriod + to) / float(kMinChainPeriod + from);
            row[to] = -kChainTransLambda * std::fabs(ratio - 1.0f);
            sum += std::exp(row[to]);
        }
        const float logSum = std::log(sum);
        for (int to = 0; to < kChainTempi; ++to) row[to] -= logSum;
    }
    m_chainOnsetMean = 0.0f;
    std::fill(std::begin(m_chainRecent), std::end(m_chainRecent), 0.0f);
    m_chainSwitchVotes = 0;
    m_followed = {};
    m_lockedEstimates = 0;
}

void BeatTracker::ChainsStep(float onset) {
    // Onset as a probability-like 0..1 (soft-saturating against the recent average). Loose enough
    // that a kick still outscores an off-beat hat: saturate early and every onset looks the same.
    m_chainOnsetMean += (onset - m_chainOnsetMean) * 0.005f;   // ~2 s
    // Each beat gets one observation: the strongest onset in its 3-tick window (centre favoured, so
    // ties don't bias the timing). Scoring every tick of the window instead let a hat smeared over
    // two ticks outscore a bigger, sharper kick.
    m_chainRecent[2] = m_chainRecent[1];
    m_chainRecent[1] = m_chainRecent[0];
    m_chainRecent[0] = onset;
    const float pooled = std::max({ 0.9f * m_chainRecent[0], m_chainRecent[1], 0.9f * m_chainRecent[2] });
    const float o = pooled / (pooled + kChainOnsetScale * m_chainOnsetMean + 1e-6f);

    // A beat ends: each period's last position can carry on at any period (a new beat starts).
    float last[kChainTempi];
    for (int j = 0; j < kChainTempi; ++j) {
        const int p = kMinChainPeriod + j;
        last[j] = m_chain[m_chainStart[j] + (m_chainHead[j] + p - 1) % p];
    }
    for (int i = 0; i < kChainTempi; ++i) {
        float best = -1e30f;
        for (int j = 0; j < kChainTempi; ++j) best = std::max(best, last[j] + m_chainTrans[j * kChainTempi + i]);
        const int p = kMinChainPeriod + i;
        m_chainHead[i] = (m_chainHead[i] + p - 1) % p;   // every position moves on one; the old last becomes 0
        m_chain[m_chainStart[i] + m_chainHead[i]] = best + m_chainPrior[i];
    }

    // Observation: a beat explains an onset, the rest of the beat explains its absence. (Adding the
    // off-beat term to every state changes nothing, so only the beat's own observation moves.) It
    // lands as the window closes, on position 2, for the window centred on position 1: the beat.
    // (The 0.02 floors keep a quiet beat from costing much more than an onset earns.)
    const float onBeat = kChainObsWeight * (std::log(o + 0.02f) - std::log((1.0f - o) / (kChainObsLambda - 1.0f) + 0.02f));
    for (int i = 0; i < kChainTempi; ++i) {
        const int p = kMinChainPeriod + i;
        m_chain[m_chainStart[i] + (m_chainHead[i] + kChainBeatTicks - 1) % p] += onBeat * p / kChainRefPeriod;
    }
}

BeatTracker::Chain BeatTracker::BestChain() {
    // Near-ties are common in dense or syncopated music (four equal pulses a beat, or a gallop
    // that fits two tempos), and following the global best would hop between them. So keep
    // following the chain followed last unless a rival clearly and lastingly beats it.
    const bool following = m_followed.period > 0;
    const float expected = following ? Wrap01(float(m_followed.position + kEstimateEvery) / m_followed.period) : 0.0f;
    Chain global, local, level;   // best overall; best continuing the followed chain; best at its tempo
    float globalScore = -1e30f, localScore = -1e30f, levelScore = -1e30f;
    for (int i = 0; i < kChainTempi; ++i) {
        const int p = kMinChainPeriod + i;
        const bool sameTempo = following && std::fabs(float(p) / m_followed.period - 1.0f) < 0.06f;
        for (int k = 0; k < p; ++k) {
            const float s = m_chain[m_chainStart[i] + (m_chainHead[i] + k) % p];
            if (s > globalScore) { globalScore = s; global = { p, k }; }
            if (!sameTempo) continue;
            if (s > levelScore) { levelScore = s; level = { p, k }; }
            if (s > localScore && std::fabs(WrapHalf(float(k) / p - expected)) < 0.15f) { localScore = s; local = { p, k }; }
        }
    }
    for (float& s : m_chain) s -= globalScore;   // keep the scores near zero
    localScore -= globalScore;
    levelScore -= globalScore;

    Chain choice = global;
    if (following) {
        if (local.period == 0) { local = level; localScore = levelScore; }   // lost the phase: best at this tempo
        // A rival at another tempo (usually another metrical level) only needs the larger margin
        // once the grid has settled: the first lock is made before the level preference is known.
        const bool otherLevel = std::fabs(float(global.period) / m_followed.period - 1.0f) >= 0.06f;
        const bool sticky = otherLevel && m_lockedEstimates >= kChainSettleEstimates;
        const float margin = sticky ? kChainLevelMargin : kChainStickMargin;
        const int votes = sticky ? kChainLevelVotes : kChainStickVotes;
        choice = local;
        if (-localScore < margin) m_chainSwitchVotes = 0;
        else if (++m_chainSwitchVotes >= votes) { m_chainSwitchVotes = 0; choice = global; }
    }
    m_followed = choice;
    ++m_lockedEstimates;
    return choice;
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
    ChainsStep(onset);

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
    const float stability = m_chainSwitchVotes == 0 ? 1.0f : 0.7f;   // a rival chain is gaining
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
    const Chain chain = BestChain();   // (always: it also renormalises the chain scores)

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
    if (ac[0] < 1e-9f) {   // silence / no onsets: the next music locks afresh
        m_rawConfidence = 0.0f;
        m_followed = {};
        m_lockedEstimates = 0;
        return;
    }

    // Score each candidate period: its autocorrelation plus its double (a true beat period
    // also repeats at 2x), weighted by a broad preference for ~120 BPM. This is the chains'
    // metrical-level preference.
    auto raw = [&](int lag) { return ac[lag] + 0.5f * (2 * lag <= lagTop ? ac[2 * lag] : 0.0f); };
    float score[2 * 100 + 4] = {};
    int best = lagMin;
    for (int lag = lagMin; lag <= lagMax; ++lag) {
        const float bpm = 60.0f * kEnvelopeRate / lag;
        const float octaves = std::log2(bpm / 120.0f);
        score[lag] = raw(lag) / ac[0] * std::exp(-0.5f * octaves * octaves);
        if (score[lag] > score[best]) best = lag;
    }
    for (int i = 0; i < kChainTempi; ++i) {
        const float ratio = score[kMinChainPeriod + i] / std::max(score[best], 1e-9f);
        m_chainPrior[i] = kChainLevelWeight * std::log(std::clamp(ratio, 0.02f, 1.0f)) * (kMinChainPeriod + i) / kChainRefPeriod;
    }
    const float periodicity = std::max(0.0f, ac[best] / ac[0]);
    m_state.periodicity = periodicity;
    if (periodicity < 0.05f) { m_rawConfidence = 0.0f; return; }   // too aperiodic to trust

    // Tempo: the beat chain picks the metrical level (and is sticky about it); the autocorrelation
    // refines it to a fractional period, so the published BPM is smooth and exact.
    int peak = std::clamp(chain.period, lagMin + 1, lagMax - 1);
    for (int lag = std::max(lagMin + 1, chain.period - 2); lag <= std::min(lagMax - 1, chain.period + 2); ++lag)
        if (raw(lag) > raw(peak)) peak = lag;
    float chainPeriod = static_cast<float>(peak);
    {
        const float a = raw(peak - 1), b = raw(peak), c = raw(peak + 1);
        const float denom = a - 2.0f * b + c;
        if (denom < 0.0f) chainPeriod += 0.5f * (a - c) / denom;
    }
    if (std::fabs(chainPeriod - chain.period) > 1.5f) chainPeriod = static_cast<float>(chain.period);
    const bool firstLock = m_period <= 0.0f;
    if (firstLock) m_period = chainPeriod;
    else if (std::fabs(chainPeriod / m_period - 1.0f) < 0.05f) m_period += 0.25f * (chainPeriod - m_period);
    else m_period = chainPeriod;   // the chain has already been sticky about this move

    // Comb contrast (diagnostic): how much the best beat position stands out from the others.
    float bestScore = -1.0f, scoreSum = 0.0f;
    int offsets = 0;
    for (float offset = 0.0f; offset < m_period; offset += 0.5f) {
        float s = 0.0f, w = 1.0f;
        for (int k = 0; k < kCombBeats; ++k, w *= 0.85f) s += w * EnvelopeAt(offset + k * m_period);
        scoreSum += s;
        ++offsets;
        bestScore = std::max(bestScore, s);
    }
    const float meanScore = scoreSum / std::max(offsets, 1);
    m_state.contrast = meanScore > 1e-9f ? bestScore / meanScore - 1.0f : 0.0f;

    // An onset only shows up in the envelope a little after it happened (it has to get well
    // inside the analysis window), so beats are really kLatencyTicks further back than they look.
    // Measured on real recordings with tools/analysis: uncorrected, grids ran 30-40 ms late.
    const float measured = Wrap01((chain.position - 1 + kLatencyTicks) / chain.period);   // (position 1 is the beat)
    if (firstLock) m_phase = measured;   // start on the chain rather than easing in from nowhere
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
