// Feeds synthetic signals through AudioAnalyzer at a simulated 72 fps and checks
// that bands, auto-gain and beat detection behave. Run: build\Release\analyzer_test.exe
#include "audio/AudioAnalyzer.h"

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

struct Result {
    AudioFeatures last;
    float avgBass = 0, avgTreble = 0, avgLevel = 0;
    uint32_t beats = 0;
};

// signal(t) gives the sample at time t seconds. Averages are taken over the second half.
Result Run(float seconds, const std::function<float(float)>& signal) {
    auto analyzer = std::make_unique<AudioAnalyzer>();
    const int n = AudioAnalyzer::kFftSize;
    std::vector<float> history(n, 0.0f);
    const int hop = int(kRate / kFps);
    const int frames = int(seconds * kFps);
    Result r;
    int counted = 0;
    long long sampleIndex = 0;
    for (int frame = 0; frame < frames; ++frame) {
        history.erase(history.begin(), history.begin() + hop);
        for (int i = 0; i < hop; ++i, ++sampleIndex) history.push_back(signal(sampleIndex / kRate));
        analyzer->Update(history.data(), kRate, true, 1.0f / kFps);
        if (frame >= frames / 2) {
            const AudioFeatures& f = analyzer->Features();
            r.avgBass += f.bass; r.avgTreble += f.treble; r.avgLevel += f.level;
            ++counted;
        }
    }
    r.last = analyzer->Features();
    r.avgBass /= counted; r.avgTreble /= counted; r.avgLevel /= counted;
    r.beats = r.last.beatCount;
    return r;
}

// Kick drum: 55 Hz sine burst with fast decay, every 60/bpm seconds, over a hi-hat-ish noise bed.
std::function<float(float)> Kicks(float bpm, float gain) {
    auto rng = std::make_shared<std::mt19937>(42);
    return [=](float t) {
        const float period = 60.0f / bpm;
        const float since = std::fmod(t, period);
        const float kick = std::sin(2.0f * kPi * 55.0f * since) * std::exp(-since * 18.0f);
        const float noise = std::uniform_real_distribution<float>(-1.0f, 1.0f)(*rng) * 0.05f;
        const float pad = 0.15f * std::sin(2.0f * kPi * 440.0f * t);
        return gain * (0.8f * kick + noise + pad);
    };
}

} // namespace

int main() {
    std::printf("Silence\n");
    {
        Result r = Run(3.0f, [](float) { return 0.0f; });
        Check(r.last.silent, "reports silent");
        Check(r.avgLevel < 0.02f, "level ~0");
        Check(r.beats == 0, "no beats");
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

    std::printf("Kick drum 120 BPM, 10 s\n");
    {
        Result r = Run(10.0f, Kicks(120.0f, 0.5f));
        std::printf("    beats %u (expect ~20)\n", r.beats);
        Check(r.beats >= 17 && r.beats <= 21, "beat count within 17..21");
    }

    std::printf("Kick drum 174 BPM, 10 s\n");
    {
        Result r = Run(10.0f, Kicks(174.0f, 0.5f));
        std::printf("    beats %u (expect ~29)\n", r.beats);
        Check(r.beats >= 25 && r.beats <= 30, "beat count within 25..30");
    }

    std::printf("Auto-gain: same music at full vs 1/20 volume\n");
    {
        Result loud = Run(10.0f, Kicks(120.0f, 0.5f));
        Result quiet = Run(10.0f, Kicks(120.0f, 0.025f));
        std::printf("    level loud %.2f quiet %.2f, beats %u vs %u\n", loud.avgLevel, quiet.avgLevel, loud.beats, quiet.beats);
        Check(std::fabs(loud.avgLevel - quiet.avgLevel) < 0.05f, "levels match within 0.05");
        Check(quiet.beats >= 17 && quiet.beats <= 21, "beats still detected when quiet");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
