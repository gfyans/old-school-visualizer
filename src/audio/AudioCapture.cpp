#include "audio/AudioCapture.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

enum class SampleFormat { Unsupported, Float32, Int16, Int24, Int32 };

SampleFormat DetectFormat(const WAVEFORMATEX* wf) {
    WORD tag = wf->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) return SampleFormat::Float32;
    if (tag == WAVE_FORMAT_PCM) {
        switch (wf->wBitsPerSample) {
        case 16: return SampleFormat::Int16;
        case 24: return SampleFormat::Int24;
        case 32: return SampleFormat::Int32;
        }
    }
    return SampleFormat::Unsupported;
}

// One interleaved frame -> one mono sample (average of channels).
float ReadMono(const BYTE* frame, SampleFormat fmt, int channels, int bytesPerSample) {
    float sum = 0.0f;
    for (int c = 0; c < channels; ++c) {
        const BYTE* p = frame + c * bytesPerSample;
        switch (fmt) {
        case SampleFormat::Float32: { float v; memcpy(&v, p, 4); sum += v; break; }
        case SampleFormat::Int16: { int16_t v; memcpy(&v, p, 2); sum += v / 32768.0f; break; }
        case SampleFormat::Int24: {
            int32_t v = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 24);
            sum += (v >> 8) / 8388608.0f;
            break;
        }
        case SampleFormat::Int32: { int32_t v; memcpy(&v, p, 4); sum += v / 2147483648.0f; break; }
        default: break;
        }
    }
    return sum / float(channels);
}

struct CoTaskMemDeleter { void operator()(void* p) const { CoTaskMemFree(p); } };

bool DefaultDeviceId(IMMDeviceEnumerator* en, std::wstring& id) {
    ComPtr<IMMDevice> dev;
    LPWSTR raw = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || FAILED(dev->GetId(&raw)))
        return false;
    id = raw;
    CoTaskMemFree(raw);
    return true;
}

std::wstring FriendlyName(IMMDevice* device) {
    std::wstring name = L"(unknown device)";
    ComPtr<IPropertyStore> props;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
        SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        name = pv.pwszVal;
    PropVariantClear(&pv);
    return name;
}

} // namespace

void AudioCapture::Start() {
    if (m_thread.joinable()) return;
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread = std::thread(&AudioCapture::ThreadMain, this);
}

void AudioCapture::Stop() {
    if (!m_thread.joinable()) return;
    SetEvent(m_stopEvent);
    m_thread.join();
    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;
}

std::wstring AudioCapture::DeviceName() const {
    std::lock_guard lock(m_nameMutex);
    return m_deviceName;
}

void AudioCapture::GetLatest(float* out, size_t count) const {
    const uint64_t end = m_written.load(std::memory_order_acquire);
    const uint64_t start = end - count;   // wraps harmlessly; early frames read zeros
    for (size_t i = 0; i < count; ++i)
        out[i] = m_ring[(start + i) & (kRingSize - 1)];
}

void AudioCapture::Write(const float* samples, size_t count) {
    uint64_t w = m_written.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) m_ring[(w + i) & (kRingSize - 1)] = samples[i];
    m_written.store(w + count, std::memory_order_release);
}

void AudioCapture::WriteSilence(size_t count) {
    count = std::min(count, kRingSize);
    uint64_t w = m_written.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) m_ring[(w + i) & (kRingSize - 1)] = 0.0f;
    m_written.store(w + count, std::memory_order_release);
}

void AudioCapture::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0) {
        if (!RunSession()) {
            // No device / failure: keep the visuals decaying with silence and retry shortly.
            WriteSilence(m_sampleRate / 10);
            WaitForSingleObject(m_stopEvent, 1000);
        }
    }
    CoUninitialize();
}

std::vector<AudioDevice> AudioCapture::ListOutputDevices() {
    std::vector<AudioDevice> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) ||
        FAILED(devices->GetCount(&count)))
        return result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        LPWSTR id = nullptr;
        if (FAILED(devices->Item(i, &dev)) || FAILED(dev->GetId(&id))) continue;
        result.push_back({ id, FriendlyName(dev.Get()) });
        CoTaskMemFree(id);
    }
    return result;
}

void AudioCapture::SetPreferredDevice(const std::wstring& id) {
    {
        std::lock_guard lock(m_nameMutex);
        if (m_preferred == id) return;
        m_preferred = id;
    }
    m_reselect.store(true);
}

std::wstring AudioCapture::PreferredDevice() const {
    std::lock_guard lock(m_nameMutex);
    return m_preferred;
}

// The preferred device if set and currently active, otherwise the system default.
bool AudioCapture::ResolveDevice(IMMDeviceEnumerator* enumerator, ComPtr<IMMDevice>& device, std::wstring& id) {
    const std::wstring preferred = PreferredDevice();
    device.Reset();
    if (!preferred.empty()) {
        DWORD state = 0;
        if (SUCCEEDED(enumerator->GetDevice(preferred.c_str(), &device)) &&
            SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE) {
            id = preferred;
            return true;
        }
        device.Reset();
    }
    return SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
           DefaultDeviceId(enumerator, id);
}

bool AudioCapture::RunSession() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    std::wstring deviceId;
    m_reselect.store(false);
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        !ResolveDevice(enumerator.Get(), device, deviceId) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
        return false;
    {
        std::wstring name = FriendlyName(device.Get());
        std::lock_guard lock(m_nameMutex);
        m_deviceName = name;
    }

    WAVEFORMATEX* rawFormat = nullptr;
    if (FAILED(client->GetMixFormat(&rawFormat))) return false;
    std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter> format(rawFormat);

    const SampleFormat sampleFormat = DetectFormat(format.get());
    if (sampleFormat == SampleFormat::Unsupported) return false;
    const int channels = format->nChannels;
    const int blockAlign = format->nBlockAlign;
    const int bytesPerSample = format->wBitsPerSample / 8;
    const uint32_t rate = format->nSamplesPerSec;

    // Event-driven where supported; we also wake every 10 ms regardless, which covers
    // systems that never signal loopback events and lets us pad silence.
    HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    constexpr REFERENCE_TIME kBuffer = 2'000'000;   // 200 ms
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBuffer, 0, format.get(), nullptr);
    bool useEvent = SUCCEEDED(hr);
    if (!useEvent) {
        client.Reset();
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) ||
            FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBuffer, 0, format.get(), nullptr))) {
            CloseHandle(audioEvent);
            return false;
        }
    }
    if ((useEvent && FAILED(client->SetEventHandle(audioEvent))) ||
        FAILED(client->GetService(IID_PPV_ARGS(&capture))) ||
        FAILED(client->Start())) {
        CloseHandle(audioEvent);
        return false;
    }
    m_sampleRate.store(rate, std::memory_order_relaxed);

    LARGE_INTEGER freq, lastWrite, lastDeviceCheck;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastWrite);
    lastDeviceCheck = lastWrite;

    std::vector<float> mono;
    bool retryNow = false;
    HANDLE waits[2] = { m_stopEvent, audioEvent };

    for (;;) {
        DWORD w = WaitForMultipleObjects(useEvent ? 2 : 1, waits, FALSE, 10);
        if (w == WAIT_OBJECT_0) break;   // stop requested

        bool gotData = false;
        UINT32 packet = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                WriteSilence(frames);
            } else {
                mono.resize(frames);
                for (UINT32 i = 0; i < frames; ++i)
                    mono[i] = ReadMono(data + size_t(i) * blockAlign, sampleFormat, channels, bytesPerSample);
                Write(mono.data(), frames);
            }
            capture->ReleaseBuffer(frames);
            gotData = true;
        }
        if (FAILED(hr)) break;   // device invalidated etc. -> new session

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (gotData) {
            lastWrite = now;
        } else {
            // Loopback delivers nothing while nothing plays; feed silence so the analyzer decays.
            double idle = double(now.QuadPart - lastWrite.QuadPart) / double(freq.QuadPart);
            if (idle > 0.03) {
                WriteSilence(size_t(idle * rate));
                lastWrite = now;
            }
        }

        // User picked another device: switch now.
        if (m_reselect.load()) {
            retryNow = true;
            break;
        }
        // Follow the default device / a preferred device coming back (cheap poll every 2 s).
        if (now.QuadPart - lastDeviceCheck.QuadPart > 2 * freq.QuadPart) {
            lastDeviceCheck = now;
            ComPtr<IMMDevice> wanted;
            std::wstring wantedId;
            if (ResolveDevice(enumerator.Get(), wanted, wantedId) && wantedId != deviceId) {
                retryNow = true;
                break;
            }
        }
    }

    client->Stop();
    CloseHandle(audioEvent);
    return retryNow || WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0;
}
