// Dev tool: records whatever is playing into one WAV per song, for tuning the
// analyzer on real music. Splits automatically when the track changes (via the
// Windows media session) and names files after the track. Only records while
// audio is actually playing. Uses the same audio device as the app (settings.ini).
//
//   vis_record.exe [--seconds 75] [--out recordings]
//
// Play a few songs (skipping to the next after a minute is fine), then Ctrl+C.
// Recordings are for local testing only: the folder is git-ignored.
#include "audio/AudioCapture.h"
#include "media/NowPlaying.h"
#include "platform/Settings.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{ false };

BOOL WINAPI OnCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

class WavWriter {
public:
    bool Open(const std::wstring& path, uint32_t rate) {
        Close();
        if (_wfopen_s(&m_file, path.c_str(), L"wb") != 0 || !m_file) { m_file = nullptr; return false; }
        m_rate = rate;
        m_samples = 0;
        WriteHeader();
        return true;
    }
    void Write(const float* s, size_t n) {
        if (!m_file) return;
        std::vector<int16_t> pcm(n);
        for (size_t i = 0; i < n; ++i) pcm[i] = static_cast<int16_t>(std::lround(std::fmax(-1.0f, std::fmin(1.0f, s[i])) * 32767.0f));
        fwrite(pcm.data(), sizeof(int16_t), n, m_file);
        m_samples += n;
    }
    double Seconds() const { return m_rate ? double(m_samples) / m_rate : 0.0; }
    bool IsOpen() const { return m_file != nullptr; }
    void Close() {
        if (!m_file) return;
        fseek(m_file, 0, SEEK_SET);
        WriteHeader();   // now with the real sizes
        fclose(m_file);
        m_file = nullptr;
    }
    ~WavWriter() { Close(); }

private:
    void WriteHeader() {
        const uint32_t dataBytes = static_cast<uint32_t>(m_samples * 2);
        auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, m_file); };
        auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, m_file); };
        fwrite("RIFF", 1, 4, m_file); u32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, m_file);
        u32(16); u16(1); u16(1); u32(m_rate); u32(m_rate * 2); u16(2); u16(16);
        fwrite("data", 1, 4, m_file); u32(dataBytes);
    }
    FILE* m_file = nullptr;
    uint32_t m_rate = 0;
    uint64_t m_samples = 0;
};

std::wstring SafeName(std::wstring s) {
    for (wchar_t& c : s)
        if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_';
    if (s.size() > 100) s.resize(100);
    return s;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    double maxSeconds = 75.0, stopAfter = 0.0;
    std::wstring outDir = L"recordings";
    for (int i = 1; i + 1 < argc; ++i) {
        if (!wcscmp(argv[i], L"--seconds")) maxSeconds = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--out")) outDir = argv[++i];
        else if (!wcscmp(argv[i], L"--stop-after")) stopAfter = _wtof(argv[++i]);   // for scripted runs
    }
    const ULONGLONG startTick = GetTickCount64();
    CreateDirectoryW(outDir.c_str(), nullptr);
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    SetConsoleOutputCP(CP_UTF8);

    Settings settings;
    settings.Load();
    auto capture = std::make_unique<AudioCapture>();
    capture->SetPreferredDevice(settings.audioDevice);
    capture->Start();
    NowPlaying nowPlaying;
    nowPlaying.Start();

    std::wprintf(L"Recording to %ls\\ (max %.0f s per song). Play some music; Ctrl+C to finish.\n", outDir.c_str(), maxSeconds);

    WavWriter wav;
    uint32_t trackSerial = 0, fileIndex = 0;
    std::wstring trackName = L"unknown";
    bool trackDone = false;
    uint64_t last = capture->TotalSamples();
    std::vector<float> chunk(AudioCapture::kRingSize / 2);
    int lastPrinted = -1;
    int saved = 0;

    while (!g_stop) {
        Sleep(50);
        if (stopAfter > 0.0 && (GetTickCount64() - startTick) / 1000.0 >= stopAfter) break;
        const TrackInfo track = nowPlaying.Get();
        if (track.trackSerial != trackSerial) {
            trackSerial = track.trackSerial;
            if (wav.IsOpen()) {
                std::wprintf(L"\n  saved %.0f s\n", wav.Seconds());
                wav.Close();
                ++saved;
            }
            trackName = track.title.empty() ? L"unknown" : track.artist + L" - " + track.title;
            trackDone = false;
            std::wprintf(L"\nNow playing: %ls\n", trackName.c_str());
        }

        const uint64_t total = capture->TotalSamples();
        const size_t fresh = static_cast<size_t>(std::min<uint64_t>(total - last, chunk.size()));
        last = total;
        if (fresh == 0 || trackDone) continue;
        capture->GetLatest(chunk.data(), fresh);

        float peak = 0.0f;
        for (size_t i = 0; i < fresh; ++i) peak = std::fmax(peak, std::fabs(chunk[i]));
        if (peak < 1e-4f) continue;   // paused / silent: don't record

        if (!wav.IsOpen()) {
            wchar_t name[16];
            swprintf_s(name, L"%02u - ", ++fileIndex);
            wav.Open(outDir + L"\\" + name + SafeName(trackName) + L".wav", capture->SampleRate());
        }
        wav.Write(chunk.data(), fresh);
        const int secs = static_cast<int>(wav.Seconds());
        if (secs != lastPrinted) {
            lastPrinted = secs;
            std::wprintf(L"\r  recording %3d s  ", secs);
        }
        if (wav.Seconds() >= maxSeconds) {
            std::wprintf(L"\n  saved %.0f s (limit reached; skip to the next song when ready)\n", wav.Seconds());
            wav.Close();
            ++saved;
            trackDone = true;
        }
    }
    if (wav.IsOpen()) {
        std::wprintf(L"\n  saved %.0f s\n", wav.Seconds());
        wav.Close();
        ++saved;
    }
    std::wprintf(L"\nDone: %d recording(s) in %ls\\\n", saved, outDir.c_str());
    return 0;
}
