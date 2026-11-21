#include "audio/AudioAnalyzer.h"
#include "audio/AudioCapture.h"
#include "capture/Recorder.h"
#include "capture/Screenshot.h"
#include "media/NowPlaying.h"
#include "platform/FrameLimiter.h"
#include "platform/Settings.h"
#include "platform/Window.h"
#include "render/Renderer.h"
#include "ui/EqBars.h"
#include "ui/Overlay.h"
#include "update/Updater.h"
#include "viz/VisualizerHost.h"
#include "viz/liquid/LiquidVisualizer.h"
#include "viz/nebula/NebulaVisualizer.h"
#include "viz/neongrid/NeonGridVisualizer.h"
#include "viz/scope/ScopeVisualizer.h"
#include "viz/aurora/AuroraVisualizer.h"
#include "viz/galaxy/GalaxyVisualizer.h"
#include "viz/fractal/FractalVisualizer.h"
#include "viz/signal/SignalVisualizer.h"
#include "viz/detonation/DetonationVisualizer.h"
#include "viz/spectrum/SpectrumVisualizer.h"

#include <algorithm>
#include <memory>
#include <random>

namespace {

constexpr wchar_t kAppName[] = L"Visualizer";
constexpr float kIdleAfterSilence = 5.0f;   // seconds of silence before dropping to low-power frame rate
constexpr double kIdleFps = 30.0;
constexpr float kAutoCycleSeconds = 240.0f;   // auto-cycle fallback when no song changes are reported
constexpr float kRandomSeconds = 60.0f;       // Random: about this long on each scene (then the next phrase)

// To add a visualiser: implement IVisualizer and add it here. Order = ←/→ order.
void RegisterVisualizers(VisualizerHost& host) {
    host.Add(std::make_unique<NebulaVisualizer>());
    host.Add(std::make_unique<NeonGridVisualizer>());
    host.Add(std::make_unique<LiquidVisualizer>());
    host.Add(std::make_unique<ScopeVisualizer>());
    host.Add(std::make_unique<AuroraVisualizer>());
    host.Add(std::make_unique<GalaxyVisualizer>());
    host.Add(std::make_unique<FractalVisualizer>());
    host.Add(std::make_unique<SignalVisualizer>());
    host.Add(std::make_unique<DetonationVisualizer>());
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // An update downloaded last time but not swapped in (the app didn't close cleanly): do it now.
    if (Updater::ApplyAtStartup()) return 0;
    const bool justUpdated = cmdLine && wcsstr(cmdLine, L"--updated");
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

    // Updates from GitHub releases: checked in the background, installed when the app closes.
    Updater updater;

    // Video recording (MP4 with the music): instant replay and start/stop.
    Recorder recorder;
    const bool canRecord = recorder.Init(capture.get());
    if (canRecord) recorder.SetInstantReplay(settings.instantReplay);
    std::wstring videoFolder = settings.videoFolder;
    recorder.SetOutputFolder(videoFolder);

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
    bool screenshotPending = false;   // F12: saved once this frame is drawn
    // Stream mode (O): 1 = clean output for OBS (no on-screen text, a 1920x1080 window),
    // 2 = only the EQ bars on chroma-key green, to overlay on a stream.
    int streamMode = 0;
    // Mood colour: warm for bright/energetic music, cool for calm/minor; chords nudge the hue.
    bool moodColour = settings.moodColour;
    float gradeWarmth = 0.0f, gradeHue = 0.0f, gradeHueTarget = 0.0f, gradeAmount = moodColour ? 1.0f : 0.0f;
    uint32_t seenChord = 0;
    // Glitch filter (X): ASCII / dither / LED / scan over any visualiser, palette changing on drops and songs.
    int glitchFilter = settings.glitchFilter, glitchPalette = 0;
    float glitchAmount = 0.0f;
    uint32_t seenDrop = 0;
    static const float kGlitchPalettes[4][4][3] = {   // bg, mid, hi, accent
        { { 0.035f, 0.008f, 0.10f }, { 1.00f, 0.16f, 0.43f }, { 0.82f, 0.97f, 1.00f }, { 0.02f, 0.85f, 0.91f } },   // Noir
        { { 0.015f, 0.04f, 0.02f },  { 0.20f, 0.90f, 0.30f }, { 0.85f, 1.00f, 0.60f }, { 1.00f, 0.90f, 0.10f } },   // Acid
        { { 0.05f, 0.00f, 0.01f },   { 0.90f, 0.06f, 0.12f }, { 1.00f, 0.88f, 0.72f }, { 1.00f, 0.50f, 0.10f } },   // Blood
        { { 0.00f, 0.02f, 0.06f },   { 0.12f, 0.45f, 0.95f }, { 0.90f, 0.97f, 1.00f }, { 0.65f, 0.25f, 1.00f } },   // Ice
    };
    static const wchar_t* kGlitchNames[] = { L"Glitch filter off", L"Glitch filter: ASCII", L"Glitch filter: dither",
                                             L"Glitch filter: LED", L"Glitch filter: scan lines" };
    // Scene changes (C): 0 off, 1 a new visualiser with each song, 2 Random: a random visualiser and look
    // about every minute, switched on a phrase (or on a drop).
    int sceneChanges = settings.sceneChanges;
    static const wchar_t* kSceneNames[] = { L"Scene changes off", L"Scene changes: a new visualiser with each song",
                                            L"Random: a new scene and look about every minute" };
    float sinceSwitch = 0.0f;
    int barsSinceSwitch = 0;
    float lastBarPhase = 0.0f;
    uint32_t seenRandomDrop = 0;
    std::mt19937 rng(std::random_device{}());
    int recent[2] = { -1, -1 };   // the last visualisers Random showed, not to repeat straight away
    auto announce = [&] { overlay.ShowMessage(host.Active().Name()); sinceSwitch = 0.0f; barsSinceSwitch = 0; };
    auto randomScene = [&] {
        std::vector<int> pool;
        for (int i = 0; i < host.Count(); ++i)
            if (i != host.Current() && i != recent[0] && i != recent[1]) pool.push_back(i);
        if (pool.empty()) for (int i = 0; i < host.Count(); ++i) if (i != host.Current()) pool.push_back(i);
        if (pool.empty()) return;
        const int pick = pool[rng() % pool.size()];
        recent[1] = recent[0];
        recent[0] = host.Current();
        host.Select(pick);
        host.Active().Randomise(rng());
        host.ShuffleExtras();
        if (glitchFilter != 0 && rng() % 3 == 0) glitchFilter = 1 + static_cast<int>(rng() % 4);
        announce();
    };

    // D: cycle System default -> each active output device -> back to default.
    auto cycleAudioDevice = [&] {
        const std::vector<AudioDevice> devices = AudioCapture::ListOutputDevices();
        const std::wstring current = capture->PreferredDevice();
        int index = -1;   // -1 = system default
        for (int i = 0; i < static_cast<int>(devices.size()); ++i)
            if (devices[i].id == current) index = i;
        index = index + 1 < static_cast<int>(devices.size()) ? index + 1 : -1;
        capture->SetPreferredDevice(index < 0 ? std::wstring() : devices[index].id);
        overlay.ShowMessage(index < 0 ? std::wstring(L"Audio: system default output") : L"Audio: " + devices[index].name, 3.0f);
    };

    // --- Multi-monitor ------------------------------------------------------------------
    // Fullscreen covers one monitor (mode 0, G picks which), or all of them: each showing its own
    // visualiser (mode 1) or one visualiser stretched across (mode 2). W cycles the modes.
    int displayMode = settings.displayMode;
    std::wstring fullscreenMonitor = settings.monitor;
    auto applyDisplay = [&] {
        const std::vector<MonitorDesc> monitors = Window::Monitors();
        if (monitors.size() < 2 || displayMode == 0) {
            const MonitorDesc* chosen = nullptr;
            for (const MonitorDesc& m : monitors) if (m.name == fullscreenMonitor) chosen = &m;
            window.SetFullscreenArea(chosen ? &chosen->rect : nullptr);
            host.SetRegions({});
            return;
        }
        RECT all = monitors[0].rect;
        for (const MonitorDesc& m : monitors) UnionRect(&all, &all, &m.rect);
        window.SetFullscreenArea(&all);
        std::vector<RECT> regions;
        if (displayMode == 1 && window.IsFullscreen()) {
            for (const MonitorDesc& m : monitors) {
                RECT r = m.rect;
                OffsetRect(&r, -all.left, -all.top);
                regions.push_back(r);
            }
        }
        host.SetRegions(regions);
    };
    window.onFullscreenChanged = [&](bool fullscreen) {
        if (!fullscreen) host.SetRegions({});
        else if (displayMode == 1) {   // regions follow the (new) window: re-derive them
            const std::vector<MonitorDesc> monitors = Window::Monitors();
            if (monitors.size() >= 2) {
                RECT all = monitors[0].rect;
                for (const MonitorDesc& m : monitors) UnionRect(&all, &all, &m.rect);
                std::vector<RECT> regions;
                for (const MonitorDesc& m : monitors) {
                    RECT r = m.rect;
                    OffsetRect(&r, -all.left, -all.top);
                    regions.push_back(r);
                }
                host.SetRegions(regions);
            }
        }
    };
    auto setDisplayMode = [&](int mode) {
        if (Window::Monitors().size() < 2) { overlay.ShowMessage(L"Only one monitor is connected", 2.0f); return; }
        displayMode = mode % 3;
        applyDisplay();
        if (!window.IsFullscreen()) window.SetFullscreen(true);
        static const wchar_t* kNames[] = { L"One monitor", L"Every monitor: a different visualiser on each", L"Spanning every monitor" };
        overlay.ShowMessage(kNames[displayMode], 2.5f);
    };
    auto cycleDisplayMode = [&] { setDisplayMode(displayMode + 1); };
    auto nextMonitor = [&] {
        const std::vector<MonitorDesc> monitors = Window::Monitors();
        if (monitors.size() < 2) { overlay.ShowMessage(L"Only one monitor is connected", 2.0f); return; }
        int index = 0;
        for (int i = 0; i < static_cast<int>(monitors.size()); ++i) if (monitors[i].name == fullscreenMonitor) index = i;
        if (fullscreenMonitor.empty()) {   // start from the one the window is on
            const HMONITOR here = MonitorFromWindow(window.Handle(), MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(here, &mi);
            for (int i = 0; i < static_cast<int>(monitors.size()); ++i) if (monitors[i].name == mi.szDevice) index = i;
        }
        index = (index + 1) % static_cast<int>(monitors.size());
        fullscreenMonitor = monitors[index].name;
        displayMode = 0;
        applyDisplay();
        if (!window.IsFullscreen()) window.SetFullscreen(true);
        overlay.ShowMessage(L"Monitor " + std::to_wstring(index + 1) + L" of " + std::to_wstring(monitors.size()), 2.0f);
    };
    applyDisplay();

    auto saveSettings = [&] {
        window.GetPlacement(settings.windowRect, settings.maximized);
        settings.hasPlacement = true;
        settings.fullscreen = window.IsFullscreen();
        settings.visualizer = host.Active().Name();
        settings.audioDevice = capture->PreferredDevice();
        settings.showBars = eqBars.Visible();
        settings.albumColours = albumColours;
        settings.trackTitles = trackTitles;
        settings.sceneChanges = sceneChanges;
        settings.instantReplay = recorder.InstantReplay();
        settings.videoFolder = videoFolder;
        settings.moodColour = moodColour;
        settings.glitchFilter = glitchFilter;
        settings.displayMode = displayMode;
        settings.monitor = fullscreenMonitor;
        settings.Save();
    };
    window.onClose = saveSettings;   // while the window still exists

    window.onResize = [&](int cw, int ch) {
        renderer.Resize(cw, ch);
        host.Resize(cw, ch);
    };
    auto setSceneChanges = [&](int mode) {
        sceneChanges = mode;
        sinceSwitch = 0.0f;
        barsSinceSwitch = 0;
        overlay.ShowMessage(kSceneNames[sceneChanges], 2.5f);
    };
    auto setStreamMode = [&](int mode) {
        if (mode == streamMode) return;
        if (streamMode == 0 && mode != 0) window.SetClientSize(1920, 1080);
        if (mode == 0) window.RestoreSize();
        streamMode = mode;
        if (streamMode == 2 && !eqBars.Visible()) eqBars.SetVisible(true);
        static const wchar_t* kModes[] = { L"Stream mode off", L"Stream mode: clean (no on-screen text, 1920 x 1080 window)",
                                           L"Stream mode: EQ bars on green (chroma key)" };
        // In stream mode on-screen text is hidden, so say it in the title bar instead.
        window.SetTitle(streamMode ? kModes[streamMode] : kAppName);
        if (streamMode == 0) overlay.ShowMessage(kModes[0], 1.5f);
    };
    auto setAudioDevice = [&](const AudioDevice* device) {
        capture->SetPreferredDevice(device ? device->id : std::wstring());
        overlay.ShowMessage(device ? L"Audio: " + device->name : std::wstring(L"Audio: system default output"), 3.0f);
    };
    window.onKeyDown = [&](UINT vk) {
        switch (vk) {
        case 'F':        window.ToggleFullscreen(); break;
        case VK_ESCAPE:  if (window.IsFullscreen()) window.SetFullscreen(false); else window.Close(); break;
        case VK_RIGHT:   host.Next(); announce(); break;
        case VK_LEFT:    host.Previous(); announce(); break;
        case VK_F1:      showStats = !showStats; break;
        case VK_F12:     screenshotPending = true; break;
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
            setSceneChanges((sceneChanges + 1) % 3);
            break;
        case 'W':        cycleDisplayMode(); break;
        case 'O':        setStreamMode((streamMode + 1) % 3); break;
        case 'U':
            if (updater.Ready()) {
                saveSettings();
                if (updater.RestartNow()) window.Close();
                else overlay.ShowMessage(L"Couldn't install the update", 2.5f);
            } else {
                settings.lastUpdateCheck = updater.Start(settings.lastUpdateCheck, true);
                overlay.ShowMessage(L"Checking for updates…", 2.0f);
            }
            break;
        case 'X':
            glitchFilter = (glitchFilter + 1) % 5;
            overlay.ShowMessage(kGlitchNames[glitchFilter], 1.5f);
            break;
        case 'K':
            moodColour = !moodColour;
            overlay.ShowMessage(moodColour ? L"Mood colour on: the picture warms and cools with the music" : L"Mood colour off", 2.5f);
            break;
        case 'G':        nextMonitor(); break;
        case 'R':
            if (!canRecord) { overlay.ShowMessage(L"Recording isn't available on this PC", 2.5f); break; }
            recorder.ToggleRecording();
            overlay.ShowMessage(recorder.Recording() ? L"Recording" : L"Saving the recording…", 1.5f);
            break;
        case 'I':
            if (!canRecord) { overlay.ShowMessage(L"Recording isn't available on this PC", 2.5f); break; }
            recorder.SetInstantReplay(!recorder.InstantReplay());
            overlay.ShowMessage(recorder.InstantReplay() ? L"Instant replay on: press S to save the last 30 s" : L"Instant replay off", 2.5f);
            break;
        case 'S':
            if (canRecord) recorder.SaveReplay();
            break;
        case 'V': {   // choose where videos are saved
            if (!canRecord) { overlay.ShowMessage(L"Recording isn't available on this PC", 2.5f); break; }
            const bool wasFullscreen = window.IsFullscreen();
            if (wasFullscreen) window.SetFullscreen(false);   // so the dialog isn't hidden behind
            const std::wstring chosen = Window::PickFolder(window.Handle(), L"Save videos to", recorder.OutputFolder());
            if (wasFullscreen) window.SetFullscreen(true);
            if (!chosen.empty()) {
                videoFolder = chosen;
                recorder.SetOutputFolder(videoFolder);
                saveSettings();
            }
            overlay.ShowMessage(L"Videos are saved to " + Recorder::ShortPath(recorder.OutputFolder()), 4.0f);
            break;
        }
        case 'T':
            trackTitles = !trackTitles;
            if (trackTitles && !track.title.empty()) overlay.ShowTrack(track.title, track.artist);
            else overlay.ShowMessage(L"Track titles off", 1.5f);
            break;
        default:         host.Active().OnKey(vk); break;
        }
    };

    // --- The right-click menu: every setting, with its key alongside -------------------------
    window.onContextMenu = [&](POINT at) {
        enum : UINT { kViz = 1000, kScene = 1100, kGlitch = 1200, kDevice = 1300, kDisplay = 1400, kStream = 1500 };
        auto keyName = [](UINT vk) -> std::wstring {
            switch (vk) {
            case VK_SPACE: return L"Space";
            case VK_LEFT: return L"\u2190";
            case VK_RIGHT: return L"\u2192";
            case VK_F1: return L"F1";
            case VK_F12: return L"F12";
            case VK_ESCAPE: return L"Esc";
            default: return std::wstring(1, static_cast<wchar_t>(vk));
            }
        };
        auto item = [&](HMENU m, UINT id, const std::wstring& label, UINT key = 0, bool checked = false, bool enabled = true) {
            const std::wstring text = key ? label + L"\t" + keyName(key) : label;
            AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text.c_str());
        };
        auto radio = [&](HMENU m, UINT id, const std::wstring& label, bool on) {
            AppendMenuW(m, MF_STRING, id, label.c_str());
            if (on) CheckMenuRadioItem(m, id, id, id, MF_BYCOMMAND);
        };
        auto sub = [&](HMENU m, HMENU child, const wchar_t* label) { AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(child), label); };
        auto sep = [](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };

        HMENU menu = CreatePopupMenu();
        HMENU vizMenu = CreatePopupMenu();
        for (int i = 0; i < host.Count(); ++i) radio(vizMenu, kViz + i, host.At(i).Name(), i == host.Current());
        sep(vizMenu);
        item(vizMenu, VK_LEFT, L"Previous", VK_LEFT);
        item(vizMenu, VK_RIGHT, L"Next", VK_RIGHT);
        sub(menu, vizMenu, L"Visualiser");

        HMENU sceneMenu = CreatePopupMenu();
        static const wchar_t* kSceneItems[] = { L"Off", L"A new visualiser with each song", L"Random: a new scene and look every minute or so" };
        for (int i = 0; i < 3; ++i) radio(sceneMenu, kScene + i, kSceneItems[i], i == sceneChanges);
        sub(menu, sceneMenu, L"Scene changes\tC");

        // The visualiser's own keys.
        const std::vector<VisualizerAction> actions = host.Active().Actions();
        if (!actions.empty()) {
            sep(menu);
            for (const VisualizerAction& a : actions) item(menu, a.key, a.label, a.key);
        }
        sep(menu);

        HMENU effects = CreatePopupMenu();
        HMENU glitch = CreatePopupMenu();
        static const wchar_t* kGlitchItems[] = { L"Off", L"ASCII", L"Dither", L"LED", L"Scan lines" };
        for (int i = 0; i < 5; ++i) radio(glitch, kGlitch + i, kGlitchItems[i], i == glitchFilter);
        sub(effects, glitch, L"Glitch filter\tX");
        item(effects, 'B', L"Equaliser bars", 'B', eqBars.Visible());
        item(effects, 'K', L"Mood colour", 'K', moodColour);
        item(effects, 'A', L"Album-art colours", 'A', albumColours);
        item(effects, 'T', L"Track titles", 'T', trackTitles);
        sub(menu, effects, L"Effects");

        HMENU display = CreatePopupMenu();
        item(display, 'F', L"Fullscreen", 'F', window.IsFullscreen());
        const bool multi = Window::Monitors().size() >= 2;
        sep(display);
        static const wchar_t* kDisplayItems[] = { L"One monitor", L"A different visualiser on each monitor", L"Spanning every monitor" };
        for (int i = 0; i < 3; ++i) {
            radio(display, kDisplay + i, kDisplayItems[i], multi && i == displayMode);
            if (!multi) EnableMenuItem(display, kDisplay + i, MF_BYCOMMAND | MF_GRAYED);
        }
        item(display, 'G', L"Next monitor", 'G', false, multi);
        sep(display);
        HMENU stream = CreatePopupMenu();
        static const wchar_t* kStreamItems[] = { L"Off", L"Clean (no on-screen text, 1920 x 1080)", L"Equaliser bars on green" };
        for (int i = 0; i < 3; ++i) radio(stream, kStream + i, kStreamItems[i], i == streamMode);
        sub(display, stream, L"Stream mode\tO");
        sub(menu, display, L"Display");

        HMENU audioMenu = CreatePopupMenu();
        const std::vector<AudioDevice> devices = AudioCapture::ListOutputDevices();
        const std::wstring current = capture->PreferredDevice();
        radio(audioMenu, kDevice, L"System default", current.empty());
        for (int i = 0; i < static_cast<int>(devices.size()) && i < 90; ++i)
            radio(audioMenu, kDevice + 1 + i, devices[i].name, devices[i].id == current);
        sub(menu, audioMenu, L"Audio device\tD");

        HMENU record = CreatePopupMenu();
        item(record, 'R', recorder.Recording() ? L"Stop and save the recording" : L"Record", 'R', false, canRecord);
        item(record, 'I', L"Instant replay", 'I', recorder.InstantReplay(), canRecord);
        item(record, 'S', L"Save the last 30 seconds", 'S', false, canRecord && recorder.InstantReplay());
        item(record, 'V', L"Video folder\u2026", 'V', false, canRecord);
        sep(record);
        item(record, VK_F12, L"Save a screenshot", VK_F12);
        sub(menu, record, L"Record");
        sep(menu);

        item(menu, VK_F1, L"Stats", VK_F1, showStats);
        item(menu, 'H', L"Help", 'H', showHelp);
        item(menu, 'U', updater.Ready() ? L"Restart to update" : L"Check for updates", 'U');
        sep(menu);
        item(menu, 1, window.IsFullscreen() ? L"Leave fullscreen" : L"Quit", VK_ESCAPE);

        SetForegroundWindow(window.Handle());
        const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, window.Handle(), nullptr);
        DestroyMenu(menu);   // and its submenus
        if (cmd == 0) return;
        if (cmd == 1) window.onKeyDown(VK_ESCAPE);
        else if (cmd >= kStream && cmd < kStream + 3) setStreamMode(cmd - kStream);
        else if (cmd >= kDisplay && cmd < kDisplay + 3) setDisplayMode(cmd - kDisplay);
        else if (cmd >= kDevice && cmd < kDevice + 100) setAudioDevice(cmd == kDevice ? nullptr : &devices[cmd - kDevice - 1]);
        else if (cmd >= kGlitch && cmd < kGlitch + 5) { glitchFilter = cmd - kGlitch; overlay.ShowMessage(kGlitchNames[glitchFilter], 1.5f); }
        else if (cmd >= kScene && cmd < kScene + 3) setSceneChanges(cmd - kScene);
        else if (cmd >= kViz && cmd < kViz + 100) { if (static_cast<int>(cmd - kViz) != host.Current()) { host.Select(cmd - kViz); announce(); } }
        else window.onKeyDown(cmd);   // everything else is the key it's labelled with
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

    overlay.ShowMessage(std::wstring(host.Active().Name()) + L"   ·   right-click for the menu, H for help", 4.0f);
    if (justUpdated) overlay.ShowMessage(std::wstring(L"Updated to version ") + Updater::CurrentVersion(), 4.0f);
    if (settings.autoUpdate) settings.lastUpdateCheck = updater.Start(settings.lastUpdateCheck, false);

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
            if (newTrack) glitchPalette = (glitchPalette + 1) % 4;
            if (newTrack || newPalette) applyPalette(newTrack);
            // Auto-cycle: a new visualiser with each new song (not for the song already
            // playing when the app starts).
            if (newTrack && sceneChanges == 1 && track.trackSerial > 1) {
                host.SelectRandom();
                host.ShuffleExtras();
                sinceSwitch = 0.0f;
            }
        }
        sinceSwitch += dt;
        if (sceneChanges == 1 && sinceSwitch > kAutoCycleSeconds && !audio.silent) {
            host.SelectRandom();
            announce();
        }
        // Random: after about a minute, switch at the start of the next 8-bar phrase, or on a drop if one comes
        // first (unless the drop is the current scene's own big moment), never in the middle of a set piece.
        const bool onDownbeat = audio.barPhase < lastBarPhase;
        lastBarPhase = audio.barPhase;
        const bool gridLocked = audio.tempoConfidence > 0.4f;
        if (onDownbeat && gridLocked) ++barsSinceSwitch;
        const bool newDrop = audio.dropCount != seenRandomDrop;
        seenRandomDrop = audio.dropCount;
        if (sceneChanges == 2 && !audio.silent) {
            bool dropMoment = false;
            for (const VisualizerAction& a : host.Active().Actions()) dropMoment |= a.key == 'Q';
            bool go = sinceSwitch > kRandomSeconds && (gridLocked ? onDownbeat && barsSinceSwitch % 8 == 0 : true);
            go |= newDrop && !dropMoment && sinceSwitch > kRandomSeconds * 0.7f;
            go |= sinceSwitch > kRandomSeconds * 1.5f;
            if (host.Active().Busy() && sinceSwitch < kRandomSeconds * 2.5f) go = false;
            if (go) randomScene();
        }

        // --- Visuals ---
        host.Update(audio, dt);
        std::wstring message = host.Active().TakeMessage();
        if (!message.empty()) overlay.ShowMessage(message);

        // Chords move the hue around the circle of fifths: related chords stay close, distant ones
        // swing further (at most about 12 degrees). The mood sets the warmth.
        if (audio.chordCount != seenChord) {
            seenChord = audio.chordCount;
            const int fifths = (audio.chordRoot * 7) % 12;
            gradeHueTarget = 0.21f * std::sin(6.2831853f * fifths / 12.0f) - (audio.chordMinor ? 0.05f : 0.0f);
        }
        gradeHue += (gradeHueTarget - gradeHue) * (1.0f - std::exp(-dt / 1.2f));
        gradeWarmth += (audio.mood - gradeWarmth) * (1.0f - std::exp(-dt / 3.0f));
        gradeAmount += ((moodColour ? 1.0f : 0.0f) - gradeAmount) * (1.0f - std::exp(-dt / 0.5f));
        renderer.SetGrade(gradeWarmth, gradeHue, gradeAmount);
        if (audio.dropCount != seenDrop) { seenDrop = audio.dropCount; glitchPalette = (glitchPalette + 1) % 4; }
        glitchAmount = std::max(glitchAmount * std::exp(-dt / 0.12f),
                                std::min(1.0f, 0.7f * audio.snare + 0.6f * audio.buildUp * audio.buildUp + audio.drop));
        // (Signal already is glitch art: no filter on top of it.)
        const bool signalShown = std::wcscmp(host.Active().Name(), L"Signal") == 0;
        renderer.SetFilter(signalShown ? 0 : glitchFilter, glitchAmount, static_cast<float>(std::fmod(time, 1000.0)), kGlitchPalettes[glitchPalette]);
        renderer.UploadAudio(audio);
        if (streamMode == 2) renderer.ClearBackBuffer(0.0f, 1.0f, 0.0f);   // bars on green: no visualiser
        else host.Render(renderer, static_cast<float>(time), dt);   // leaves the back buffer bound

        float accentLo[3], accentHi[3];
        host.Active().AccentColours(accentLo, accentHi);
        eqBars.Update(audio, accentLo, accentHi, dt);
        const RECT primary = host.PrimaryRegion();
        renderer.BindBackBuffer(host.HasRegions() ? &primary : nullptr);
        eqBars.Render(renderer);
        recorder.CaptureFrame(renderer);   // what's recorded: the visuals and bars, not the text overlays
        if (screenshotPending) {   // likewise the screenshot
            screenshotPending = false;
            const std::wstring shot = Screenshot::Save(renderer, primary);
            overlay.ShowMessage(shot.empty() ? std::wstring(L"Couldn't save the screenshot") : L"Screenshot saved to " + Recorder::ShortPath(shot), 3.0f);
        }
        if (std::wstring note = recorder.TakeMessage(); !note.empty()) overlay.ShowMessage(note, 4.0f);
        if (std::wstring note = updater.TakeMessage(); !note.empty()) overlay.ShowMessage(note, 4.0f);
        if (recorder.Recording()) {
            const int secs = static_cast<int>(recorder.RecordingSeconds());
            wchar_t rec[32];
            swprintf_s(rec, L"REC  %d:%02d", secs / 60, secs % 60);
            overlay.SetIndicator(rec, true);
        } else {
            overlay.SetIndicator(L"", false);
        }

        overlay.Update(dt);
        if (streamMode != 0) { overlay.SetIndicator(L"", false); overlay.ClearTransient(); }   // nothing on screen but what you ask for
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
                if (showHelp) overlay.SetHelpExtras(host.Active().Name(), host.Active().Actions());
                overlay.Draw(d2d, primary.right - primary.left, primary.bottom - primary.top, stats, audio, showStats, showHelp,
                             eqBars.Visible() ? 230.0f : 0.0f, static_cast<float>(primary.left), static_cast<float>(primary.top));
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
            recorder.ReleaseDeviceResources();
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
    updater.ApplyOnExit();   // a downloaded update takes over from the next launch
    return 0;
}
