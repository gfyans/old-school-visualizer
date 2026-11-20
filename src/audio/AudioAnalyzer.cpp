#include "audio/AudioAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kMinFreq = 30.0f, kMaxFreq = 16000.0f;
constexpr float kRangeDb = 45.0f;      // dynamic range shown below the auto-gain reference
constexpr float kFloorDb = -50.0f;     // auto-gain never boosts beyond this (keeps silence dark)
constexpr float kStructureWarmUp = 4.0f;   // seconds of audio before build-ups/drops are judged
constexpr float kRhythmMemory = 15.0f;     // how long drums keep structure detection on
constexpr float kDrumsPresent = 0.36f;     // percussiveness above this = drums are playing

// Exponential approach with separate rise/fall time constants (seconds); frame-rate independent.
float Smooth(float current, float target, float dt, float attack, float release) {
    const float tau = target > current ? attack : release;
    return target + (current - target) * std::exp(-dt / tau);
}

float ToDb(float power) { return 10.0f * std::log10(power + 1e-12f); }

} // namespace

AudioAnalyzer::AudioAnalyzer() {
    const std::vector<float> hann = Fft::Hann(kFftSize);
    std::copy(hann.begin(), hann.end(), m_window);
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
    // Onset beats look at the kick-drum range.
    m_fluxLo = std::max(1, int(40.0f / binHz));
    m_fluxHi = std::max(m_fluxLo + 1, int(180.0f / binHz) + 1);
    // Structure detection: bass (to 200 Hz) and the "riser" range (2–12 kHz).
    m_bassBinHi = std::max(2, int(200.0f / binHz));
    m_highBinLo = std::max(1, int(2000.0f / binHz));
    m_highBinHi = std::min(kBins, std::max(m_highBinLo + 1, int(12000.0f / binHz)));
}

void AudioAnalyzer::ProcessStream(const float* samples, size_t count, float sampleRate) {
    m_beats.Process(samples, count, sampleRate);
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
    m_fftEngine.Transform(m_fft);

    const float norm = 4.0f / kFftSize;   // full-scale sine -> magnitude 1 with a Hann window
    for (int k = 0; k < kBins; ++k) m_power[k] = std::norm(m_fft[k]) * norm * norm;

    float bandDb[kBands];
    float peakDb = -200.0f;
    for (int b = 0; b < kBands; ++b) {
        double p = 0.0;
        for (int k = m_bandLo[b]; k < m_bandHi[b]; ++k) p += m_power[k];
        p /= (m_bandHi[b] - m_bandLo[b]);
        bandDb[b] = ToDb(float(p)) + m_bandTilt[b];
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

    // --- Onset beats: spectral flux in the kick range vs adaptive threshold ---
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

    // --- Beat grid ---
    const BeatTracker::State& grid = m_beats.GetState();
    f.bpm = grid.bpm;
    f.beatPhase = grid.beatPhase;
    f.barPhase = grid.barPhase;
    f.gridBeat = grid.beatIndex;
    f.percussive = grid.percussive;
    // Without drums a "beat" is at best a soft pulse: don't let visuals lock onto it hard.
    const float drumFactor = std::clamp((grid.percussive - 0.22f) / 0.14f, 0.0f, 1.0f);
    f.tempoConfidence = f.silent ? 0.0f : grid.confidence * drumFactor;

    // --- Structure & waveform ---
    double bassP = 0, highP = 0, totalP = 0;
    for (int k = 1; k < m_bassBinHi; ++k) bassP += m_power[k];
    for (int k = m_highBinLo; k < m_highBinHi; ++k) highP += m_power[k];
    for (int k = 1; k < kBins; ++k) totalP += m_power[k];
    UpdateStructure(float(bassP), float(highP), float(totalP), dt);
    UpdateWaveform(samples, sampleRate, dt);
}

void AudioAnalyzer::UpdateWaveform(const float* samples, float sampleRate, float dt) {
    // Oscilloscope-style: start at a rising zero crossing so the shape stands still.
    const int span = std::clamp(static_cast<int>(sampleRate * 0.02f), 128, kFftSize / 2);
    int start = kFftSize - span;
    for (int i = kFftSize - span - 1; i > kFftSize - 2 * span && i > 0; --i) {
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f) { start = i; break; }
    }
    float peak = 0.0f;
    for (int i = 0; i < span; ++i) peak = std::max(peak, std::fabs(samples[start + i]));
    m_wavePeak = std::max({ peak, m_wavePeak * std::exp(-dt / 1.5f), 1e-3f });

    const float per = static_cast<float>(span) / AudioFeatures::kWaveformPoints;
    for (int p = 0; p < AudioFeatures::kWaveformPoints; ++p) {
        const int a = start + static_cast<int>(p * per);
        const int b = std::max(a + 1, start + static_cast<int>((p + 1) * per));
        float sum = 0.0f;
        for (int i = a; i < b; ++i) sum += samples[i];
        m_features.waveform[p] = std::clamp(sum / (b - a) / m_wavePeak, -1.0f, 1.0f);
    }
}

void AudioAnalyzer::UpdateStructure(float bassPower, float highPower, float totalPower, float dt) {
    AudioFeatures& f = m_features;
    const float bassDb = ToDb(bassPower), highDb = ToDb(highPower), levelDb = ToDb(totalPower);
    if (!m_structureInit) {
        m_structureInit = true;
        m_bassShort = m_bassRef = bassDb;
        m_highShort = highDb;
        m_levelFast = m_levelSlow = levelDb;
    }
    f.drop *= std::exp(-dt / 1.0f);
    m_dropCooldown = std::max(0.0f, m_dropCooldown - dt);

    if (f.silent) {   // nothing to analyse; don't let silence look like "bass removed"
        m_bassAbsentTime = 0.0f;
        m_lastBassAbsent = 0.0f;
        m_activeTime = 0.0f;
        m_structureInit = false;   // the next song starts with fresh baselines
        m_highCount = 0;
        f.buildUp = Smooth(f.buildUp, 0.0f, dt, 1.0f, 0.5f);
        return;
    }
    const bool warmUpEnds = m_activeTime <= kStructureWarmUp && m_activeTime + dt > kStructureWarmUp;
    m_activeTime += dt;
    if (warmUpEnds) {
        // Start judging from fresh baselines: forget the intro / fade-in entirely.
        m_structureInit = false;
        m_highCount = 0;
        m_bassAbsentTime = m_lastBassAbsent = 0.0f;
    }

    m_bassShort = Smooth(m_bassShort, bassDb, dt, 0.08f, 0.15f);
    m_bassRef = Smooth(m_bassRef, m_bassShort, dt, 0.5f, 20.0f);   // the song's usual bass level
    m_highShort = Smooth(m_highShort, highDb, dt, 0.3f, 0.3f);
    m_levelFast = Smooth(m_levelFast, levelDb, dt, 0.05f, 0.1f);
    m_levelSlow = Smooth(m_levelSlow, levelDb, dt, 2.0f, 2.0f);

    // Slope of the high band over the last 4 s (dB per second) via least squares.
    m_historyTimer += dt;
    while (m_historyTimer >= 0.1f) {
        m_historyTimer -= 0.1f;
        m_highHistory[m_highWrite] = m_highShort;
        m_highWrite = (m_highWrite + 1) % 40;
        m_highCount = std::min(m_highCount + 1, 40);
    }
    float slope = 0.0f;
    if (m_highCount >= 20) {
        const int n = m_highCount;
        float sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; ++i) {
            const float x = i * 0.1f;
            const float y = m_highHistory[(m_highWrite - n + i + 40) % 40];
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        slope = (n * sxy - sx * sy) / std::max(n * sxx - sx * sx, 1e-6f);
    }

    const bool bassAbsent = m_bassShort < m_bassRef - 9.0f;
    m_bassAbsentTime = bassAbsent ? m_bassAbsentTime + dt : 0.0f;

    // Build-ups and drops only exist in music with drums. Remember drums for 15 s, which
    // comfortably covers a beatless breakdown before a drop, but keeps piano crescendos and
    // ambient swells from reading as builds and drops. (Calibrated with tools/analysis:
    // drum-driven songs sit at 0.38-0.60, solo piano at <= 0.28.)
    m_rhythmHold = f.percussive > kDrumsPresent ? kRhythmMemory : std::max(0.0f, m_rhythmHold - dt);

    // Warm-up: the first seconds of a song (often a fade-in) would otherwise read as a
    // rising build followed by a jump. Let the baselines settle before judging structure.
    const bool warmedUp = m_activeTime > kStructureWarmUp && m_rhythmHold > 0.0f;

    // Build-up: the highs are steadily rising (risers, snare rolls), more so with the bass cut.
    float build = warmedUp ? std::clamp((slope - 0.5f) / 3.0f, 0.0f, 1.0f) * (bassAbsent ? 1.0f : 0.5f) : 0.0f;
    f.buildUp = Smooth(f.buildUp, build, dt, 1.0f, 0.6f);

    // Drop: the bass slams back in after being away, or a big jump in level after a build.
    const bool bassReturn = !bassAbsent && m_lastBassAbsent >= 1.5f;
    const bool levelJump = f.buildUp > 0.3f && !bassAbsent && m_levelFast > m_levelSlow + 6.0f;
    if (warmedUp && (bassReturn || levelJump) && m_dropCooldown <= 0.0f) {
        f.drop = 1.0f;
        ++f.dropCount;
        f.buildUp = 0.0f;
        m_dropCooldown = 6.0f;
    }
    m_lastBassAbsent = m_bassAbsentTime;
}
