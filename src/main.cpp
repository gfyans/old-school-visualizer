#include "audio/AudioAnalyzer.h"
#include "audio/AudioCapture.h"
#include "media/NowPlaying.h"
#include "platform/FrameLimiter.h"
#include "platform/Settings.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "ui/EqBars.h"
#include "ui/Overlay.h"
#include "viz/VisualizerHost.h"
#include "viz/feedback/FeedbackVisualizer.h"
#include "viz/nebula/NebulaVisualizer.h"
#include "viz/neongrid/NeonGridVisualizer.h"
#include "viz/scope/ScopeVisualizer.h"
#include "viz/spectrum/SpectrumVisualizer.h"

#include <algorithm>
#include <memory>

namespace {

constexpr wchar_t kAppName[] = L"Visualizer";
constexpr float kIdleAfterSilence = 5.0f;   // seconds of silence before dropping to low-power frame rate
constexpr double kIdleFps = 30.0;
constexpr float kAutoCycleSeconds = 240.0f;   // auto-cycle fallback when no song changes are reported

// To add a visualiser: implement IVisualizer and add it here. Order = ←/→ order.
void RegisterVisualizers(VisualizerHost& host) {
    host.Add(std::make_unique<NebulaVisualizer>());
    host.Add(std::make_unique<NeonGridVisualizer>());
    host.Add(std::make_unique<FeedbackVisualizer>());
    host.Add(std::make_unique<ScopeVisualizer>());
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

    EqBars eqBars;
    eqBars.SetVisible(settings.showBars);

    auto createDeviceResources = [&] {
        overlay.CreateDeviceResources(renderer.D2DContext());
        eqBars.CreateResources(renderer.Device());   // optional: bars just won't draw if this fails
        return host.CreateResources(renderer);
    };
    if (!createDeviceResources()) { Fatal(L"Could not create graphics resources."); return 1; }
    if (const int saved = host.FindByName(settings.visualizer); saved > 0) host.Select(saved, false);
    else host.Active().Activate();

    auto capture = std::make_unique<AudioCapture>();
    auto analyzer = std::make_unique<AudioAnalyzer>();
    float samples[AudioAnalyzer::kFftSize];
    constexpr size_t kStreamChunk = AudioCapture::kRingSize / 2;
    auto streamChunk = std::make_unique<float[]>(kStreamChunk);
    constexpr size_t kScopeWindow = 6144;   // > 2 x 25 ms even at 96 kHz (for the scope's trigger search)
    auto scopeMono = std::make_unique<float[]>(kScopeWindow);
    auto scopeSide = std::make_unique<float[]>(kScopeWindow);
    uint64_t lastSampleCount = 0;
    capture->SetPreferredDevice(settings.audioDevice);
    capture->Start();

    // Now playing: title/artist captions and album-art palettes.
    NowPlaying nowPlaying;
    nowPlaying.Start();
    bool albumColours = settings.albumColours, trackTitles = settings.trackTitles;
    TrackInfo track;
    uint32_t seenSerial = 0, seenTrack = 0, seenPalette = 0;
    auto applyPalette = [&](bool newTrack) {
        host.SetTrackPalette(albumColours && track.hasPalette ? &track.palette : nullptr, newTrack);
    };

    bool showStats = false, showHelp = false;
    bool autoCycle = settings.autoCycle;
    float sinceSwitch = 0.0f;
    auto announce = [&] { overlay.ShowMessage(host.Active().Name()); sinceSwitch = 0.0f; };

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
        settings.showBars = eqBars.Visible();
        settings.albumColours = albumColours;
        settings.trackTitles = trackTitles;
        settings.autoCycle = autoCycle;
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
        case 'B':
            eqBars.SetVisible(!eqBars.Visible());
            overlay.ShowMessage(eqBars.Visible() ? L"Equaliser bars on" : L"Equaliser bars off", 1.5f);
            break;
        case 'A':
            albumColours = !albumColours;
            applyPalette(false);
            overlay.ShowMessage(albumColours ? (track.hasPalette ? L"Album colours on" : L"Album colours on (none for this track)")
                                             : L"Album colours off", 2.0f);
            break;
        case 'C':
            autoCycle = !autoCycle;
            sinceSwitch = 0.0f;
            overlay.ShowMessage(autoCycle ? L"Auto-cycle on: a new visualiser with each song" : L"Auto-cycle off", 2.5f);
            break;
        case 'T':
            trackTitles = !trackTitles;
            if (trackTitles && !track.title.empty()) overlay.ShowTrack(track.title, track.artist);
            else overlay.ShowMessage(L"Track titles off", 1.5f);
            break;
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
        // Everything new since last frame feeds the beat grid (it works on a fixed 100 Hz hop).
        const size_t fresh = static_cast<size_t>(std::min<uint64_t>(sampleCount - lastSampleCount, kStreamChunk));
        if (fresh > 0) {
            capture->GetLatest(streamChunk.get(), fresh);
            analyzer->ProcessStream(streamChunk.get(), fresh, static_cast<float>(capture->SampleRate()));
        }
        capture->GetLatest(samples, AudioAnalyzer::kFftSize);
        analyzer->Update(samples, static_cast<float>(capture->SampleRate()), sampleCount != lastSampleCount, dt);
        capture->GetLatestStereo(scopeMono.get(), scopeSide.get(), kScopeWindow);
        analyzer->UpdateScope(scopeMono.get(), scopeSide.get(), kScopeWindow, static_cast<float>(capture->SampleRate()), dt);
        lastSampleCount = sampleCount;
        const AudioFeatures& audio = analyzer->Features();

        silentTime = audio.silent ? silentTime + dt : 0.0f;
        if (lowPower != (silentTime > kIdleAfterSilence)) {
            lowPower = !lowPower;
            applyFrameRate();
        }

        // --- Now playing ---
        if (nowPlaying.Serial() != seenSerial) {
            seenSerial = nowPlaying.Serial();
            track = nowPlaying.Get();
            const bool newTrack = track.trackSerial != seenTrack;
            const bool newPalette = track.paletteSerial != seenPalette;
            seenTrack = track.trackSerial;
            seenPalette = track.paletteSerial;
            if (newTrack && trackTitles) overlay.ShowTrack(track.title, track.artist);
            if (newTrack || newPalette) applyPalette(newTrack);
            // Auto-cycle: a new visualiser with each new song (not for the song already
            // playing when the app starts).
            if (newTrack && autoCycle && track.trackSerial > 1) {
                host.SelectRandom();
                sinceSwitch = 0.0f;
            }
        }
        sinceSwitch += dt;
        if (autoCycle && sinceSwitch > kAutoCycleSeconds && !audio.silent) {
            host.SelectRandom();
            announce();
        }

        // --- Visuals ---
        host.Update(audio, dt);
        std::wstring message = host.Active().TakeMessage();
        if (!message.empty()) overlay.ShowMessage(message);

        renderer.UploadAudio(audio);
        host.Render(renderer, static_cast<float>(time), dt);   // leaves the back buffer bound

        float accentLo[3], accentHi[3];
        host.Active().AccentColours(accentLo, accentHi);
        eqBars.Update(audio, accentLo, accentHi, dt);
        eqBars.Render(renderer);

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
                overlay.Draw(d2d, renderer.Width(), renderer.Height(), stats, audio, showStats, showHelp,
                             eqBars.Visible() ? 230.0f : 0.0f);
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
            eqBars.ReleaseResources();
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
