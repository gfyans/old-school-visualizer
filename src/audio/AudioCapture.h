#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>

struct IMMDevice;
struct IMMDeviceEnumerator;

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

// Captures whatever is playing on an output device (WASAPI loopback), downmixed
// to mono, into a lock-free ring buffer. Runs on its own thread. By default it
// follows the system default device (e.g. headphones plugged in); a preferred
// device can be chosen instead, with automatic fallback while it is unavailable.
class AudioCapture {
public:
    AudioCapture() = default;
    ~AudioCapture() { Stop(); }
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    void Start();
    void Stop();

    // Copies the most recent `count` mono samples, oldest first (count <= kRingSize / 2).
    void GetLatest(float* out, size_t count) const;
    // The latest `count` mono and stereo "side" ((L - R) / 2) samples, from one consistent
    // snapshot: left = mono + side, right = mono - side.
    void GetLatestStereo(float* mono, float* side, size_t count) const;
    // Every sample written since the absolute index `from` (up to maxCount), oldest first, for
    // consumers that need the whole stream (recording). Advances `from`; skips ahead if the
    // reader fell more than half the ring behind. Returns the number of samples copied.
    size_t ReadSince(uint64_t& from, float* mono, float* side, size_t maxCount) const;
    uint32_t SampleRate() const { return m_sampleRate.load(std::memory_order_relaxed); }
    uint64_t TotalSamples() const { return m_written.load(std::memory_order_acquire); }
    std::wstring DeviceName() const;   // friendly name of the device being captured

    // Active output devices (requires COM initialised on the calling thread).
    static std::vector<AudioDevice> ListOutputDevices();
    // Empty = follow the system default output.
    void SetPreferredDevice(const std::wstring& id);
    std::wstring PreferredDevice() const;

    static constexpr size_t kRingSize = 16384;   // power of two, ~340 ms at 48 kHz

private:
    void ThreadMain();
    bool RunSession();   // returns true to retry immediately (device changed), false to back off
    bool ResolveDevice(IMMDeviceEnumerator* enumerator, Microsoft::WRL::ComPtr<IMMDevice>& device, std::wstring& id);
    void Write(const float* samples, const float* side, size_t count);
    void WriteSilence(size_t count);

    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    float m_ring[kRingSize] = {};   // mono
    float m_side[kRingSize] = {};   // (L - R) / 2
    std::atomic<uint64_t> m_written{ 0 };
    std::atomic<uint32_t> m_sampleRate{ 48000 };
    mutable std::mutex m_nameMutex;   // guards the two strings below
    std::wstring m_deviceName;
    std::wstring m_preferred;
    std::atomic<bool> m_reselect{ false };
};
