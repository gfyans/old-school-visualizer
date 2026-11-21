// Feeds synthetic signals through AudioAnalyzer at a simulated 72 fps and checks
// levels, auto-gain, onset beats, the beat grid and song-structure detection.
// Run: .\build.ps1 -Test
#include "audio/AudioAnalyzer.h"
#include "media/TrackPalette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace {

constexpr float kRate = 48000.0f;
constexpr float kFps = 72.0f;
constexpr float kPi = 3.14159265358979f;
int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

using Signal = std::function<float(float)>;
using FrameHook = std::function<void(float t, const AudioFeatures&)>;

struct Result {
    AudioFeatures last;
    float avgBass = 0, avgTreble = 0, avgLevel = 0;
    uint32_t beats = 0;
};

// signal(t) gives the sample at time t seconds. Averages are taken over the second half.
Result Run(float seconds, const Signal& signal, const FrameHook& hook = {}) {
    auto analyzer = std::make_unique<AudioAnalyzer>();
    const int n = AudioAnalyzer::kFftSize;
    std::vector<float> history(n, 0.0f), fresh;
    const int hop = int(kRate / kFps);
    const int frames = int(seconds * kFps);
    Result r;
    int counted = 0;
    long long sampleIndex = 0;
    for (int frame = 0; frame < frames; ++frame) {
        fresh.clear();
        for (int i = 0; i < hop; ++i, ++sampleIndex) fresh.push_back(signal(sampleIndex / kRate));
        history.erase(history.begin(), history.begin() + hop);
        history.insert(history.end(), fresh.begin(), fresh.end());
        analyzer->ProcessStream(fresh.data(), fresh.size(), kRate);
        analyzer->Update(history.data(), kRate, true, 1.0f / kFps);
        const AudioFeatures& f = analyzer->Features();
        if (hook) hook(sampleIndex / kRate, f);
        if (frame >= frames / 2) {
            r.avgBass += f.bass; r.avgTreble += f.treble; r.avgLevel += f.level;
            ++counted;
        }
    }
    r.last = analyzer->Features();
    r.avgBass /= counted; r.avgTreble /= counted; r.avgLevel /= counted;
    r.beats = r.last.beatCount;
    return r;
}

float Kick(float since) {   // pitch-dropping sine thump
    return std::sin(2.0f * kPi * (50.0f + 80.0f * std::exp(-since * 30.0f)) * since) * std::exp(-since * 9.0f);
}

// Kick on every beat, off-beat hats, a pad. bpm may vary over time via bpmAt(t).
Signal Groove(std::function<float(float)> bpmAt, float gain) {
    auto rng = std::make_shared<std::mt19937>(42);
    auto phase = std::make_shared<double>(0.0);
    auto lastT = std::make_shared<double>(0.0);
    return [=](float t) {
        *phase += (t - *lastT) * bpmAt(t) / 60.0;   // integrate tempo so changes are continuous
        *lastT = t;
        const float beatFrac = float(*phase - std::floor(*phase));
        const float period = 60.0f / bpmAt(t);
        const float since = beatFrac * period;
        const float hatSince = float(std::fmod(*phase + 0.5, 1.0)) * period;
        const float hat = std::uniform_real_distribution<float>(-1.0f, 1.0f)(*rng) * 0.2f * std::exp(-hatSince * 60.0f);
        const float pad = 0.12f * std::sin(2.0f * kPi * 220.0f * t);
        return gain * (0.8f * Kick(since) + hat + pad);
    };
}

Signal Kicks(float bpm, float gain) { return Groove([bpm](float) { return bpm; }, gain); }

bool TempoMatches(float measured, float truth) {
    for (float k : { 1.0f, 0.5f, 2.0f })
        if (std::fabs(measured / (truth * k) - 1.0f) < 0.02f) return true;
    return false;
}

float WrapHalf(float x) { return x - std::floor(x + 0.5f); }

// EDM-style arrangement at 128 BPM: groove 0-12 s, build-up 12-20 s (kick and bass out,
// rising noise riser, accelerating snare roll), drop at 20 s (groove back), until 28 s.
float Arrangement(float t, std::mt19937& rng) {
    const float period = 60.0f / 128.0f;
    const float since = std::fmod(t, period);
    const float noise = std::uniform_real_distribution<float>(-1.0f, 1.0f)(rng);
    const bool build = t >= 12.0f && t < 20.0f;
    if (!build) {
        const float bass = 0.35f * std::sin(2.0f * kPi * 55.0f * t);
        const float hat = noise * 0.15f * std::exp(-std::fmod(t + period / 2, period) * 60.0f);
        return 0.5f * (0.8f * Kick(since) + bass + hat);
    }
    const float progress = (t - 12.0f) / 8.0f;                  // 0 -> 1
    const float riser = noise * 0.02f * std::pow(30.0f, progress);   // ~+30 dB over the build
    const float rollPeriod = period / (progress < 0.5f ? 2.0f : progress < 0.75f ? 4.0f : 8.0f);
    const float snare = noise * 0.25f * std::exp(-std::fmod(t, rollPeriod) * 40.0f);
    return 0.5f * (riser + snare);
}

} // namespace

int main() {
    std::printf("Silence\n");
    {
        Result r = Run(3.0f, [](float) { return 0.0f; });
        Check(r.last.silent, "reports silent");
        Check(r.avgLevel < 0.02f, "level ~0");
        Check(r.beats == 0, "no beats");
        Check(r.last.tempoConfidence < 0.1f, "no tempo confidence");
    }

    std::printf("60 Hz sine\n");
    {
        Result r = Run(3.0f, [](float t) { return 0.5f * std::sin(2.0f * kPi * 60.0f * t); });
        std::printf("    bass %.2f treble %.2f beats %u\n", r.avgBass, r.avgTreble, r.beats);
        Check(!r.last.silent, "not silent");
        Check(r.avgBass > 0.3f && r.avgTreble < 0.05f, "bass high, treble ~0");
        Check(r.beats <= 1, "steady tone: at most one beat (its onset)");
    }

    std::printf("Broadband treble (8 tones, 4.5-14 kHz)\n");
    {
        Result r = Run(3.0f, [](float t) {
            float s = 0.0f;
            for (int k = 0; k < 8; ++k) s += std::sin(2.0f * kPi * 4500.0f * std::pow(1.175f, float(k)) * t);
            return 0.08f * s;
        });
        std::printf("    bass %.2f treble %.2f\n", r.avgBass, r.avgTreble);
        Check(r.avgTreble > 0.3f && r.avgBass < 0.05f, "treble high, bass ~0");
    }

    std::printf("Onset beats\n");
    {
        Result r120 = Run(10.0f, Kicks(120.0f, 0.5f));
        Result r174 = Run(10.0f, Kicks(174.0f, 0.5f));
        std::printf("    120 BPM: %u beats (~20), 174 BPM: %u beats (~29)\n", r120.beats, r174.beats);
        Check(r120.beats >= 17 && r120.beats <= 21, "120 BPM beat count within 17..21");
        Check(r174.beats >= 25 && r174.beats <= 30, "174 BPM beat count within 25..30");
    }

    std::printf("Auto-gain: same music at full vs 1/20 volume\n");
    {
        Result loud = Run(10.0f, Kicks(120.0f, 0.5f));
        Result quiet = Run(10.0f, Kicks(120.0f, 0.025f));
        std::printf("    level loud %.2f quiet %.2f, beats %u vs %u\n", loud.avgLevel, quiet.avgLevel, loud.beats, quiet.beats);
        Check(std::fabs(loud.avgLevel - quiet.avgLevel) < 0.05f, "levels match within 0.05");
        Check(quiet.beats >= 17 && quiet.beats <= 21, "beats still detected when quiet");
    }

    std::printf("Beat grid: tempo and phase\n");
    float tempoFactorAt[4] = {};
    int tempoIndex = 0;
    for (float bpm : { 120.0f, 128.0f, 140.0f, 174.0f }) {
        // Phase check: at each true kick in the last 4 s, the predicted phase should be ~0.
        const float period = 60.0f / bpm;
        float worst = 0.0f, sumErr = 0.0f;
        int samples = 0;
        float prevT = 0.0f;
        Result r = Run(12.0f, Kicks(bpm, 0.5f), [&](float t, const AudioFeatures& f) {
            if (t < 8.0f) { prevT = t; return; }
            // A kick happened during this frame -> its phase should be just past 0.
            const float kPrev = std::floor(prevT / period), kNow = std::floor(t / period);
            if (kNow > kPrev) {
                const float sinceKick = t - kNow * period;
                // Half-time lock (e.g. DnB counted at 87): grid beats fall on every other kick.
                const float ratio = f.bpm > 0.0f ? std::round(bpm / f.bpm) : 1.0f;
                const float err = std::fabs(WrapHalf(f.beatPhase * ratio - sinceKick / period));
                worst = std::max(worst, err);
                sumErr += err;
                ++samples;
            }
            prevT = t;
        });
        const float meanMs = samples ? sumErr / samples * period * 1000.0f : 999.0f;
        char label[128];
        std::printf("    %.0f BPM -> measured %.1f BPM, confidence %.2f, phase error mean %.0f ms\n",
            bpm, r.last.bpm, r.last.tempoConfidence, meanMs);
        std::snprintf(label, sizeof(label), "%.0f BPM: tempo within 2%% (or half/double)", bpm);
        Check(TempoMatches(r.last.bpm, bpm), label);
        std::snprintf(label, sizeof(label), "%.0f BPM: confident (> 0.4)", bpm);
        Check(r.last.tempoConfidence > 0.4f, label);
        // Half-time locks are judged on every kick, including the off-grid ones, so allow a bit more.
        const bool halfTime = r.last.bpm > 0.0f && bpm / r.last.bpm > 1.5f;
        const float limitMs = halfTime ? 45.0f : 30.0f;
        std::snprintf(label, sizeof(label), "%.0f BPM: predicted beats within %.0f ms of kicks%s", bpm, limitMs,
            halfTime ? " (half-time lock)" : "");
        Check(meanMs < limitMs, label);
        tempoFactorAt[tempoIndex++] = r.last.tempoFactor;
    }
    std::printf("    tempo factor: 120 -> %.2f, 128 -> %.2f, 140 -> %.2f, 174 -> %.2f\n",
        tempoFactorAt[0], tempoFactorAt[1], tempoFactorAt[2], tempoFactorAt[3]);
    Check(std::fabs(tempoFactorAt[0] - 1.0f) < 0.08f, "tempo factor ~1 at 120 BPM");
    Check(tempoFactorAt[2] > tempoFactorAt[1] && tempoFactorAt[1] > tempoFactorAt[0], "tempo factor rises with tempo");
    Check(tempoFactorAt[3] > tempoFactorAt[2], "174 BPM counts as fast despite the half-time grid lock");

    std::printf("Beat grid: tempo change 120 -> 140 BPM at 10 s\n");
    {
        float bpmAt15 = 0.0f;
        Result r = Run(20.0f, Groove([](float t) { return t < 10.0f ? 120.0f : 140.0f; }, 0.5f),
            [&](float t, const AudioFeatures& f) { if (std::fabs(t - 15.0f) < 0.01f) bpmAt15 = f.bpm; });
        std::printf("    at 15 s: %.1f BPM, at 20 s: %.1f BPM\n", bpmAt15, r.last.bpm);
        Check(TempoMatches(r.last.bpm, 140.0f), "follows the new tempo");
        Check(TempoMatches(bpmAt15, 140.0f), "re-locks within 5 s");
    }

    std::printf("Song structure: groove, 8 s build-up, drop at 20 s\n");
    {
        std::mt19937 rng(9);
        float buildDuringGroove = 0.0f, buildLate = 0.0f;
        std::vector<float> dropTimes;
        uint32_t lastDrops = 0;
        Run(28.0f, [&](float t) { return Arrangement(t, rng); }, [&](float t, const AudioFeatures& f) {
            if (t > 3.0f && t < 11.0f) buildDuringGroove = std::max(buildDuringGroove, f.buildUp);
            if (t > 16.0f && t < 20.0f) buildLate = std::max(buildLate, f.buildUp);
            if (f.dropCount != lastDrops) { dropTimes.push_back(t); lastDrops = f.dropCount; }
        });
        std::printf("    build-up during groove %.2f, late in build %.2f; drops at:", buildDuringGroove, buildLate);
        for (float t : dropTimes) std::printf(" %.2fs", t);
        std::printf("\n");
        Check(buildDuringGroove < 0.25f, "no build-up reported during a steady groove");
        Check(buildLate > 0.4f, "build-up reported during the riser");
        Check(dropTimes.size() == 1, "exactly one drop");
        Check(!dropTimes.empty() && dropTimes[0] >= 20.0f && dropTimes[0] < 20.7f, "drop detected within 0.7 s");
    }

    std::printf("Song starting from silence with a fade-in\n");
    {
        // 2 s silence, 3 s fade-in, then a steady groove: none of this is a drop or a build-up.
        Signal groove = Kicks(124.0f, 0.5f);
        float maxBuild = 0.0f;
        Result r = Run(14.0f, [&](float t) {
            if (t < 2.0f) return 0.0f;
            const float fade = std::fmin(1.0f, (t - 2.0f) / 3.0f);
            return groove(t) * fade * fade;
        }, [&](float, const AudioFeatures& f) { maxBuild = std::fmax(maxBuild, f.buildUp); });
        std::printf("    drops %u, max build-up %.2f\n", r.last.dropCount, maxBuild);
        Check(r.last.dropCount == 0, "no drop at song start");
        Check(maxBuild < 0.25f, "no build-up from a fade-in");
    }

    std::printf("Drumless piano: crescendo, left hand coming and going\n");
    {
        // Soft-attack piano-like notes at irregular (rubato) times; a crescendo over 12-20 s;
        // low left-hand notes absent 8-14 s then back. Real-music tuning found these read as
        // build-ups and drops (Debussy). None of it should.
        std::mt19937 rng(5);
        struct Note { float start, freq, amp; };
        std::vector<Note> notes;
        for (float t = 0.2f; t < 26.0f; t += std::uniform_real_distribution<float>(0.18f, 0.55f)(rng)) {
            const float crescendo = 0.25f + 0.75f * std::clamp((t - 12.0f) / 8.0f, 0.0f, 1.0f);
            notes.push_back({ t, 440.0f * std::pow(2.0f, std::uniform_int_distribution<int>(-5, 14)(rng) / 12.0f), 0.18f * crescendo });
            if (t < 8.0f || t > 14.0f) notes.push_back({ t, 55.0f * std::pow(2.0f, std::uniform_int_distribution<int>(0, 7)(rng) / 12.0f), 0.25f });
        }
        auto piano = [&](float t) {
            float s = 0.0f;
            for (const Note& n : notes) {
                const float a = t - n.start;
                if (a < 0.0f || a > 3.0f) continue;
                const float env = std::fmin(1.0f, a / 0.03f) * std::exp(-a * 1.8f);   // soft hammer attack
                s += n.amp * env * (std::sin(2 * kPi * n.freq * a) + 0.4f * std::sin(4 * kPi * n.freq * a));
            }
            return s * 0.5f;
        };
        float maxBuild = 0.0f, maxConf = 0.0f;
        Result r = Run(26.0f, piano, [&](float t, const AudioFeatures& f) {
            maxBuild = std::fmax(maxBuild, f.buildUp);
            if (t > 8.0f) maxConf = std::fmax(maxConf, f.tempoConfidence);
        });
        std::printf("    drops %u, max build-up %.2f, max tempo confidence %.2f, percussive %.2f\n",
            r.last.dropCount, maxBuild, maxConf, r.last.percussive);
        Check(r.last.dropCount == 0, "no drops without drums");
        Check(maxBuild < 0.25f, "no build-up from a crescendo");
        Check(maxConf < 0.4f, "beat grid not trusted without drums");
    }

    std::printf("Waveform\n");
    {
        Result r = Run(1.0f, [](float t) { return 0.3f * std::sin(2.0f * kPi * 200.0f * t); });
        float lo = 1, hi = -1;
        for (float v : r.last.waveform) { lo = std::fmin(lo, v); hi = std::fmax(hi, v); }
        std::printf("    range %.2f..%.2f, first points %.2f %.2f\n", lo, hi, r.last.waveform[0], r.last.waveform[1]);
        Check(hi > 0.8f && lo < -0.8f, "normalised to about +-1");
        Check(std::fabs(r.last.waveform[0]) < 0.2f && r.last.waveform[1] > r.last.waveform[0], "starts on a rising zero crossing");
    }

    std::printf("Oscilloscope data\n");
    {
        constexpr size_t n = 6144;
        std::vector<float> mono(n), side(n);
        auto analyzer = std::make_unique<AudioAnalyzer>();

        // Mono 200 Hz sine: triggered on a rising zero crossing, scaled to about +-0.9.
        for (size_t i = 0; i < n; ++i) { mono[i] = 0.3f * std::sin(2.0f * kPi * 200.0f * i / kRate + 1.0f); side[i] = 0.0f; }
        for (int k = 0; k < 10; ++k) analyzer->UpdateScope(mono.data(), side.data(), n, kRate, 1.0f / kFps);
        const AudioFeatures& f = analyzer->Features();
        float lo = 1, hi = -1;
        for (float v : f.scopeMid) { lo = std::fmin(lo, v); hi = std::fmax(hi, v); }
        std::printf("    sine: first points %.2f %.2f, range %.2f..%.2f\n", f.scopeMid[0], f.scopeMid[1], lo, hi);
        Check(std::fabs(f.scopeMid[0]) < 0.1f && f.scopeMid[1] > f.scopeMid[0], "triggers on a rising zero crossing");
        Check(hi > 0.8f && hi < 0.95f && lo < -0.8f, "auto-scaled to about +-0.9");

        // Left = sine, right = cosine: the X/Y (mid/side) trace is a circle.
        for (size_t i = 0; i < n; ++i) {
            const float l = 0.4f * std::sin(2.0f * kPi * 300.0f * i / kRate), r = 0.4f * std::cos(2.0f * kPi * 300.0f * i / kRate);
            mono[i] = 0.5f * (l + r);
            side[i] = 0.5f * (l - r);
        }
        for (int k = 0; k < 10; ++k) analyzer->UpdateScope(mono.data(), side.data(), n, kRate, 1.0f / kFps);
        float rMin = 9, rMax = 0;
        for (int p = 0; p < AudioFeatures::kScopePoints; ++p) {
            const float rr = std::sqrt(f.scopeMid[p] * f.scopeMid[p] + f.scopeSide[p] * f.scopeSide[p]);
            rMin = std::fmin(rMin, rr); rMax = std::fmax(rMax, rr);
        }
        std::printf("    stereo sine/cosine: X/Y radius %.3f..%.3f\n", rMin, rMax);
        Check(rMax > 0.3f && rMin > 0.97f * rMax, "X/Y trace is a circle");
    }

    std::printf("Album-art palette\n");
    {
        constexpr int n = 48;
        std::vector<uint8_t> img(n * n * 4);
        auto fill = [&](auto colourAt) {
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const auto c = colourAt(x, y);   // r, g, b
                    uint8_t* p = &img[(y * n + x) * 4];
                    p[0] = static_cast<uint8_t>(c[2]); p[1] = static_cast<uint8_t>(c[1]);
                    p[2] = static_cast<uint8_t>(c[0]); p[3] = 255;
                }
        };
        auto dominant = [](const float* c) { return c[0] > c[1] && c[0] > c[2] ? 'r' : c[2] > c[1] ? 'b' : 'g'; };

        // 70% deep red, 30% blue: red should lead, blue be the accent.
        fill([](int x, int) { return x < 34 ? std::array<int, 3>{ 170, 20, 30 } : std::array<int, 3>{ 30, 60, 200 }; });
        TrackPalette p{};
        bool ok = ExtractPalette(img.data(), n, n, p);
        std::printf("    red/blue: mid %.2f %.2f %.2f  hot %.2f %.2f %.2f\n", p.mid[0], p.mid[1], p.mid[2], p.hot[0], p.hot[1], p.hot[2]);
        Check(ok && dominant(p.mid) == 'r', "dominant colour leads (red)");
        Check(ok && dominant(p.hot) == 'b', "contrasting accent found (blue)");
        Check(ok && std::max({ p.deep[0], p.deep[1], p.deep[2] }) < 0.2f, "deep colour is dark");

        // Mostly grey with a little noise: no palette.
        fill([](int x, int y) { const int g = 60 + (x * 7 + y * 13) % 120; return std::array<int, 3>{ g, g, g + 4 }; });
        Check(!ExtractPalette(img.data(), n, n, p), "greyscale art: no palette");

        // Single hue (orange on black): accepted, accent is a light variant.
        fill([](int x, int y) { return (x - 24) * (x - 24) + (y - 24) * (y - 24) < 300 ? std::array<int, 3>{ 240, 130, 20 } : std::array<int, 3>{ 5, 5, 5 }; });
        ok = ExtractPalette(img.data(), n, n, p);
        Check(ok && p.hot[0] > 0.9f && p.hot[1] > 0.6f, "single-hue art: light accent");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
