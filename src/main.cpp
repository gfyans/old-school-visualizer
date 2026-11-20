#include "audio/AudioAnalyzer.h"
#include "audio/AudioCapture.h"
#include "platform/FrameLimiter.h"
#include "platform/Settings.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "ui/Overlay.h"
#include "viz/VisualizerHost.h"
#include "viz/nebula/NebulaVisualizer.h"
#include "viz/spectrum/SpectrumVisualizer.h"

#include <algorithm>
#include <memory>

namespace {

constexpr wchar_t kAppName[] = L"Visualizer";
constexpr float kIdleAfterSilence = 5.0f;   // seconds of silence before dropping to low-power frame rate
constexpr double kIdleFps = 30.0;

// To add a visualiser: implement IVisualizer and add it here. Order = ←/→ order.
void RegisterVisualizers(VisualizerHost& host) {
    host.Add(std::make_unique<NebulaVisualizer>());
    host.Add(std::make_unique<SpectrumVisualizer>());
}

class Clock {
public:
    Clock() { QueryPerformanceFrequency(&m_freq); QueryPerformanceCounter(&m_last); }
    // Seconds since the last call, clamped so a stall (drag, minimise) never causes a big jump.
    float Tick() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - m_last.QuadPart) / double(m_freq.QuadPart);
        m_last = now;
        return static_cast<float>(std::min(dt, 0.1));
    }
private:
    LARGE_INTEGER m_freq{}, m_last{};
};

void Fatal(const wchar_t* message) {
    MessageBoxW(nullptr, message, kAppName, MB_OK | MB_ICONERROR);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // for audio device enumeration

    Settings settings;
    settings.Load();

    Window window;
    const UINT dpi = GetDpiForSystem();
    if (!window.Create(instance, kAppName, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96))) {
        Fatal(L"Could not create the window.");
        return 1;
    }
    if (settings.hasPlacement) window.RestorePlacement(settings.windowRect);
    window.Show(settings.hasPlacement && settings.maximized);
    if (settings.fullscreen) window.SetFullscreen(true);

    int w, h;
    window.ClientSize(w, h);
    Renderer renderer;
    if (!renderer.Init(window.Handle(), w, h)) {
        Fatal(L"Could not initialise Direct3D 11 (a DirectX 11 capable GPU is required).");
        return 1;
    }

    VisualizerHost host;
    RegisterVisualizers(host);
    Overlay overlay;
    overlay.Init();

    auto createDeviceResources = [&] {
        overlay.CreateDeviceResources(renderer.D2DContext());
        return host.CreateResources(renderer);
    };
    if (!createDeviceResources()) { Fatal(L"Could not create graphics resources."); return 1; }
    if (const int saved = host.FindByName(settings.visualizer); saved > 0) host.Select(saved, false);
    else host.Active().Activate();

    auto capture = std::make_unique<AudioCapture>();
    auto analyzer = std::make_unique<AudioAnalyzer>();
    float samples[AudioAnalyzer::kFftSize];
    uint64_t lastSampleCount = 0;
    capture->SetPreferredDevice(settings.audioDevice);
    capture->Start();

    bool showStats = false, showHelp = false;
    auto announce = [&] { overlay.ShowMessage(host.Active().Name()); };

    // D: cycle System default -> each active output device -> back to default.
    auto cycleAudioDevice = [&] {
        const std::vector<AudioDevice> devices = AudioCapture::ListOutputDevices();
        const std::wstring current = capture->PreferredDevice();
        int index = -1;   // -1 = system default
        for (int i = 0; i < static_cast<int>(devices.size()); ++i)
            if (devices[i].id == current) index = i;
        index = index + 1 < static_cast<int>(devices.size()) ? index + 1 : -1;
        if (index < 0) {
            capture->SetPreferredDevice({});
            overlay.ShowMessage(L"Audio: system default output", 3.0f);
        } else {
            capture->SetPreferredDevice(devices[index].id);
            overlay.ShowMessage(L"Audio: " + devices[index].name, 3.0f);
        }
    };

    auto saveSettings = [&] {
        window.GetPlacement(settings.windowRect, settings.maximized);
        settings.hasPlacement = true;
        settings.fullscreen = window.IsFullscreen();
        settings.visualizer = host.Active().Name();
        settings.audioDevice = capture->PreferredDevice();
        settings.Save();
    };
    window.onClose = saveSettings;   // while the window still exists

    window.onResize = [&](int cw, int ch) {
        renderer.Resize(cw, ch);
        host.Resize(cw, ch);
    };
    window.onKeyDown = [&](UINT vk) {
        switch (vk) {
        case 'F':        window.ToggleFullscreen(); break;
        case VK_ESCAPE:  if (window.IsFullscreen()) window.SetFullscreen(false); else window.Close(); break;
        case VK_RIGHT:   host.Next(); announce(); break;
        case VK_LEFT:    host.Previous(); announce(); break;
        case VK_F1:      showStats = !showStats; break;
        case 'H':        showHelp = !showHelp; break;
        case 'D':        cycleAudioDevice(); break;
        default:         host.Active().OnKey(vk); break;
        }
    };

    FrameLimiter limiter;
    HMONITOR currentMonitor = nullptr;
    double displayFps = 60.0;
    bool lowPower = false;
    auto applyFrameRate = [&] {
        limiter.SetTargetFps(lowPower ? std::min(displayFps, kIdleFps) : displayFps);
    };
    auto updateMonitor = [&] {   // re-pick the cap when the window lands on another monitor
        HMONITOR m = MonitorFromWindow(window.Handle(), MONITOR_DEFAULTTONEAREST);
        if (m == currentMonitor) return;
        currentMonitor = m;
        displayFps = FrameLimiter::PickTargetFps(FrameLimiter::DisplayRefreshHz(window.Handle()));
        applyFrameRate();
    };
    updateMonitor();

    overlay.ShowMessage(std::wstring(host.Active().Name()) + L"   ·   press H for help", 4.0f);

    Clock clock;
    double time = 0.0;
    int frames = 0, fps = 0;
    float statsTimer = 0.0f, silentTime = 0.0f;

    for (;;) {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;

        // Minimised: render nothing and sleep until a message arrives.
        if (window.IsMinimized()) {
            WaitMessage();
            clock.Tick();
            continue;
        }

        limiter.Wait();
        renderer.WaitForNextFrame();
        const float dt = clock.Tick();
        time += dt;

        // --- Audio ---
        const uint64_t sampleCount = capture->TotalSamples();
        capture->GetLatest(samples, AudioAnalyzer::kFftSize);
        analyzer->Update(samples, static_cast<float>(capture->SampleRate()), sampleCount != lastSampleCount, dt);
        lastSampleCount = sampleCount;
        const AudioFeatures& audio = analyzer->Features();

        silentTime = audio.silent ? silentTime + dt : 0.0f;
        if (lowPower != (silentTime > kIdleAfterSilence)) {
            lowPower = !lowPower;
            applyFrameRate();
        }

        // --- Visuals ---
        host.Update(audio, dt);
        std::wstring message = host.Active().TakeMessage();
        if (!message.empty()) overlay.ShowMessage(message);

        renderer.UploadAudio(audio);
        host.Render(renderer, static_cast<float>(time), dt);

        overlay.Update(dt);
        if (overlay.HasContent(showStats, showHelp)) {
            if (ID2D1DeviceContext* d2d = renderer.BeginOverlay()) {
                OverlayStats stats;
                if (showStats) {
                    stats.fps = fps;
                    stats.targetFps = limiter.TargetFps();
                    stats.lowPower = lowPower;
                    stats.width = renderer.Width();
                    stats.height = renderer.Height();
                    stats.renderWidth = host.ActiveRenderWidth();
                    stats.renderHeight = host.ActiveRenderHeight();
                    stats.gpuMs = host.ActiveGpuMs();
                    stats.audioDevice = capture->DeviceName();
                    stats.sampleRate = capture->SampleRate();
                    stats.visualizer = host.Active().Name();
                    stats.visualizerStatus = host.Active().Status();
                }
                overlay.Draw(d2d, renderer.Width(), renderer.Height(), stats, audio, showStats, showHelp);
                renderer.EndOverlay();
            }
        }

        switch (renderer.Present()) {
        case PresentResult::Ok:
            break;
        case PresentResult::Occluded:
            Sleep(50);   // fully hidden: vsync won't throttle us, so do it ourselves
            break;
        case PresentResult::DeviceLost:
            host.ReleaseResources();
            overlay.ReleaseDeviceResources();
            if (!renderer.Reinit() || !createDeviceResources()) {
                Fatal(L"The graphics device was lost and could not be recovered.");
                return 1;
            }
            break;
        }

        ++frames;
        statsTimer += dt;
        if (statsTimer >= 1.0f) {
            updateMonitor();
            fps = frames;
            frames = 0;
            statsTimer = 0.0f;
        }
    }
    return 0;
}
