#include "audio/AudioAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kMinFreq = 30.0f, kMaxFreq = 16000.0f;
constexpr float kRangeDb = 45.0f;      // dynamic range shown below the auto-gain reference
constexpr float kFloorDb = -50.0f;     // auto-gain never boosts beyond this (keeps silence dark)
constexpr float kStructureWarmUp = 4.0f;   // seconds of audio before build-ups/drops are judged
constexpr float kRhythmMemory = 15.0f;     // how long drums keep structure detection on
constexpr float kDanceRhythmMemory = 60.0f;   // ...in dance music, whose breakdowns run 30-60 s
constexpr float kDrumsPresent = 0.36f;     // percussiveness above this = drums are playing
// Big moments in a big section without a drop: how loud the section must be (drum-driven songs sit at
// intensity 0.4-0.6, solo piano ~0.25) and how long since the last big moment.
constexpr float kPeakIntensity = 0.38f;
constexpr float kPeakGap = 22.0f;          // on a phrase downbeat when the beat grid is locked
constexpr float kPeakGapNoGrid = 30.0f;
constexpr int kPeakPhraseBeats = 16;       // a phrase: 4 bars
constexpr float kDanceOn = 0.4f;           // AudioFeatures::edm above this: dance-music rules for drops
constexpr float kDancePeakGap = 45.0f;     // ...big moments without a drop come this rarely
constexpr int kDancePhraseBeats = 32;      // ...on 8-bar phrase boundaries
// Dance music (AudioFeatures::edm): how alike the four beats of a bar must be (correlation of the
// kick-range profile), how much the kick range must swing within a beat, and how far the bass must
// lead the high mids. (Calibrated with tools/analysis: house and techno repeat 0.8+, swing 3.5-20 dB
// and lead by 0.2-0.6; metal leads by 0.08 at most, even when it kicks on every beat.)
constexpr float kDanceRepeat = 0.55f;
constexpr float kDanceDepthDb = 1.5f;
constexpr float kDanceBassLead = 0.08f;

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
    // Harmony: map each bin to its nearest pitch class (A4 = 440 Hz = MIDI 69).
    for (int k = 0; k < kBins; ++k) {
        const float hz = k * binHz;
        m_binPitch[k] = -1;
        if (hz < 150.0f || hz > 5000.0f) continue;
        const int midi = static_cast<int>(std::lround(12.0f * std::log2(hz / 440.0f) + 69.0f));
        m_binPitch[k] = static_cast<signed char>(((midi % 12) + 12) % 12);
    }
    // Dance music: the kick's fundamental.
    m_subLo = std::clamp(int(40.0f / binHz), 1, kBins - 1);
    m_subHi = std::clamp(int(120.0f / binHz) + 1, m_subLo + 1, kBins);
    // Instruments: snare body and snap, hi-hats, and the vocal range.
    auto band = [&](FluxBand& b, float lo, float hi) {
        b.lo = std::clamp(int(lo / binHz), 1, kBins - 1);
        b.hi = std::clamp(int(hi / binHz) + 1, b.lo + 1, kBins);
    };
    band(m_snareBody, 150.0f, 350.0f);
    band(m_snareNoise, 1500.0f, 5000.0f);
    band(m_hatBand, 7000.0f, 16000.0f);
    m_vocalLo = std::clamp(int(300.0f / binHz), 1, kBins - 1);
    m_vocalHi = std::clamp(int(3400.0f / binHz) + 1, m_vocalLo + 1, kBins);
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
            // How often hits are actually heard (smoothed), to spot half-time grid locks.
            if (m_timeSinceBeat < 1.5f) m_onsetInterval += 0.15f * (m_timeSinceBeat - m_onsetInterval);
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
    // Tempo factor for animation speed: follow the tempo while it's trusted, hold it through
    // breaks, drift back to neutral in silence.
    if (f.silent) {
        f.tempoFactor = Smooth(f.tempoFactor, 1.0f, dt, 6.0f, 6.0f);
    } else if (grid.bpm > 0.0f && grid.confidence > 0.3f) {
        float bpm = grid.bpm;
        if (bpm < 70.0f) bpm *= 2.0f;
        // The grid may lock at half-time (174 BPM drum & bass counted as 87); if hits are heard
        // about twice as often as the grid, the song is fast.
        if (bpm < 110.0f && 60.0f / m_onsetInterval > 1.7f * bpm) bpm *= 2.0f;
        if (bpm > 180.0f) bpm *= 0.5f;
        const float target = std::clamp(std::pow(bpm / 120.0f, 0.85f), 0.7f, 1.4f);
        f.tempoFactor = Smooth(f.tempoFactor, target, dt, 2.5f, 2.5f);
    }

    // After the grid: dance detection reads it, and the vocal detector removes beat-locked pumping.
    UpdateDance(dt);
    UpdateInstruments(newData, dt);
    UpdateHarmony(dt);

    // --- Structure & waveform ---
    double bassP = 0, highP = 0, totalP = 0;
    for (int k = 1; k < m_bassBinHi; ++k) bassP += m_power[k];
    for (int k = m_highBinLo; k < m_highBinHi; ++k) highP += m_power[k];
    for (int k = 1; k < kBins; ++k) totalP += m_power[k];
    UpdateStructure(float(bassP), float(highP), float(totalP), dt);
    UpdateWaveform(samples, sampleRate, dt);
}

void AudioAnalyzer::UpdateInstruments(bool newData, float dt) {
    AudioFeatures& f = m_features;
    m_sinceSnare += dt;
    m_sinceHat += dt;
    f.snare *= std::exp(-dt / 0.15f);
    f.hat *= std::exp(-dt / 0.08f);
    if (f.silent) {
        f.vocal = Smooth(f.vocal, 0.0f, dt, 0.1f, 0.5f);
        f.pad = Smooth(f.pad, 0.0f, dt, 0.1f, 0.5f);
        m_vocalProfileReady = false;
        return;
    }

    if (newData) {
        // Band-limited spectral flux (rise in log magnitude), each with a ~1 s adaptive threshold.
        // Bins far below the loudest band are clamped to a floor, so a faint click in a near-silent
        // band (e.g. the edge of a kick) can't look like a big jump.
        const float floorPower = std::pow(10.0f, (m_referenceDb - 42.0f) / 10.0f);
        float logMag[kBins];
        for (int k = 1; k < kBins; ++k) logMag[k] = 0.5f * std::log(std::max(m_power[k], floorPower));
        auto flux = [&](FluxBand& b) {
            float sum = 0.0f;
            for (int k = b.lo; k < b.hi; ++k) sum += std::max(0.0f, logMag[k] - m_prevInstLog[k]);
            b.value = sum / float(b.hi - b.lo);
        };
        flux(m_snareBody);
        flux(m_snareNoise);
        flux(m_hatBand);
        // Snare / clap: the body and the noisy snap jump together. Hi-hat: a jump up top on its own.
        const bool warm = m_activeTime > 1.0f;   // let the thresholds settle first
        // ...and the band must be audible in the mix, not just a faint click (tilted like the
        // spectrum, +10 dB around 10 kHz, so it compares fairly with the loudest band).
        auto audible = [&](const FluxBand& b, float tiltDb, float belowDb) {
            double p = 0.0;
            for (int k = b.lo; k < b.hi; ++k) p += m_power[k];
            return ToDb(float(p / (b.hi - b.lo))) + tiltDb > m_referenceDb - belowDb;
        };
        const bool snare = warm && m_snareNoise.Spike(2.0f) && m_snareBody.Spike(1.0f) && m_sinceSnare > 0.12f &&
                           audible(m_snareNoise, 4.5f, 30.0f);
        const bool hat = warm && !snare && m_hatBand.Spike(2.0f) && m_sinceHat > 0.05f && audible(m_hatBand, 10.0f, 30.0f);
        if (snare) { f.snare = 1.0f; ++f.snareCount; m_sinceSnare = 0.0f; }
        if (hat) { f.hat = 1.0f; ++f.hatCount; m_sinceHat = 0.0f; }
        const float a = 1.0f - std::exp(-dt / 1.0f);
        for (FluxBand* b : { &m_snareBody, &m_snareNoise, &m_hatBand }) {
            const float d = b->value - b->mean;
            b->mean += a * d;
            b->var += a * (d * d - b->var);
        }
        for (int k = 1; k < kBins; ++k) m_prevInstLog[k] = logMag[k];
    }

    // Vocal range (300-3400 Hz): how tonal it is (spectral flatness: peaky = tonal), how present
    // (share of the total), and how much its loudness moves (voices pulse with syllables at a
    // few per second; pads and held notes stay steady).
    double sum = 0.0, logSum = 0.0, total = 0.0;
    for (int k = m_vocalLo; k < m_vocalHi; ++k) {
        sum += m_power[k];
        logSum += std::log(double(m_power[k]) + 1e-14);
    }
    for (int k = 1; k < kBins; ++k) total += m_power[k];
    const int n = m_vocalHi - m_vocalLo;
    const float flatness = float(std::exp(logSum / n) / (sum / n + 1e-14));
    m_tonal = Smooth(m_tonal, std::clamp((0.25f - flatness) / 0.2f, 0.0f, 1.0f), dt, 0.1f, 0.3f);
    m_vocalPresence = Smooth(m_vocalPresence, std::clamp(float(sum / (total + 1e-14)) * 2.5f, 0.0f, 1.0f), dt, 0.1f, 0.4f);
    // Syllable-rate movement: band-pass the loudness envelope to roughly 3-8 Hz (difference of
    // two smoothers) and measure how much is left. Beats are slower, hi-hats far faster, pads flat.
    float db = ToDb(float(sum / n));
    // Sidechain pumping: dance music ducks everything after each kick, a beat-locked swing of a few
    // per second, right where syllables sit. Learn the vocal range's loudness at each 1/16 of the beat
    // and take that shape out, so pumping pads read as pads (voices don't repeat with the beat).
    if (f.tempoConfidence > 0.4f) {
        const int bin = std::clamp(int(f.beatPhase * kBeatBins), 0, kBeatBins - 1);
        if (!m_vocalProfileReady) std::fill(std::begin(m_vocalBeatProfile), std::end(m_vocalBeatProfile), db);
        m_vocalProfileReady = true;
        m_vocalBeatProfile[bin] += 0.03f * (db - m_vocalBeatProfile[bin]);   // ~4 bars of memory per step
        float mean = 0.0f;
        for (float v : m_vocalBeatProfile) mean += v / kBeatBins;
        db -= f.edm * (m_vocalBeatProfile[bin] - mean);
    }
    if (m_vocalFast < -99.0f) m_vocalFast = m_vocalSlow = db;
    m_vocalFast = Smooth(m_vocalFast, db, dt, 0.02f, 0.02f);   // ~8 Hz
    m_vocalSlow = Smooth(m_vocalSlow, db, dt, 0.055f, 0.055f); // ~3 Hz
    const float band = m_vocalFast - m_vocalSlow;
    m_vocalMod = Smooth(m_vocalMod, band * band, dt, 0.3f, 0.6f);
    const float pulsing = std::clamp((std::sqrt(m_vocalMod) - 0.8f) / 1.6f, 0.0f, 1.0f);
    const float base = m_tonal * m_vocalPresence;
    f.vocal = Smooth(f.vocal, base * pulsing, dt, 0.08f, 0.4f);
    f.pad = Smooth(f.pad, base * (1.0f - pulsing), dt, 0.3f, 0.8f);
}

// Four-on-the-floor: fold the kick range's loudness over the bar (32 steps). In dance music every
// beat of the bar looks the same (a kick, then a dip, often deepened by sidechain ducking), so the
// four quarter-bar profiles match and swing a lot. Rock (kick on 1 and 3, snare on 2 and 4), hip-hop
// (syncopated kicks) and blast beats (no gap between kicks) fail one test or the other.
void AudioAnalyzer::UpdateDance(float dt) {
    AudioFeatures& f = m_features;
    if (f.silent) {
        f.edm = 0.0f;
        m_profileFrames = 0;
        m_kickRepeat = m_kickDepth = 0.0f;
        return;
    }
    // Hold everything while the grid is unsure, the drums are out or the kick and bass have been
    // pulled for a moment (breakdowns and build-ups keep the verdict; a snare roll over a riser is
    // not the groove). Not the gaps between kicks, which deep sidechained grooves dip into.
    if (f.tempoConfidence < 0.4f || f.percussive < kDrumsPresent || m_bassAbsentTime > 0.5f) return;

    // Mix balance: dance music is bass-led; guitar bands (who can also kick on every beat) fill the
    // high mids just as much. Average of bass minus high mids (both auto-gained 0..1) over ~6 s.
    m_bassLead = m_profileFrames == 0 ? f.bass - f.highMid : Smooth(m_bassLead, f.bass - f.highMid, dt, 6.0f, 6.0f);

    double sub = 0.0;
    for (int k = m_subLo; k < m_subHi; ++k) sub += m_power[k];
    const float subDb = ToDb(float(sub / (m_subHi - m_subLo)));
    const int bin = std::clamp(int(f.barPhase * kBarBins), 0, kBarBins - 1);
    if (m_profileFrames == 0) std::fill(std::begin(m_subProfile), std::end(m_subProfile), subDb);
    m_subProfile[bin] += 0.06f * (subDb - m_subProfile[bin]);   // ~4 bars of memory per step
    ++m_profileFrames;

    float repeat = 0.0f, depth = 0.0f;
    constexpr int kQ = kBarBins / 4;
    float centered[4][kQ];
    for (int q = 0; q < 4; ++q) {
        float mean = 0.0f, lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < kQ; ++i) {
            const float v = m_subProfile[q * kQ + i];
            mean += v; lo = std::min(lo, v); hi = std::max(hi, v);
        }
        mean /= kQ;
        for (int i = 0; i < kQ; ++i) centered[q][i] = m_subProfile[q * kQ + i] - mean;
        depth += (hi - lo) * 0.25f;
    }
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            float sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < kQ; ++i) {
                sxy += centered[a][i] * centered[b][i];
                sxx += centered[a][i] * centered[a][i];
                syy += centered[b][i] * centered[b][i];
            }
            repeat += sxy / std::sqrt(sxx * syy + 1e-9f) / 6.0f;
        }
    }
    m_kickRepeat = repeat;
    m_kickDepth = depth;
    // Trust the profile only after a few bars of locked, drum-driven audio (~4 s).
    if (m_profileFrames < 300) return;
    const float target = std::clamp((repeat - kDanceRepeat) / 0.2f, 0.0f, 1.0f) *
                         std::clamp((depth - kDanceDepthDb) / 3.0f, 0.0f, 1.0f) *
                         std::clamp((m_bassLead - kDanceBassLead) / 0.15f, 0.0f, 1.0f);
    f.edm = Smooth(f.edm, target, dt, 3.0f, 8.0f);
}

// Key (Krumhansl-Kessler key profiles against a ~10 s chroma average), chords (major/minor triads
// against a ~0.4 s average, changing only once a new one holds for a moment) and an overall mood.
void AudioAnalyzer::UpdateHarmony(float dt) {
    AudioFeatures& f = m_features;
    if (f.silent) { f.mood = Smooth(f.mood, 0.0f, dt, 2.0f, 2.0f); return; }

    // Chroma from spectral peaks only (a note smears over neighbouring bins, which at these
    // frequencies are wider than a semitone), each peak's frequency refined between bins.
    float chroma[12] = {};
    double total = 0.0, centroid = 0.0, all = 0.0;
    const float binHz = m_configuredRate / kFftSize;
    for (int k = 1; k < kBins; ++k) {
        const float mag = std::sqrt(m_power[k]);
        all += mag;
        centroid += mag * k;
        if (m_binPitch[k] < 0 || k + 1 >= kBins) continue;
        if (m_power[k] <= m_power[k - 1] || m_power[k] < m_power[k + 1]) continue;
        const float la = std::log(m_power[k - 1] + 1e-12f), lb = std::log(m_power[k] + 1e-12f), lc = std::log(m_power[k + 1] + 1e-12f);
        const float denom = la - 2.0f * lb + lc;
        const float offset = std::fabs(denom) > 1e-6f ? std::clamp(0.5f * (la - lc) / denom, -0.5f, 0.5f) : 0.0f;
        const float hz = (k + offset) * binHz;
        const int midi = static_cast<int>(std::lround(12.0f * std::log2(hz / 440.0f) + 69.0f));
        chroma[((midi % 12) + 12) % 12] += mag;
        total += mag;
    }
    // Brightness: spectral centroid, 0 (dark, ~500 Hz) .. 1 (bright, ~4 kHz).
    const float centroidHz = all > 0.0 ? float(centroid / all) * (m_configuredRate / kFftSize) : 0.0f;
    m_brightness = Smooth(m_brightness, std::clamp(std::log2(std::max(centroidHz, 1.0f) / 500.0f) / 3.0f, 0.0f, 1.0f), dt, 1.0f, 1.0f);
    if (total <= 1e-9) return;
    // Tonal frames count fully; noisy (drum-heavy) frames count less.
    const float weight = 0.25f + 0.75f * m_tonal;
    const float aLong = (1.0f - std::exp(-dt / 10.0f)) * weight, aShort = (1.0f - std::exp(-dt / 0.4f)) * weight;
    for (int i = 0; i < 12; ++i) {
        const float c = chroma[i] / float(total);
        m_chromaLong[i] += aLong * (c - m_chromaLong[i]);
        m_chromaShort[i] += aShort * (c - m_chromaShort[i]);
    }

    // Key: correlate the long chroma with each rotation of the major and minor profiles.
    static constexpr float kMajor[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static constexpr float kMinor[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
    auto correlate = [&](const float* profile, int root) {
        float mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += m_chromaLong[i]; my += profile[i]; }
        mx /= 12; my /= 12;
        float sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i) {
            const float x = m_chromaLong[(i + root) % 12] - mx, y = profile[i] - my;
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        return sxy / std::sqrt(sxx * syy + 1e-12f);
    };
    float best = -2, second = -2;
    int bestKey = 0;
    bool bestMinor = false;
    for (int root = 0; root < 12; ++root) {
        for (int m = 0; m < 2; ++m) {
            const float r = correlate(m ? kMinor : kMajor, root);
            if (r > best) { second = best; best = r; bestKey = root; bestMinor = m == 1; }
            else if (r > second) second = r;
        }
    }
    f.key = bestKey;
    f.minorKey = bestMinor;
    f.keyConfidence = Smooth(f.keyConfidence, std::clamp(best * 1.2f - 0.2f, 0.0f, 1.0f) * std::clamp((best - second) * 8.0f, 0.0f, 1.0f),
                             dt, 1.0f, 1.0f);

    // Chord: best matching triad in the short chroma; a change must hold for 0.2 s.
    float bestScore = -1;
    int bestChord = 0;   // root * 2 + minor
    for (int root = 0; root < 12; ++root) {
        for (int m = 0; m < 2; ++m) {
            const float score = m_chromaShort[root] + m_chromaShort[(root + (m ? 3 : 4)) % 12] + m_chromaShort[(root + 7) % 12];
            if (score > bestScore) { bestScore = score; bestChord = root * 2 + m; }
        }
    }
    const int current = f.chordRoot * 2 + (f.chordMinor ? 1 : 0);
    if (bestChord != current && bestScore > 0.42f && m_tonal > 0.2f) {
        if (bestChord != m_chordCandidate) { m_chordCandidate = bestChord; m_chordHold = 0.0f; }
        m_chordHold += dt;
        if (m_chordHold > 0.2f) {
            f.chordRoot = bestChord / 2;
            f.chordMinor = (bestChord & 1) != 0;
            ++f.chordCount;
            m_chordCandidate = -1;
        }
    } else {
        m_chordCandidate = -1;
    }

    // Mood: major/minor (as far as we're sure), tempo, how big the section is and how bright it sounds.
    // Most dance music is in a minor key however euphoric it is, so there the key counts for less.
    const float majorness = (f.minorKey ? -1.0f : 1.0f) * f.keyConfidence * (1.0f - 0.6f * f.edm);
    const float target = std::clamp(1.6f * (0.45f * majorness + 0.9f * (f.tempoFactor - 1.0f) + 0.6f * (f.intensity - 0.45f) + 0.4f * (m_brightness - 0.5f)),
                                    -1.0f, 1.0f);
    f.mood = Smooth(f.mood, target, dt, 3.0f, 3.0f);
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

void AudioAnalyzer::UpdateScope(const float* mono, const float* side, size_t count, float sampleRate, float dt) {
    constexpr int kPoints = AudioFeatures::kScopePoints;
    if (count < 2 * static_cast<size_t>(kPoints) || sampleRate <= 0.0f) return;
    const int n = static_cast<int>(count);
    const int span = std::clamp(static_cast<int>(sampleRate * 0.025f), kPoints, n / 2);

    // Trigger on a rising zero crossing (like a real scope) so steady waveforms stand still.
    int start = n - span;
    for (int i = n - span - 1; i > n - 2 * span && i > 0; --i) {
        if (mono[i - 1] < 0.0f && mono[i] >= 0.0f) { start = i; break; }
    }

    // Box-filter down to kPoints (no aliasing), tracking the peak for auto-scaling.
    float peak = 0.0f;
    float mid[kPoints], sid[kPoints];
    const float per = static_cast<float>(span) / kPoints;
    for (int p = 0; p < kPoints; ++p) {
        const int a = start + static_cast<int>(p * per);
        const int b = std::max(a + 1, start + static_cast<int>((p + 1) * per));
        float sm = 0.0f, ss = 0.0f;
        for (int i = a; i < b; ++i) { sm += mono[i]; ss += side[i]; }
        mid[p] = sm / (b - a);
        sid[p] = ss / (b - a);
        peak = std::max(peak, std::fabs(mid[p]) + std::fabs(sid[p]));
    }
    m_scopePeak = std::max({ peak, m_scopePeak * std::exp(-dt / 1.5f), 2e-3f });
    const float scale = 0.9f / m_scopePeak;
    for (int p = 0; p < kPoints; ++p) {
        m_features.scopeMid[p] = mid[p] * scale;
        m_features.scopeSide[p] = sid[p] * scale;
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
        m_sinceHit = 0.0f;   // a new song gets the full gap before a phrase hit
        m_buildPeak = 0.0f;
        m_phraseBeats = 0;
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
    // The song's usual bass level. In dance music, hold it while the bass is pulled out for a
    // breakdown (they run 30-60 s): otherwise it sinks towards the breakdown's level, the riser's
    // rumble reads as the bass coming back, and the drop fires early.
    const bool holdBassRef = f.edm > kDanceOn && m_bassShort < m_bassRef - 7.5f;
    if (!holdBassRef) m_bassRef = Smooth(m_bassRef, m_bassShort, dt, 0.5f, 20.0f);
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

    const bool bassAbsent = m_bassShort < m_bassRef - 7.5f;
    m_bassAbsentTime = bassAbsent ? m_bassAbsentTime + dt : 0.0f;

    // Build-ups and drops only exist in music with drums. Remember drums for 15 s, which
    // comfortably covers a beatless breakdown before a drop, but keeps piano crescendos and
    // ambient swells from reading as builds and drops. (Calibrated with tools/analysis:
    // drum-driven songs sit at 0.38-0.60, solo piano at <= 0.28.)
    // Dance music's breakdowns run far longer, so there the memory stretches to a minute.
    m_rhythmHold = f.percussive > kDrumsPresent ? kRhythmMemory + (kDanceRhythmMemory - kRhythmMemory) * f.edm
                                                : std::max(0.0f, m_rhythmHold - dt);

    // Warm-up: the first seconds of a song (often a fade-in) would otherwise read as a
    // rising build followed by a jump. Let the baselines settle before judging structure.
    const bool warmedUp = m_activeTime > kStructureWarmUp && m_rhythmHold > 0.0f;

    // Build-up: the highs are steadily rising (risers, snare rolls), more so with the bass cut.
    float build = warmedUp ? std::clamp((slope - 0.5f) / 3.0f, 0.0f, 1.0f) * (bassAbsent ? 1.0f : 0.5f) : 0.0f;
    f.buildUp = Smooth(f.buildUp, build, dt, 1.0f, 0.6f);

    // Drop: the bass slams back in after being away, or a big jump in level after a build.
    const bool bassReturn = !bassAbsent && m_lastBassAbsent >= 1.0f;
    const bool levelJump = f.buildUp > 0.25f && !bassAbsent && m_levelFast > m_levelSlow + 5.0f;
    // A big build-up letting go without a clear drop (while the music carries on, not fading out).
    m_buildPeak = std::max(m_buildPeak, f.buildUp);
    const bool released = m_buildPeak > 0.4f && f.buildUp < 0.15f && m_levelFast > m_levelSlow - 3.0f;
    if (m_buildPeak > 0.0f && f.buildUp < 0.05f && !released) m_buildPeak = 0.0f;
    m_sinceHit += dt;
    const bool gridLocked = f.tempoConfidence > 0.4f;
    bool gridBeat = false;
    if (f.gridBeat != m_lastGridBeat) { m_lastGridBeat = f.gridBeat; if (gridLocked) { ++m_phraseBeats; gridBeat = true; } }
    const bool onDownbeat = f.barPhase < m_lastBarPhase;
    m_lastBarPhase = f.barPhase;
    const bool dance = f.edm > kDanceOn;
    // A big section that hasn't had a big moment for a while: the start of the next phrase (4 bars
    // since the last one, on the downbeat), or after a while without a beat grid. Dance music is built
    // in 8-bar phrases counted from the last drop, and its big moments belong to the drops, so there
    // it waits longer and lands exactly 8, 16, 24... bars after the last one.
    const bool phraseStart = dance ? gridBeat && m_phraseBeats > 0 && m_phraseBeats % kDancePhraseBeats == 0
                                   : onDownbeat && m_phraseBeats >= kPeakPhraseBeats;
    // (Not during a build-up: that saves the moment for the drop.)
    const bool peak = f.intensity > kPeakIntensity && f.buildUp < 0.2f &&
                      (gridLocked ? phraseStart && m_sinceHit > (dance ? kDancePeakGap : kPeakGap) : m_sinceHit > kPeakGapNoGrid);
    if (warmedUp && (bassReturn || levelJump || released || peak) && m_dropCooldown <= 0.0f) {
        f.drop = 1.0f;
        ++f.dropCount;
        f.buildUp = 0.0f;
        m_dropCooldown = 6.0f;
        m_sinceHit = 0.0f;
        m_buildPeak = 0.0f;
        // The phrase starts on the beat nearest the drop: if that beat is still to come, don't count it.
        m_phraseBeats = f.beatPhase > 0.5f ? -1 : 0;
    }
    m_lastBassAbsent = m_bassAbsentTime;
}
