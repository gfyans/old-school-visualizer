// vis_mp4check: decodes the audio track of a video (e.g. one the app recorded) and reports its
// length and any dropouts: 10 ms windows that go near-silent between loud ones. Record a steady
// tone to check that saved videos have no gaps.
//   vis_mp4check.exe clip.mp4
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::fwprintf(stderr, L"usage: vis_mp4check clip.mp4\n"); return 2; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(argv[1], nullptr, &reader))) { std::fwprintf(stderr, L"cannot open %ls\n", argv[1]); return 1; }
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want.Get()))) {
        std::fwprintf(stderr, L"no decodable audio track\n");
        return 1;
    }
    ComPtr<IMFMediaType> got;
    reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got);
    UINT32 rate = 48000, channels = 2;
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);

    std::vector<float> mono;
    int gaps = 0;
    LONGLONG expected = -1;   // where the next block should start if the audio is continuous (100 ns)
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, &ts, &sample))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        // A block stamped later than the previous one ended: players play silence in between.
        LONGLONG duration = 0;
        sample->GetSampleDuration(&duration);
        if (expected >= 0 && ts - expected > 20000) {   // > 2 ms
            if (gaps < 20) std::printf("  timestamp gap at %.2f s: %.1f ms of silence\n", ts / 1e7, (ts - expected) / 1e4);
            ++gaps;
        }
        expected = ts + duration;
        ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data = nullptr;
        DWORD length = 0;
        buffer->Lock(&data, nullptr, &length);
        const float* f = reinterpret_cast<const float*>(data);
        for (DWORD i = 0; i + channels <= length / 4; i += channels) {
            float m = 0;
            for (UINT32 c = 0; c < channels; ++c) m += f[i + c];
            mono.push_back(m / channels);
        }
        buffer->Unlock();
    }

    const size_t win = rate / 100;   // 10 ms
    std::vector<float> rms;
    for (size_t i = 0; i + win <= mono.size(); i += win) {
        double s = 0;
        for (size_t k = 0; k < win; ++k) s += double(mono[i + k]) * mono[i + k];
        rms.push_back(float(std::sqrt(s / win)));
    }
    // A dropout: the level falls below a tenth of the last 50 ms, stays down for at most 300 ms,
    // then comes back to at least half (a brief hole in otherwise playing audio).
    int dropouts = 0;
    for (size_t i = 5; i < rms.size(); ++i) {
        float before = 0;
        for (size_t k = i - 5; k < i; ++k) before = std::max(before, rms[k]);
        if (before < 0.02f || rms[i] >= 0.1f * before) continue;
        size_t j = i;
        while (j < rms.size() && rms[j] < 0.5f * before && j - i < 30) ++j;
        if (j < rms.size() && j - i < 30) {
            if (dropouts < 20) std::printf("  dropout at %.2f s, %zu ms long\n", i * 0.01, (j - i) * 10);
            ++dropouts;
        }
        i = j;
    }
    std::printf("%ls: %.2f s of audio at %u Hz, %u channels, %d dropouts, %d timestamp gaps\n", argv[1],
                double(mono.size()) / rate, rate, channels, dropouts, gaps);
    MFShutdown();
    return dropouts == 0 && gaps == 0 ? 0 : 3;
}
