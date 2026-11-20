// Dev tool: runs a WAV file through the real AudioAnalyzer exactly as the app
// does (72 fps frames + the continuous sample stream) and writes one CSV row per
// frame, plus a summary on stdout. Pair with tools/analysis/plot.py for charts.
//
//   vis_replay.exe song.wav [out.csv]
#include "audio/AudioAnalyzer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

bool ReadWav(const wchar_t* path, std::vector<float>& mono, uint32_t& rate) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    std::vector<uint8_t> bytes;
    fseek(f, 0, SEEK_END);
    bytes.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    fread(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    if (bytes.size() < 44 || memcmp(bytes.data(), "RIFF", 4) || memcmp(bytes.data() + 8, "WAVE", 4)) return false;

    uint16_t format = 0, channels = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        uint32_t size;
        memcpy(&size, &bytes[pos + 4], 4);
        const uint8_t* body = &bytes[pos + 8];
        if (!memcmp(&bytes[pos], "fmt ", 4)) {
            memcpy(&format, body, 2);
            memcpy(&channels, body + 2, 2);
            memcpy(&rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
            if (format == 0xFFFE && size >= 26) memcpy(&format, body + 24, 2);   // extensible: sub-format
        } else if (!memcmp(&bytes[pos], "data", 4)) {
            size = static_cast<uint32_t>(std::min<size_t>(size, bytes.size() - pos - 8));
            const size_t frameBytes = size_t(channels) * bits / 8;
            if (!frameBytes) return false;
            const size_t frames = size / frameBytes;
            mono.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    const uint8_t* p = body + i * frameBytes + c * (bits / 8);
                    if (format == 3 && bits == 32) { float v; memcpy(&v, p, 4); sum += v; }
                    else if (bits == 16) { int16_t v; memcpy(&v, p, 2); sum += v / 32768.0f; }
                    else if (bits == 24) { sum += ((int32_t(p[0]) << 8 | int32_t(p[1]) << 16 | int32_t(p[2]) << 24) >> 8) / 8388608.0f; }
                    else if (bits == 32) { int32_t v; memcpy(&v, p, 4); sum += v / 2147483648.0f; }
                }
                mono[i] = sum / channels;
            }
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: vis_replay song.wav [out.csv]\n");
        return 2;
    }
    std::vector<float> audio;
    uint32_t rate = 0;
    if (!ReadWav(argv[1], audio, rate) || rate == 0) {
        std::fwprintf(stderr, L"could not read %ls (PCM 16/24/32-bit or float WAV expected)\n", argv[1]);
        return 1;
    }
    std::wstring csvPath = argc > 2 ? argv[2] : std::wstring(argv[1]) + L".csv";
    FILE* csv = nullptr;
    if (_wfopen_s(&csv, csvPath.c_str(), L"w") != 0 || !csv) return 1;
    std::fprintf(csv, "t,level,bass,lowmid,highmid,treble,intensity,silent,onset_beat,bpm,confidence,beat_phase,grid_beat,build_up,drop,periodicity,contrast,phase_error,percussive\n");

    constexpr float kFps = 72.0f;
    const size_t hop = static_cast<size_t>(rate / kFps);
    const size_t n = AudioAnalyzer::kFftSize;
    auto analyzer = std::make_unique<AudioAnalyzer>();
    std::vector<float> window(n, 0.0f);

    uint32_t lastBeat = 0, lastGrid = 0, lastDrop = 0;
    std::vector<float> dropTimes, bpms, confs;
    size_t onsetBeats = 0, gridBeats = 0;
    float peakBuild = 0.0f, peakBuildAt = 0.0f;

    for (size_t start = 0; start + hop <= audio.size(); start += hop) {
        const float* fresh = &audio[start];
        window.erase(window.begin(), window.begin() + hop);
        window.insert(window.end(), fresh, fresh + hop);
        analyzer->ProcessStream(fresh, hop, static_cast<float>(rate));
        analyzer->Update(window.data(), static_cast<float>(rate), true, 1.0f / kFps);
        const AudioFeatures& f = analyzer->Features();
        const float t = float(start + hop) / rate;

        const bool onset = f.beatCount != lastBeat, grid = f.gridBeat != lastGrid, drop = f.dropCount != lastDrop;
        lastBeat = f.beatCount; lastGrid = f.gridBeat; lastDrop = f.dropCount;
        onsetBeats += onset;
        if (grid && f.tempoConfidence > 0.4f) ++gridBeats;
        if (drop) dropTimes.push_back(t);
        if (f.buildUp > peakBuild) { peakBuild = f.buildUp; peakBuildAt = t; }
        if (t > 10.0f && f.bpm > 0.0f) { bpms.push_back(f.bpm); confs.push_back(f.tempoConfidence); }

        const BeatTracker::State& diag = analyzer->TrackerState();
        std::fprintf(csv, "%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.2f,%.3f,%.3f,%d,%.3f,%d,%.3f,%.3f,%.3f,%.3f\n",
            t, f.level, f.bass, f.lowMid, f.highMid, f.treble, f.intensity, f.silent ? 1 : 0, onset ? 1 : 0,
            f.bpm, f.tempoConfidence, f.beatPhase, grid ? 1 : 0, f.buildUp, drop ? 1 : 0,
            diag.periodicity, diag.contrast, diag.phaseError, diag.percussive);
    }
    fclose(csv);

    auto median = [](std::vector<float> v) { if (v.empty()) return 0.0f; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
    const float duration = float(audio.size()) / rate;
    std::wprintf(L"%ls\n", argv[1]);
    std::printf("  %.1f s at %u Hz | tempo (after 10 s): median %.1f BPM, median confidence %.2f\n",
        duration, rate, median(bpms), median(confs));
    std::printf("  onset beats %zu, confident grid beats %zu | peak build-up %.2f at %.1f s | drops:",
        onsetBeats, gridBeats, peakBuild, peakBuildAt);
    for (float d : dropTimes) std::printf(" %.1fs", d);
    std::printf("%s\n", dropTimes.empty() ? " none" : "");
    return 0;
}
