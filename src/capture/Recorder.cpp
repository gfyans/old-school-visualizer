#include "capture/Recorder.h"
#include "audio/AudioCapture.h"
#include "render/Renderer.h"
#include "g_capture_blit_ps.h"
#include "g_capture_y_ps.h"
#include "g_capture_uv_ps.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <strmif.h>
#include <initguid.h>   // defines the CODECAPI_ GUIDs below in this file
#include <codecapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int64_t kSecond = 10'000'000;            // Media Foundation time unit: 100 ns
constexpr int64_t kSegmentLength = 10 * kSecond;
constexpr UINT32 kFps = 60;
constexpr UINT32 kAudioRate = 48000;               // AAC wants 44.1 or 48 kHz
constexpr int kMaxWidth = 2560, kMaxHeight = 1440;  // keeps any GPU's H.264 encoder happy

int64_t QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t QpcFreq() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }

std::wstring VideosFolder() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    if (result.empty()) return {};
    result += L"\\Visualizer";
    CreateDirectoryW(result.c_str(), nullptr);
    return result;
}

std::wstring Timestamped(const wchar_t* prefix) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"%s %04d-%02d-%02d %02d-%02d-%02d.mp4", prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return name;
}

// Recording size: the frame, scaled to fit kMaxWidth x kMaxHeight, even dimensions.
void EncodeSize(int w, int h, int& ew, int& eh) {
    const double s = std::min({ 1.0, double(kMaxWidth) / w, double(kMaxHeight) / h });
    ew = std::max(64, static_cast<int>(w * s) & ~1);
    eh = std::max(64, static_cast<int>(h * s) & ~1);
}

} // namespace

struct Recorder::Writer {
    ComPtr<IMFSinkWriter> sink;
    DWORD video = 0, audio = 0;
};

bool Recorder::Init(const AudioCapture* audio) {
    m_audio = audio;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    m_mfStarted = true;

    // Segments live in %TEMP%\Visualizer; clear out leftovers from a previous run.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    m_tempDir = std::wstring(temp) + L"Visualizer";
    CreateDirectoryW(m_tempDir.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((m_tempDir + L"\\segment-*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do { DeleteFileW((m_tempDir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    m_thread = std::thread(&Recorder::EncoderMain, this);
    m_audioThread = std::thread(&Recorder::AudioMain, this);
    return true;
}

void Recorder::Shutdown() {
    if (m_thread.joinable()) {
        {
            std::lock_guard lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    if (m_audioThread.joinable()) m_audioThread.join();   // it watches m_quit too
    for (std::thread& t : m_joiners) if (t.joinable()) t.join();
    m_joiners.clear();
    for (const Segment& s : m_segments) { DeleteFileW(s.path.c_str()); DeleteFileW(s.pcmPath.c_str()); }
    m_segments.clear();
    ReleaseDeviceResources();
    if (m_mfStarted) { MFShutdown(); m_mfStarted = false; }
}

void Recorder::ReleaseDeviceResources() {
    for (auto& s : m_staging) s.Reset();
    for (auto& s : m_stagingUV) s.Reset();
    m_yRTV.Reset(); m_uvRTV.Reset(); m_yTex.Reset(); m_uvTex.Reset();
    m_scaledSRV.Reset();
    m_yPS.Reset(); m_uvPS.Reset();
    m_srcSRV.Reset();
    m_srcCopy.Reset();
    m_scaledRTV.Reset();
    m_scaled.Reset();
    m_blitPS.Reset();
    m_sampler.Reset();
    m_srcW = m_srcH = m_gpuW = m_gpuH = 0;
    for (bool& f : m_stagingFull) f = false;
}

void Recorder::Post(const std::wstring& message) {
    std::lock_guard lock(m_mutex);
    m_message = message;
}

std::wstring Recorder::TakeMessage() {
    std::lock_guard lock(m_mutex);
    std::wstring m;
    m.swap(m_message);
    return m;
}

void Recorder::SetOutputFolder(const std::wstring& folder) {
    std::lock_guard lock(m_mutex);
    m_outputFolder = folder;
}

std::wstring Recorder::ShortPath(const std::wstring& path, size_t maxChars) {
    if (path.size() <= maxChars) return path;
    const size_t last = path.find_last_of(L'\\');
    const size_t prev = last == std::wstring::npos || last == 0 ? std::wstring::npos : path.find_last_of(L'\\', last - 1);
    const size_t root = path.find(L'\\');
    if (prev == std::wstring::npos || root == std::wstring::npos || prev <= root) return L"…" + path.substr(path.size() - maxChars);
    return path.substr(0, root + 1) + L"…" + path.substr(prev);
}

std::wstring Recorder::OutputFolder() const {
    std::wstring chosen;
    {
        std::lock_guard lock(m_mutex);
        chosen = m_outputFolder;
    }
    const DWORD attrs = chosen.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(chosen.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return chosen;
    return VideosFolder();
}

void Recorder::SetInstantReplay(bool on) {
    m_replayWanted.store(on);
    m_wake.notify_all();
}

void Recorder::SaveReplay() {
    {
        std::lock_guard lock(m_mutex);
        m_saveReplay = true;
    }
    m_wake.notify_all();
}

void Recorder::ToggleRecording() {
    const bool on = !m_recordWanted.load();
    if (on) m_recordStartQpc.store(QpcNow());
    m_recordWanted.store(on);
    m_wake.notify_all();
}

float Recorder::RecordingSeconds() const {
    if (!m_recordWanted.load()) return 0.0f;
    return static_cast<float>(double(QpcNow() - m_recordStartQpc.load()) / double(QpcFreq()));
}

int64_t Recorder::Now() const {
    return static_cast<int64_t>(double(QpcNow() - m_session.startQpc) * double(kSecond) / double(QpcFreq()));
}

// ------------------------------------------------------------------ GPU side (main thread)

bool Recorder::EnsureGpuResources(ID3D11Device* device, int srcW, int srcH) {
    if (!m_blitPS) {
        if (FAILED(device->CreatePixelShader(g_capture_blit_ps, sizeof(g_capture_blit_ps), nullptr, &m_blitPS)) ||
            FAILED(device->CreatePixelShader(g_capture_y_ps, sizeof(g_capture_y_ps), nullptr, &m_yPS)) ||
            FAILED(device->CreatePixelShader(g_capture_uv_ps, sizeof(g_capture_uv_ps), nullptr, &m_uvPS)))
            return false;
        D3D11_SAMPLER_DESC s{};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&s, &m_sampler))) return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    if (srcW != m_srcW || srcH != m_srcH) {   // a copy of the back buffer we can sample
        m_srcSRV.Reset();
        m_srcCopy.Reset();
        d.Width = srcW;
        d.Height = srcH;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &m_srcCopy)) ||
            FAILED(device->CreateShaderResourceView(m_srcCopy.Get(), nullptr, &m_srcSRV)))
            return false;
        m_srcW = srcW;
        m_srcH = srcH;
    }
    if (m_session.width != m_gpuW || m_session.height != m_gpuH) {   // recording-size target + readback ring
        m_scaledRTV.Reset();
        m_scaledSRV.Reset();
        m_scaled.Reset();
        m_yRTV.Reset(); m_uvRTV.Reset(); m_yTex.Reset(); m_uvTex.Reset();
        for (auto& s : m_staging) s.Reset();
        for (auto& s : m_stagingUV) s.Reset();
        d.Width = m_session.width;
        d.Height = m_session.height;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &m_scaled)) ||
            FAILED(device->CreateRenderTargetView(m_scaled.Get(), nullptr, &m_scaledRTV)) ||
            FAILED(device->CreateShaderResourceView(m_scaled.Get(), nullptr, &m_scaledSRV)))
            return false;
        // NV12 planes: luma at full size, chroma at half size.
        D3D11_TEXTURE2D_DESC y = d, uv = d;
        y.Format = DXGI_FORMAT_R8_UNORM;
        y.BindFlags = D3D11_BIND_RENDER_TARGET;
        uv.Format = DXGI_FORMAT_R8G8_UNORM;
        uv.BindFlags = D3D11_BIND_RENDER_TARGET;
        uv.Width = d.Width / 2;
        uv.Height = d.Height / 2;
        if (FAILED(device->CreateTexture2D(&y, nullptr, &m_yTex)) || FAILED(device->CreateRenderTargetView(m_yTex.Get(), nullptr, &m_yRTV)) ||
            FAILED(device->CreateTexture2D(&uv, nullptr, &m_uvTex)) || FAILED(device->CreateRenderTargetView(m_uvTex.Get(), nullptr, &m_uvRTV)))
            return false;
        y.Usage = uv.Usage = D3D11_USAGE_STAGING;
        y.BindFlags = uv.BindFlags = 0;
        y.CPUAccessFlags = uv.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < 3; ++i)
            if (FAILED(device->CreateTexture2D(&y, nullptr, &m_staging[i])) || FAILED(device->CreateTexture2D(&uv, nullptr, &m_stagingUV[i])))
                return false;
        for (bool& f : m_stagingFull) f = false;
        m_gpuW = m_session.width;
        m_gpuH = m_session.height;
    }
    return true;
}

void Recorder::CaptureFrame(Renderer& renderer) {
    if (!m_thread.joinable()) return;
    const bool want = m_replayWanted.load() || m_recordWanted.load();
    if (!want) return;

    ComPtr<ID3D11Texture2D> back = renderer.BackBuffer();
    if (!back) return;
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);

    {
        std::lock_guard lock(m_mutex);
        if (!m_sessionActive) {   // a new encoding run: fix its size now
            m_session.startQpc = QpcNow();
            EncodeSize(static_cast<int>(bd.Width), static_cast<int>(bd.Height), m_session.width, m_session.height);
            m_sessionActive = true;
            m_frames.clear();
            for (bool& f : m_stagingFull) f = false;
            m_lastCaptureQpc = 0;
        }
    }

    // At most 60 frames a second.
    const int64_t qpc = QpcNow();
    if (m_lastCaptureQpc && qpc - m_lastCaptureQpc < QpcFreq() / (kFps + 2)) return;
    m_lastCaptureQpc = qpc;

    ID3D11Device* device = renderer.Device();
    ID3D11DeviceContext* ctx = renderer.Context();
    if (!EnsureGpuResources(device, static_cast<int>(bd.Width), static_cast<int>(bd.Height))) return;

    // Scale into the recording-size target, letterboxed if the window's shape has changed.
    ctx->CopyResource(m_srcCopy.Get(), back.Get());
    const float black[4] = { 0, 0, 0, 1 };
    ID3D11RenderTargetView* rtv = m_scaledRTV.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, black);
    const float s = std::min(float(m_gpuW) / m_srcW, float(m_gpuH) / m_srcH);
    const float vw = m_srcW * s, vh = m_srcH * s;
    D3D11_VIEWPORT vp{ (m_gpuW - vw) * 0.5f, (m_gpuH - vh) * 0.5f, vw, vh, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* srv = m_srcSRV.Get();
    ID3D11SamplerState* sampler = m_sampler.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    renderer.DrawFullscreen(m_blitPS.Get());
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);

    // RGB -> NV12 on the GPU: luma plane, then the half-size chroma plane.
    ID3D11ShaderResourceView* scaled = m_scaledSRV.Get();
    ID3D11RenderTargetView* yRtv = m_yRTV.Get();
    ctx->OMSetRenderTargets(1, &yRtv, nullptr);
    D3D11_VIEWPORT full{ 0, 0, float(m_gpuW), float(m_gpuH), 0, 1 };
    ctx->RSSetViewports(1, &full);
    ctx->PSSetShaderResources(0, 1, &scaled);
    renderer.DrawFullscreen(m_yPS.Get());
    ID3D11RenderTargetView* uvRtv = m_uvRTV.Get();
    ctx->OMSetRenderTargets(1, &uvRtv, nullptr);
    D3D11_VIEWPORT half{ 0, 0, float(m_gpuW / 2), float(m_gpuH / 2), 0, 1 };
    ctx->RSSetViewports(1, &half);
    renderer.DrawFullscreen(m_uvPS.Get());
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Read back the frame from two captures ago (the GPU has finished with it by now).
    const int slot = m_stagingNext;
    ctx->CopyResource(m_staging[slot].Get(), m_yTex.Get());
    ctx->CopyResource(m_stagingUV[slot].Get(), m_uvTex.Get());
    m_stagingTime[slot] = static_cast<int64_t>(double(qpc - m_session.startQpc) * double(kSecond) / double(QpcFreq()));
    m_stagingFull[slot] = true;
    m_stagingNext = (slot + 1) % 3;
    const int oldest = m_stagingNext;
    if (!m_stagingFull[oldest]) return;
    D3D11_MAPPED_SUBRESOURCE my, muv;
    if (FAILED(ctx->Map(m_staging[oldest].Get(), 0, D3D11_MAP_READ, 0, &my))) return;
    if (FAILED(ctx->Map(m_stagingUV[oldest].Get(), 0, D3D11_MAP_READ, 0, &muv))) { ctx->Unmap(m_staging[oldest].Get(), 0); return; }
    Frame frame;
    frame.time = m_stagingTime[oldest];
    // NV12: the luma rows, then the interleaved chroma rows (same row length).
    frame.pixels.resize(size_t(m_gpuW) * m_gpuH * 3 / 2);
    uint8_t* out = frame.pixels.data();
    for (int y = 0; y < m_gpuH; ++y)
        memcpy(out + size_t(y) * m_gpuW, static_cast<const uint8_t*>(my.pData) + size_t(y) * my.RowPitch, size_t(m_gpuW));
    out += size_t(m_gpuW) * m_gpuH;
    for (int y = 0; y < m_gpuH / 2; ++y)
        memcpy(out + size_t(y) * m_gpuW, static_cast<const uint8_t*>(muv.pData) + size_t(y) * muv.RowPitch, size_t(m_gpuW));
    ctx->Unmap(m_staging[oldest].Get(), 0);
    ctx->Unmap(m_stagingUV[oldest].Get(), 0);
    m_stagingFull[oldest] = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_frames.size() > 6) m_frames.pop_front();   // encoder fell behind: drop rather than pile up
        m_frames.push_back(std::move(frame));
    }
    m_wake.notify_all();
}

// ------------------------------------------------------------------ encoder thread

void Recorder::EncoderMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::deque<Frame> frames;
        bool save = false, active = false;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(10), [&] { return m_quit || !m_frames.empty(); });
            if (m_quit) break;
            frames.swap(m_frames);
            save = m_saveReplay;
            m_saveReplay = false;
            active = m_sessionActive;
        }
        const bool want = m_replayWanted.load() || m_recordWanted.load();

        if (save && !m_replayWanted.load()) Post(L"Instant replay is off: press I to turn it on");
        if (active && want && !m_writer && !StartSession()) {
            m_replayWanted.store(false);
            m_recordWanted.store(false);
            std::lock_guard lock(m_mutex);
            m_sessionActive = false;
            continue;
        }
        if (!m_writer) continue;

        const int64_t now = Now();
        // Recording starts on a fresh segment (it begins with a keyframe) and ends on a boundary.
        if (m_recordWanted.load() && !m_recordingActive) {
            m_recordingActive = true;
            m_recordingFrom = now;
            DrainAudio();
            CloseSegment(now);
            OpenSegment(now);
        } else if (!m_recordWanted.load() && m_recordingActive) {
            DrainAudio();
            CloseSegment(now);
            m_recordingActive = false;
            std::vector<Segment> parts;
            for (Segment& s : m_segments)
                if (s.recording) { parts.push_back(s); ++s.inUse; s.recording = false; }
            const std::wstring folder = OutputFolder();
            if (!parts.empty() && !folder.empty())
                Join(std::move(parts), m_recordingFrom, folder + L"\\" + Timestamped(L"Visualizer"));
            if (want) OpenSegment(now);
        }
        if (!m_writer) {   // recording ended with nothing else wanted (or a segment failed to open)
            StopSession();
            continue;
        }

        DrainAudio();
        for (Frame& f : frames) {
            if (f.time - m_segmentStart >= kSegmentLength && m_wroteVideo) {
                DrainAudio();
                CloseSegment(f.time);
                OpenSegment(f.time);
                if (!m_writer) break;
            }
            WriteVideo(f);
        }

        if (save && m_replayWanted.load() && m_writer) {
            DrainAudio();
            CloseSegment(now);
            std::vector<Segment> parts;
            for (Segment& s : m_segments)
                if (s.end > now - static_cast<int64_t>(kReplaySeconds * kSecond)) { parts.push_back(s); ++s.inUse; }
            const std::wstring folder = OutputFolder();
            if (!parts.empty() && !folder.empty())
                Join(std::move(parts), now - static_cast<int64_t>(kReplaySeconds * kSecond), folder + L"\\" + Timestamped(L"Replay"));
            else
                Post(L"Nothing to save yet");
            OpenSegment(now);
        }

        Prune(now);
        if (!want) StopSession();
    }
    StopSession();
    CoUninitialize();
}

bool Recorder::StartSession() {
    m_segments.clear();
    m_recordingActive = false;
    m_audioOut = 0;
    {
        std::lock_guard lock(m_pcmMutex);
        m_pcmQueue.clear();
    }
    m_audioReset.store(true);   // the audio thread restarts its stream at this moment
    m_audioActive.store(true);
    if (!OpenSegment(0)) {
        Post(L"Recording isn't available: no H.264 video encoder found");
        return false;
    }
    return true;
}

void Recorder::StopSession() {
    m_audioActive.store(false);
    if (m_writer) { DrainAudio(); CloseSegment(Now()); }
    for (const Segment& s : m_segments)
        if (s.inUse == 0) { DeleteFileW(s.path.c_str()); DeleteFileW(s.pcmPath.c_str()); }
    std::lock_guard lock(m_mutex);
    m_segments.erase(std::remove_if(m_segments.begin(), m_segments.end(), [](const Segment& s) { return s.inUse == 0; }),
                     m_segments.end());
    m_sessionActive = false;
}

bool Recorder::OpenSegment(int64_t start) {
    wchar_t name[64];
    swprintf_s(name, L"\\segment-%05u.mp4", m_segmentCounter++);
    const std::wstring path = m_tempDir + name;

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    auto* w = new Writer;
    const UINT32 width = static_cast<UINT32>(m_session.width), height = static_cast<UINT32>(m_session.height);
    const UINT32 bitrate = std::clamp(static_cast<UINT32>(double(width) * height * kFps * 0.1), 4'000'000u, 24'000'000u);

    ComPtr<IMFMediaType> vOut, vIn;
    bool ok = SUCCEEDED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &w->sink));
    // Video: NV12 frames in (converted on the GPU), H.264 out.
    ok = ok && SUCCEEDED(MFCreateMediaType(&vOut));
    if (ok) {
        vOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vOut->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        vOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        vOut->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        vOut->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        vOut->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        vOut->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        MFSetAttributeSize(vOut.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(vOut.Get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(vOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ok = SUCCEEDED(w->sink->AddStream(vOut.Get(), &w->video));
    }
    ok = ok && SUCCEEDED(MFCreateMediaType(&vIn));
    if (ok) {
        vIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        vIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vIn->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
        vIn->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        vIn->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        vIn->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        vIn->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        MFSetAttributeSize(vIn.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(vIn.Get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(vIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ok = SUCCEEDED(w->sink->SetInputMediaType(w->video, vIn.Get(), nullptr));
    }
    if (ok) {
        // A keyframe every 2 s, so saved clips can start close to where they're asked to.
        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(w->sink->GetServiceForStream(w->video, GUID_NULL, IID_PPV_ARGS(&codec)))) {
            VARIANT v{};
            v.vt = VT_UI4;
            v.ulVal = kFps * 2;
            codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        }
        ok = SUCCEEDED(w->sink->BeginWriting());
    }
    const std::wstring pcmPath = path.substr(0, path.size() - 4) + L".pcm";
    FILE* pcm = nullptr;
    if (ok && (_wfopen_s(&pcm, pcmPath.c_str(), L"wb") != 0 || !pcm)) ok = false;
    if (!ok) {
        delete w;
        DeleteFileW(path.c_str());
        return false;
    }
    m_writer = w;
    m_pcm = pcm;
    m_segmentStart = start;
    m_wroteVideo = false;
    Segment seg;
    seg.path = path;
    seg.pcmPath = pcmPath;
    seg.audioFrame0 = m_audioOut;
    seg.start = seg.end = start;
    seg.recording = m_recordingActive;
    std::lock_guard lock(m_mutex);
    m_segments.push_back(seg);
    return true;
}

void Recorder::CloseSegment(int64_t end) {
    if (!m_writer) return;
    // A segment without video (e.g. the window was minimised) can't be finalised: drop it.
    const bool ok = m_wroteVideo && SUCCEEDED(m_writer->sink->Finalize());
    m_writer->sink.Reset();
    delete m_writer;
    m_writer = nullptr;
    if (m_pcm) { fclose(m_pcm); m_pcm = nullptr; }
    std::lock_guard lock(m_mutex);
    if (m_segments.empty()) return;
    Segment& seg = m_segments.back();
    seg.end = end;
    seg.audioFrames = m_audioOut - seg.audioFrame0;
    if (!ok) {
        DeleteFileW(seg.path.c_str());
        DeleteFileW(seg.pcmPath.c_str());
        m_segments.pop_back();
    }
}

// Audio thread: every 5 ms, everything new in the capture ring is resampled from the device rate
// (e.g. 96 kHz) to 48 kHz stereo and queued. If the ring ever overflowed, the missing stretch is
// filled with silence so the sound stays in step with the picture.
void Recorder::AudioMain() {
    float mono[4096], side[4096];
    std::vector<int16_t> out;
    out.reserve(4096 * 2);
    uint64_t index = 0;
    double pos = 0.0;
    float lastL = 0.0f, lastR = 0.0f;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            if (m_wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return m_quit; })) break;
        }
        if (!m_audio || !m_audioActive.load()) continue;
        if (m_audioReset.exchange(false)) {
            index = m_audio->TotalSamples();
            pos = 0.0;
            lastL = lastR = 0.0f;
        }
        for (;;) {
            const uint64_t before = index;
            const size_t n = m_audio->ReadSince(index, mono, side, 4096);
            const uint64_t skipped = index - n - before;   // overflowed: fill the hole with silence
            const double step = double(m_audio->SampleRate()) / kAudioRate;
            out.clear();
            if (skipped > 0) out.resize(size_t(double(skipped) / step) * 2, 0);
            if (n == 0 && out.empty()) break;
            // Linear interpolation; position 0 is the last sample of the previous block.
            while (pos < double(n)) {
                const size_t i0 = static_cast<size_t>(pos);
                const float f = static_cast<float>(pos - i0);
                const float l0 = i0 == 0 ? lastL : mono[i0 - 1] + side[i0 - 1];
                const float r0 = i0 == 0 ? lastR : mono[i0 - 1] - side[i0 - 1];
                const float l1 = mono[i0] + side[i0], r1 = mono[i0] - side[i0];
                out.push_back(static_cast<int16_t>(std::clamp(l0 + (l1 - l0) * f, -1.0f, 1.0f) * 32767.0f));
                out.push_back(static_cast<int16_t>(std::clamp(r0 + (r1 - r0) * f, -1.0f, 1.0f) * 32767.0f));
                pos += step;
            }
            if (n > 0) {
                pos -= double(n);
                lastL = mono[n - 1] + side[n - 1];
                lastR = mono[n - 1] - side[n - 1];
            }
            std::lock_guard lock(m_pcmMutex);
            m_pcmQueue.insert(m_pcmQueue.end(), out.begin(), out.end());
            if (n == 0) break;
        }
    }
}

// Encoder thread: whatever audio has arrived goes into the current segment's PCM file.
void Recorder::DrainAudio() {
    std::vector<int16_t> chunk;
    {
        std::lock_guard lock(m_pcmMutex);
        chunk.swap(m_pcmQueue);
    }
    const size_t frames = chunk.size() / 2;
    if (frames == 0) return;
    if (m_pcm) fwrite(chunk.data(), 4, frames, m_pcm);
    m_audioOut += frames;
}

void Recorder::WriteVideo(Frame& frame) {
    if (!m_writer || frame.time < m_segmentStart) return;
    const DWORD bytes = static_cast<DWORD>(frame.pixels.size());
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE* data = nullptr;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buffer)) || FAILED(buffer->Lock(&data, nullptr, nullptr))) return;
    memcpy(data, frame.pixels.data(), bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    if (FAILED(MFCreateSample(&sample))) return;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(frame.time - m_segmentStart);
    sample->SetSampleDuration(kSecond / kFps);
    if (SUCCEEDED(m_writer->sink->WriteSample(m_writer->video, sample.Get()))) m_wroteVideo = true;
}

void Recorder::Prune(int64_t now) {
    // Keep: the segment being written, anything in the current recording or being saved, and
    // (with instant replay on) enough to cover the replay window.
    const int64_t keepFrom = now - static_cast<int64_t>((kReplaySeconds + 12.0) * kSecond);
    const bool replay = m_replayWanted.load();
    std::lock_guard lock(m_mutex);
    for (size_t i = 0; i + 1 < m_segments.size();) {
        Segment& s = m_segments[i];
        const bool keep = s.recording || s.inUse > 0 || (replay && s.end > keepFrom);
        if (keep) { ++i; continue; }
        DeleteFileW(s.path.c_str());
        DeleteFileW(s.pcmPath.c_str());
        m_segments.erase(m_segments.begin() + i);
    }
}

// Joins segments into one MP4, starting at the first keyframe at or after `from` (session time):
// the video is copied as it is; the audio is encoded once from the segments' raw PCM, so it runs
// on without a break. Runs on its own thread.
void Recorder::Join(std::vector<Segment> segments, int64_t from, std::wstring outPath) {
    m_joiners.emplace_back([this, segments = std::move(segments), from, outPath = std::move(outPath)] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMFSinkWriter> out;
        DWORD outVideo = 0, outAudio = 0;
        bool started = false, wrote = false;
        int64_t base = 0, lastVideo = -1;
        int64_t audioFrom = 0, audioWritten = 0;   // first audio frame used (session count), frames written
        std::vector<int16_t> pcm(size_t(kAudioRate / 10) * 2);   // 100 ms at a time
        for (const Segment& seg : segments) {
            ComPtr<IMFSourceReader> reader;
            if (FAILED(MFCreateSourceReaderFromURL(seg.path.c_str(), nullptr, &reader))) continue;
            ComPtr<IMFMediaType> vType;
            if (FAILED(reader->GetNativeMediaType(0, 0, &vType))) continue;
            if (!out) {
                ComPtr<IMFMediaType> aOut, aIn;
                MFCreateMediaType(&aOut);
                aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
                aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);   // 192 kbps
                aOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                MFCreateMediaType(&aIn);
                aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
                aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                aIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                aIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * 4);
                // No throttling: each segment's video is written before its audio, and a throttled
                // writer would wait for audio to catch up that only comes afterwards.
                ComPtr<IMFAttributes> attrs;
                MFCreateAttributes(&attrs, 1);
                attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
                if (FAILED(MFCreateSinkWriterFromURL(outPath.c_str(), nullptr, attrs.Get(), &out)) ||
                    FAILED(out->AddStream(vType.Get(), &outVideo)) || FAILED(out->SetInputMediaType(outVideo, vType.Get(), nullptr)) ||
                    FAILED(out->AddStream(aOut.Get(), &outAudio)) || FAILED(out->SetInputMediaType(outAudio, aIn.Get(), nullptr)) ||
                    FAILED(out->BeginWriting())) {
                    out.Reset();
                    break;
                }
            }
            // This segment's video.
            for (;;) {
                DWORD stream = 0, flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sample;
                if (FAILED(reader->ReadSample(0, 0, &stream, &flags, &ts, &sample))) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                if (!sample) continue;
                const int64_t t = seg.start + ts;
                if (!started) {
                    UINT32 key = 0;
                    sample->GetUINT32(MFSampleExtension_CleanPoint, &key);
                    if (t < from || !key) continue;
                    started = true;
                    base = t;
                    audioFrom = (base * kAudioRate + kSecond - 1) / kSecond;
                }
                if (t - base <= lastVideo) continue;   // keep times strictly increasing across joins
                lastVideo = t - base;
                sample->SetSampleTime(t - base);
                if (SUCCEEDED(out->WriteSample(outVideo, sample.Get()))) wrote = true;
            }
            // ...then the same stretch of audio, continuing seamlessly from the last segment's.
            if (!started) continue;
            FILE* f = nullptr;
            if (_wfopen_s(&f, seg.pcmPath.c_str(), L"rb") != 0 || !f) continue;
            int64_t frame = seg.audioFrame0;
            if (frame < audioFrom) {   // skip what comes before the clip starts
                _fseeki64(f, (audioFrom - frame) * 4, SEEK_SET);
                frame = audioFrom;
            }
            for (;;) {
                const size_t got = fread(pcm.data(), 4, pcm.size() / 2, f);
                if (got == 0) break;
                ComPtr<IMFMediaBuffer> buffer;
                ComPtr<IMFSample> sample;
                BYTE* data = nullptr;
                const DWORD bytes = static_cast<DWORD>(got * 4);
                if (FAILED(MFCreateMemoryBuffer(bytes, &buffer)) || FAILED(buffer->Lock(&data, nullptr, nullptr))) break;
                memcpy(data, pcm.data(), bytes);
                buffer->Unlock();
                buffer->SetCurrentLength(bytes);
                if (FAILED(MFCreateSample(&sample))) break;
                sample->AddBuffer(buffer.Get());
                sample->SetSampleTime(audioWritten * kSecond / kAudioRate);
                sample->SetSampleDuration(int64_t(got) * kSecond / kAudioRate);
                out->WriteSample(outAudio, sample.Get());
                audioWritten += got;
                frame += got;
            }
            fclose(f);
        }
        const bool ok = out && wrote && SUCCEEDED(out->Finalize());
        out.Reset();
        if (!ok) DeleteFileW(outPath.c_str());
        {
            std::lock_guard lock(m_mutex);
            for (const Segment& part : segments)
                for (Segment& s : m_segments)
                    if (s.path == part.path && s.inUse > 0) --s.inUse;
        }
        const size_t slash = outPath.find_last_of(L'\\');
        Post(ok ? L"Saved: " + ShortPath(outPath) : L"Saving the video failed (is " + outPath.substr(0, slash) + L" writable?)");
        CoUninitialize();
    });
}
