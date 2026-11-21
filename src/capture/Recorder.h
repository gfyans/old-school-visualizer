#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AudioCapture;
class Renderer;

// Records what's on screen, with the music, to MP4 (H.264 + AAC via Windows Media Foundation;
// the video encoder runs on the GPU where available).
//
//   Instant replay: while on, the last ~30 s are always kept; SaveReplay() writes them out.
//   Recording:      ToggleRecording() starts, and stops and saves.
//
// How: each frame (before on-screen text) is scaled to the recording size on the GPU and read
// back a couple of frames later so nothing stalls. An encoder thread pulls the loopback audio
// straight from AudioCapture, resamples it to 48 kHz stereo, and encodes both into 10-second
// MP4 segments in %TEMP%. Saving joins the segments it needs into one file without re-encoding
// (on a worker thread). Files go to Videos\Visualizer.
class Recorder {
public:
    Recorder() = default;
    ~Recorder() { Shutdown(); }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool Init(const AudioCapture* audio);   // false if Media Foundation is unavailable
    void Shutdown();

    // Call once per frame after the visualisation is drawn and before overlays.
    void CaptureFrame(Renderer& renderer);
    void ReleaseDeviceResources();          // device lost

    void SetInstantReplay(bool on);
    bool InstantReplay() const { return m_replayWanted.load(); }
    void SaveReplay();
    void ToggleRecording();
    bool Recording() const { return m_recordWanted.load(); }
    float RecordingSeconds() const;

    // Where videos are saved. Empty = Videos\Visualizer (also used if the chosen folder is gone).
    void SetOutputFolder(const std::wstring& folder);
    std::wstring OutputFolder() const;   // the folder actually in use
    // A long path shortened for display: the drive, then the last two parts ("C:\…olderile.mp4").
    static std::wstring ShortPath(const std::wstring& path, size_t maxChars = 60);

    // A one-off notification ("Saved …", errors), cleared when read.
    std::wstring TakeMessage();

    static constexpr double kReplaySeconds = 30.0;

private:
    struct Frame { std::vector<uint8_t> pixels; int64_t time = 0; };   // NV12; time: 100 ns since session start
    struct Segment {
        std::wstring path;
        int64_t start = 0, end = 0;   // session time, 100 ns
        bool recording = false;       // part of the current/last recording
        int inUse = 0;                // being joined into a saved file
    };
    struct Session {                  // one continuous encoding run
        int width = 0, height = 0;
        int64_t startQpc = 0;
    };

    void EncoderMain();
    bool StartSession();
    void StopSession();
    bool OpenSegment(int64_t start);
    void CloseSegment(int64_t end);
    void PullAudio(int64_t now);
    void WriteVideo(Frame& frame);
    void Prune(int64_t now);
    void Join(std::vector<Segment> segments, int64_t from, std::wstring outPath);
    void Post(const std::wstring& message);
    int64_t Now() const;              // session time (100 ns) from QPC
    bool EnsureGpuResources(ID3D11Device* device, int srcW, int srcH);

    const AudioCapture* m_audio = nullptr;
    bool m_mfStarted = false;

    // --- main thread (GPU side) ---
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_srcCopy, m_scaled, m_yTex, m_uvTex, m_staging[3], m_stagingUV[3];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srcSRV, m_scaledSRV;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_scaledRTV, m_yRTV, m_uvRTV;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_blitPS, m_yPS, m_uvPS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    int m_srcW = 0, m_srcH = 0, m_gpuW = 0, m_gpuH = 0;
    int64_t m_stagingTime[3] = {};
    bool m_stagingFull[3] = {};
    int m_stagingNext = 0;
    int64_t m_lastCaptureQpc = 0;

    // --- shared ---
    std::thread m_thread;
    mutable std::mutex m_mutex;       // guards everything in this block
    std::wstring m_outputFolder;      // chosen by the user; empty = default
    std::condition_variable m_wake;
    std::deque<Frame> m_frames;
    std::wstring m_message;
    bool m_quit = false;
    bool m_saveReplay = false;
    bool m_sessionActive = false;
    Session m_session;
    std::atomic<bool> m_replayWanted{ false }, m_recordWanted{ false };
    std::atomic<int64_t> m_recordStartQpc{ 0 };
    std::vector<std::thread> m_joiners;

    // --- encoder thread ---
    struct Writer;                    // IMFSinkWriter wrapper (keeps MF headers out of here)
    Writer* m_writer = nullptr;
    std::vector<Segment> m_segments;  // oldest first; the last one is being written
    bool m_recordingActive = false;
    int64_t m_recordingFrom = 0;
    uint64_t m_audioIndex = 0;        // next capture sample to read
    double m_audioPos = 0.0;          // resampler position (source samples)
    float m_lastL = 0.0f, m_lastR = 0.0f;
    int64_t m_audioOut = 0;           // 48 kHz frames written this session
    int64_t m_segmentStart = 0;
    bool m_wroteVideo = false;
    std::wstring m_tempDir;
    uint32_t m_segmentCounter = 0;
};
