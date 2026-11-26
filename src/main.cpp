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
#include "ui/SettingsWindow.h"
#include "update/Updater.h"
#include "filter/FilterHost.h"
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
#include "viz/glasscity/GlassCityVisualizer.h"
#include "viz/spectrum/SpectrumVisualizer.h"
#include "viz/dots/DotsVisualizer.h"
#include "g_filter_ascii_ps.h"
#include "g_filter_dither_ps.h"
#include "g_filter_halftone_ps.h"
#include "g_filter_led_ps.h"
#include "g_filter_polaroid_ps.h"
#include "g_filter_scan_ps.h"
#include "g_filter_thermal_ps.h"
#include "g_filter_vhs_ps.h"

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
    host.Add(std::make_unique<GlassCityVisualizer>());
    host.Add(std::make_unique<SpectrumVisualizer>());
    host.Add(std::make_unique<DotsVisualizer>());
}

// To add a filter: write shaders/filter_<name>_ps.hlsl and add it here (or implement IFilter for one
// that needs more than a shader). Order = the order X goes through them. See docs/adding-a-filter.md.
void RegisterFilters(FilterHost& filters) {
    auto shader = [&](const wchar_t* name, const BYTE* bytecode, size_t size) {
        filters.Add(std::make_unique<ShaderFilter>(name, bytecode, size));
    };
    shader(L"ASCII", g_filter_ascii_ps, sizeof(g_filter_ascii_ps));
    shader(L"Dither", g_filter_dither_ps, sizeof(g_filter_dither_ps));
    shader(L"LED", g_filter_led_ps, sizeof(g_filter_led_ps));
    shader(L"Scan lines", g_filter_scan_ps, sizeof(g_filter_scan_ps));
    shader(L"VHS", g_filter_vhs_ps, sizeof(g_filter_vhs_ps));
    shader(L"Polaroid", g_filter_polaroid_ps, sizeof(g_filter_polaroid_ps));
    shader(L"Halftone", g_filter_halftone_ps, sizeof(g_filter_halftone_ps));
    shader(L"Thermal", g_filter_thermal_ps, sizeof(g_filter_thermal_ps));
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
    if (wchar_t scene[128] = {}; GetEnvironmentVariableW(L"VISUALIZER_SCENE", scene, ARRAYSIZE(scene)) > 0)
        settings.visualizer = scene;   // dev: start on this visualiser

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
    FilterHost filters;
    RegisterFilters(filters);
    Overlay overlay;
    overlay.Init();

    EqBars eqBars;
    eqBars.SetVisible(settings.showBars);

    auto createDeviceResources = [&] {
        overlay.CreateDeviceResources(renderer.D2DContext());
        eqBars.CreateResources(renderer.Device());   // optional: bars just won't draw if this fails
        const bool filtersOk = filters.CreateResources(renderer.Device());
        return host.CreateResources(renderer) && filtersOk;
    };
    if (!createDeviceResources()) { Fatal(L"Could not create graphics resources."); return 1; }
    filters.Select(filters.FindByName(settings.filter));
    if (const int saved = host.FindByName(settings.visualizer); saved > 0) host.Select(saved, false);
    else host.Active().Activate();
    host.RestoreScales(settings.renderScales);
    host.SetFullResolution(settings.fullResolution);

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
    bool albumColours = settings.albumColours;
    // On-screen text (T): none, song titles only, or titles and notices.
    int onScreenText = settings.onScreenText;
    overlay.SetTextMode(onScreenText);
    static const wchar_t* kTextItems[] = { L"None", L"Song titles only", L"All" };
    TrackInfo track;
    uint32_t seenSerial = 0, seenTrack = 0, seenPalette = 0;
    auto applyPalette = [&](bool newTrack) {
        host.SetTrackPalette(albumColours && track.hasPalette ? &track.palette : nullptr, newTrack);
    };

    bool showStats = false, showHelp = false;
    bool screenshotPending = false;   // F12: saved once this frame is drawn
    // Stream mode (O), for OBS. All but Off hide the on-screen text and make the window 1920 x 1080:
    // 1 clean, 2 transparent (the window is see-through where the picture is dark), 3 only the EQ bars
    // on a transparent window, 4 only the EQ bars on chroma-key green.
    enum StreamMode { kStreamOff, kStreamClean, kStreamTransparent, kStreamBarsTransparent, kStreamBarsGreen, kStreamModes };
    int streamMode = kStreamOff;
    static const wchar_t* kStreamItems[] = { L"Off", L"Clean (no on-screen text, 1920 x 1080)",
                                             L"Transparent: see-through where it's dark", L"Equaliser bars, transparent",
                                             L"Equaliser bars on green (chroma key)" };
    auto barsOnly = [&] { return streamMode == kStreamBarsTransparent || streamMode == kStreamBarsGreen; };
    // Mood colour: warm for bright/energetic music, cool for calm/minor; chords nudge the hue.
    bool moodColour = settings.moodColour;
    float gradeWarmth = 0.0f, gradeHue = 0.0f, gradeHueTarget = 0.0f, gradeAmount = moodColour ? 1.0f : 0.0f;
    uint32_t seenChord = 0;
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
    // A new visualiser: no name on screen (it's in the menu and F1), just restart the scene timers.
    auto announce = [&] { sinceSwitch = 0.0f; barsSinceSwitch = 0; };
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
        if (rng() % 3 == 0) filters.SelectRandom(rng());   // a new filter now and then (if one is on)
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
    // Fullscreen keeps the display on (no screensaver or monitor sleep); windowed, Windows decides.
    auto keepDisplayOn = [](bool on) {
        SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    };
    keepDisplayOn(window.IsFullscreen());
    window.onFullscreenChanged = [&](bool fullscreen) {
        keepDisplayOn(fullscreen);
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
        settings.onScreenText = onScreenText;
        settings.sceneChanges = sceneChanges;
        settings.instantReplay = recorder.InstantReplay();
        settings.videoFolder = videoFolder;
        settings.moodColour = moodColour;
        settings.filter = filters.CurrentName();
        settings.displayMode = displayMode;
        settings.monitor = fullscreenMonitor;
        settings.renderScales = host.LearnedScales();
        settings.fullResolution = host.FullResolution();
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
    // A see-through window needs a window made for it and a composition swap chain: swap both in
    // (the GPU device, and everything made on it, stay as they are).
    auto setTransparent = [&](bool on) {
        if (on == window.IsTransparent()) return true;
        auto attach = [&] {
            int cw, ch;
            window.ClientSize(cw, ch);
            return renderer.SetWindow(window.Handle(), cw, ch, window.IsTransparent());
        };
        if (window.Recreate(on) && attach()) return true;
        // No composition on this PC (or the window couldn't be made): back to a normal window.
        if (window.IsTransparent()) window.Recreate(false);
        if (!attach()) { Fatal(L"The window could not be recreated."); PostQuitMessage(1); }
        return false;
    };
    auto setStreamMode = [&](int mode) {
        if (mode == streamMode) return;
        if (streamMode == kStreamOff && mode != kStreamOff) window.SetClientSize(1920, 1080);
        if (mode == kStreamOff) window.RestoreSize();
        const bool transparent = mode == kStreamTransparent || mode == kStreamBarsTransparent;
        if (!setTransparent(transparent) && transparent) {
            mode = mode == kStreamTransparent ? kStreamClean : kStreamBarsGreen;
            MessageBoxW(window.Handle(), L"This PC can't show a see-through window, so it's using the nearest stream mode instead.",
                        kAppName, MB_OK | MB_ICONINFORMATION);
        }
        // The bars-only modes switch the bars on; leaving them puts them back as they were.
        static bool barsBefore = false;
        const bool wasBarsOnly = barsOnly();
        streamMode = mode;
        if (barsOnly() && !wasBarsOnly) { barsBefore = eqBars.Visible(); eqBars.SetVisible(true); }
        if (!barsOnly() && wasBarsOnly) eqBars.SetVisible(barsBefore);
        // In stream mode on-screen text is hidden, so say it in the title bar instead.
        window.SetTitle(streamMode ? (std::wstring(L"Stream mode: ") + kStreamItems[streamMode]).c_str() : kAppName);
        if (streamMode == kStreamOff) overlay.ShowMessage(L"Stream mode off", 1.5f);
    };
    auto setTextMode = [&](int mode) {
        onScreenText = mode;
        overlay.SetTextMode(mode);
        overlay.ClearTransient();
        // Always confirmed (it's the one notice "None" lets through), with the song when titles are on.
        overlay.ShowMessage(std::wstring(L"On-screen text: ") + kTextItems[mode], 2.0f, true);
        if (mode != Overlay::kTextNone && !track.title.empty()) overlay.ShowTrack(track.title, track.artist);
    };
    auto chooseVideoFolder = [&](HWND owner) {
        const std::wstring chosen = Window::PickFolder(owner, L"Save videos to", recorder.OutputFolder());
        if (chosen.empty()) return false;
        videoFolder = chosen;
        recorder.SetOutputFolder(videoFolder);
        saveSettings();
        return true;
    };
    std::wstring updateStatus;   // the updater's latest notice, shown in the settings window

    // --- The settings window (, or right-click > Settings): everything you set and leave ---------
    SettingsWindow settingsWindow;
    {
        using Item = SettingItem;
        auto choice = [](std::wstring label, std::function<std::vector<std::wstring>()> options, std::function<int()> get,
                         std::function<void(int)> set, std::function<bool()> enabled = {}) {
            Item i; i.kind = Item::Choice; i.label = std::move(label); i.options = std::move(options);
            i.get = std::move(get); i.set = std::move(set); i.enabled = std::move(enabled); return i;
        };
        auto fixed = [](std::initializer_list<const wchar_t*> list) {
            std::vector<std::wstring> v(list.begin(), list.end());
            return [v] { return v; };
        };
        auto toggle = [](std::wstring label, std::function<int()> get, std::function<void(int)> set, std::function<bool()> enabled = {}) {
            Item i; i.kind = Item::Toggle; i.label = std::move(label); i.get = std::move(get); i.set = std::move(set);
            i.enabled = std::move(enabled); return i;
        };
        auto action = [](std::function<std::wstring()> text, std::function<std::wstring()> button, std::function<void(int)> press,
                         std::function<bool()> enabled = {}) {
            Item i; i.kind = Item::Action; i.text = std::move(text); i.button = std::move(button); i.set = std::move(press);
            i.enabled = std::move(enabled); return i;
        };
        auto multiMonitor = [] { return Window::Monitors().size() >= 2; };
        // Lists the window shows by position; kept here so a choice maps back to the right entry.
        auto monitorNames = std::make_shared<std::vector<std::wstring>>();
        auto deviceIds = std::make_shared<std::vector<std::wstring>>();

        std::vector<Item> items;
        items.push_back(Item::MakeHeading(L"Display"));
        items.push_back(choice(L"On-screen text", fixed({ L"All: song titles and notices", L"Song titles only", L"None" }),
            [&] { return 2 - onScreenText; }, [&](int i) { setTextMode(2 - i); }));
        items.push_back(choice(L"Fullscreen across",
            fixed({ L"One monitor", L"Every monitor, each its own visualiser", L"Every monitor, one visualiser spanning" }),
            [&] { return displayMode; },
            [&](int i) { displayMode = i; applyDisplay(); }, multiMonitor));
        items.push_back(choice(L"Fullscreen on",
            [monitorNames] {
                const std::vector<MonitorDesc> monitors = Window::Monitors();
                monitorNames->clear();
                std::vector<std::wstring> labels{ L"The monitor the window is on" };
                for (size_t m = 0; m < monitors.size(); ++m) {
                    monitorNames->push_back(monitors[m].name);
                    labels.push_back(L"Monitor " + std::to_wstring(m + 1) + (monitors[m].primary ? L" (main)" : L""));
                }
                return labels;
            },
            [&, monitorNames] {
                for (size_t m = 0; m < monitorNames->size(); ++m) if ((*monitorNames)[m] == fullscreenMonitor) return static_cast<int>(m) + 1;
                return 0;
            },
            [&, monitorNames](int i) {
                fullscreenMonitor = i > 0 && i - 1 < static_cast<int>(monitorNames->size()) ? (*monitorNames)[i - 1] : std::wstring();
                applyDisplay();
            },
            [&, multiMonitor] { return multiMonitor() && displayMode == 0; }));
        items.push_back(choice(L"Resolution",
            fixed({ L"Variable: as sharp as the GPU keeps smooth", L"Full: every scene at its sharpest" }),
            [&] { return host.FullResolution() ? 1 : 0; }, [&](int i) { host.SetFullResolution(i == 1); }));
        items.push_back(choice(L"Stream mode", [] { return std::vector<std::wstring>(std::begin(kStreamItems), std::end(kStreamItems)); },
            [&] { return streamMode; }, [&](int i) { setStreamMode(i); }));

        items.push_back(Item::MakeHeading(L"Picture"));
        items.push_back(choice(L"Scene changes", fixed({ L"Off", L"A new visualiser with each song", L"Random: a new scene every minute or so" }),
            [&] { return sceneChanges; }, [&](int i) { setSceneChanges(i); }));
        items.push_back(choice(L"Filter",
            [&] {
                std::vector<std::wstring> v{ L"Off" };
                for (int f = 0; f < filters.Count(); ++f) v.push_back(filters.At(f).Name());
                return v;
            },
            [&] { return filters.Active() ? filters.Current() + 1 : 0; }, [&](int i) { filters.Select(i - 1); }));
        items.push_back(toggle(L"Equaliser bars along the bottom", [&] { return eqBars.Visible() ? 1 : 0; },
            [&](int on) { eqBars.SetVisible(on != 0); }, [&] { return !barsOnly(); }));
        items.push_back(toggle(L"Mood colour: warmer and cooler with the music", [&] { return moodColour ? 1 : 0; },
            [&](int on) { moodColour = on != 0; }));
        items.push_back(toggle(L"Colours from the album art", [&] { return albumColours ? 1 : 0; },
            [&](int on) { albumColours = on != 0; applyPalette(false); }));

        items.push_back(Item::MakeColumn());
        items.push_back(Item::MakeHeading(L"Audio"));
        items.push_back(choice(L"Listen to",
            [&, deviceIds] {
                const std::vector<AudioDevice> devices = AudioCapture::ListOutputDevices();
                deviceIds->clear();
                std::vector<std::wstring> labels{ L"The system's default output" };
                for (const AudioDevice& d : devices) { deviceIds->push_back(d.id); labels.push_back(d.name); }
                return labels;
            },
            [&, deviceIds] {
                const std::wstring current = capture->PreferredDevice();
                for (size_t d = 0; d < deviceIds->size(); ++d) if ((*deviceIds)[d] == current) return static_cast<int>(d) + 1;
                return 0;
            },
            [&, deviceIds](int i) {
                capture->SetPreferredDevice(i > 0 && i - 1 < static_cast<int>(deviceIds->size()) ? (*deviceIds)[i - 1] : std::wstring());
            }));

        items.push_back(Item::MakeHeading(L"Recording"));
        items.push_back(toggle(L"Instant replay: keep the last 30 seconds (S saves)", [&] { return recorder.InstantReplay() ? 1 : 0; },
            [&](int on) { recorder.SetInstantReplay(on != 0); }, [&] { return canRecord; }));
        items.push_back(action(
            [&] { return canRecord ? L"Videos go to " + Recorder::ShortPath(recorder.OutputFolder(), 40) : std::wstring(L"Recording isn't available on this PC"); },
            [] { return std::wstring(L"Change…"); },
            [&](int) { chooseVideoFolder(settingsWindow.Handle()); }, [&] { return canRecord; }));

        items.push_back(Item::MakeHeading(L"Updates"));
        items.push_back(toggle(L"Check for updates and install them on exit", [&] { return settings.autoUpdate ? 1 : 0; },
            [&](int on) { settings.autoUpdate = on != 0; }));
        items.push_back(action(
            [&] {
                if (updater.Ready()) return L"Version " + updater.ReadyVersion() + L" is ready";
                if (!updateStatus.empty()) return updateStatus;
                return std::wstring(L"This is version ") + Updater::CurrentVersion();
            },
            [&] { return std::wstring(updater.Ready() ? L"Restart now" : L"Check now"); },
            [&](int) {
                if (updater.Ready()) {
                    saveSettings();
                    if (updater.RestartNow()) window.Close();
                    else updateStatus = L"Couldn't install the update";
                } else {
                    settings.lastUpdateCheck = updater.Start(settings.lastUpdateCheck, true);
                    updateStatus = L"Checking for updates…";
                }
            }));
        settingsWindow.SetItems(std::move(items));
    }
    settingsWindow.onHidden = saveSettings;
    auto openSettings = [&] { settingsWindow.Show(window.Handle()); };
    window.onKeyDown = [&](UINT vk) {
        switch (vk) {
        case 'F':        window.ToggleFullscreen(); break;
        case VK_ESCAPE:  if (window.IsFullscreen()) window.SetFullscreen(false); else window.Close(); break;
        case VK_UP:      nowPlaying.Send(NowPlaying::Command::PlayPause); overlay.ShowMessage(L"Play / pause", 1.2f); break;
        case VK_DOWN:    nowPlaying.Send(NowPlaying::Command::Next); overlay.ShowMessage(L"Next track", 1.2f); break;
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
        case 'O':        setStreamMode((streamMode + 1) % kStreamModes); break;
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
            filters.Next();   // no name on screen: it's ticked in the menu
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
            chooseVideoFolder(window.Handle());
            if (wasFullscreen) window.SetFullscreen(true);
            overlay.ShowMessage(L"Videos are saved to " + Recorder::ShortPath(recorder.OutputFolder()), 4.0f);
            break;
        }
        case 'T':
            setTextMode((onScreenText + 2) % 3);   // All -> Song titles only -> None -> All
            break;
        case VK_OEM_COMMA: openSettings(); break;
        default:         host.Active().OnKey(vk); break;
        }
        settingsWindow.Refresh();   // if it's open, show what the key changed
    };

    // --- The right-click menu: quick actions, with their keys alongside (settings are in their window) ---
    window.onContextMenu = [&](POINT at) {
        enum : UINT { kViz = 1000, kFilter = 1200, kPrevTrack = 1600, kSettings = 1700 };
        auto keyName = [](UINT vk) -> std::wstring {
            switch (vk) {
            case VK_SPACE: return L"Space";
            case VK_LEFT: return L"\u2190";
            case VK_RIGHT: return L"\u2192";
            case VK_UP: return L"\u2191";
            case VK_DOWN: return L"\u2193";
            case VK_F1: return L"F1";
            case VK_F12: return L"F12";
            case VK_ESCAPE: return L"Esc";
            case VK_OEM_COMMA: return L",";
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
        item(menu, kSettings, L"Settings…", VK_OEM_COMMA);
        SetMenuDefaultItem(menu, kSettings, FALSE);   // bold: the way in to everything else
        sep(menu);

        HMENU vizMenu = CreatePopupMenu();
        for (int i = 0; i < host.Count(); ++i) radio(vizMenu, kViz + i, host.At(i).Name(), i == host.Current());
        sep(vizMenu);
        item(vizMenu, VK_LEFT, L"Previous", VK_LEFT);
        item(vizMenu, VK_RIGHT, L"Next", VK_RIGHT);
        sub(menu, vizMenu, L"Visualiser");

        HMENU filterMenu = CreatePopupMenu();
        radio(filterMenu, kFilter, L"Off", !filters.Active());
        for (int i = 0; i < filters.Count() && i < 99; ++i) radio(filterMenu, kFilter + 1 + i, filters.At(i).Name(), i == filters.Current());
        sub(menu, filterMenu, L"Filter	X");

        HMENU music = CreatePopupMenu();
        item(music, VK_UP, L"Play / pause", VK_UP);
        item(music, VK_DOWN, L"Next track", VK_DOWN);
        item(music, kPrevTrack, L"Previous track");
        sub(menu, music, L"Music");

        // The visualiser's own keys.
        const std::vector<VisualizerAction> actions = host.Active().Actions();
        if (!actions.empty()) {
            sep(menu);
            for (const VisualizerAction& a : actions) item(menu, a.key, a.label, a.key);
        }
        sep(menu);

        item(menu, 'F', L"Fullscreen", 'F', window.IsFullscreen());
        HMENU record = CreatePopupMenu();
        item(record, 'R', recorder.Recording() ? L"Stop and save the recording" : L"Record", 'R', false, canRecord);
        item(record, 'S', L"Save the last 30 seconds", 'S', false, canRecord && recorder.InstantReplay());
        sep(record);
        item(record, VK_F12, L"Save a screenshot", VK_F12);
        sub(menu, record, L"Record");
        sep(menu);

        item(menu, VK_F1, L"Stats", VK_F1, showStats);
        item(menu, 'H', L"Help", 'H', showHelp);
        if (updater.Ready()) item(menu, 'U', L"Restart to update", 'U');
        sep(menu);
        item(menu, 1, window.IsFullscreen() ? L"Leave fullscreen" : L"Quit", VK_ESCAPE);

        SetForegroundWindow(window.Handle());
        const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, window.Handle(), nullptr);
        DestroyMenu(menu);   // and its submenus
        if (cmd == 0) return;
        if (cmd == 1) window.onKeyDown(VK_ESCAPE);
        else if (cmd == kPrevTrack) { nowPlaying.Send(NowPlaying::Command::Previous); overlay.ShowMessage(L"Previous track", 1.2f); }
        else if (cmd == kSettings) openSettings();
        else if (cmd >= kFilter && cmd < kFilter + 100) filters.Select(static_cast<int>(cmd - kFilter) - 1);
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

    overlay.ShowMessage(L"Right-click for the menu and settings, H for help", 4.0f);
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
            if (settingsWindow.HandleDialogMessage(msg)) continue;   // Tab, Esc etc. in the settings window
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
            if (newTrack) overlay.ShowTrack(track.title, track.artist);   // the overlay drops it if text is off
            if (newTrack) filters.NewTrack();
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
        filters.Update(audio, dt);
        renderer.UploadAudio(audio);
        if (streamMode == kStreamBarsGreen) renderer.ClearBackBuffer(0.0f, 1.0f, 0.0f);   // bars only: no visualiser
        else if (streamMode == kStreamBarsTransparent) renderer.ClearBackBuffer();         // (black: see-through)
        else host.Render(renderer, static_cast<float>(time), dt, &filters);   // leaves the back buffer bound

        float accentLo[3], accentHi[3];
        host.Active().AccentColours(accentLo, accentHi);
        eqBars.Update(audio, accentLo, accentHi, dt);
        const RECT primary = host.PrimaryRegion();
        renderer.BindBackBuffer(host.HasRegions() ? &primary : nullptr);
        eqBars.Render(renderer);
        recorder.CaptureFrame(renderer);   // what's recorded: the visuals and bars, not the text overlays
        auto takeScreenshot = [&](bool keepAlpha) {
            screenshotPending = false;
            const std::wstring shot = Screenshot::Save(renderer, primary, keepAlpha);
            overlay.ShowMessage(shot.empty() ? std::wstring(L"Couldn't save the screenshot") : L"Screenshot saved to " + Recorder::ShortPath(shot), 3.0f);
        };
        // Likewise the screenshot (a transparent window's waits for its alpha, below, and keeps it).
        if (screenshotPending && !window.IsTransparent()) takeScreenshot(false);
        if (std::wstring note = recorder.TakeMessage(); !note.empty()) overlay.ShowMessage(note, 4.0f);
        if (std::wstring note = updater.TakeMessage(); !note.empty()) { overlay.ShowMessage(note, 4.0f); updateStatus = note; }
        if (nowPlaying.TakeNoPlayer()) overlay.ShowMessage(L"No music player to control", 2.5f);
        if (recorder.Recording()) {
            const int secs = static_cast<int>(recorder.RecordingSeconds());
            wchar_t rec[32];
            swprintf_s(rec, L"REC  %d:%02d", secs / 60, secs % 60);
            overlay.SetIndicator(rec, true);
        } else {
            overlay.SetIndicator(L"", false);
        }

        // Dev benchmark: VISUALIZER_BENCH_LOG=<file> appends the active scene's average GPU time
        // (seconds 5-25, after warm-up) and quits. Pair with VISUALIZER_SCENE and VISUALIZER_RENDER_SCALE.
        {
            static std::wstring benchLog = [] {
                wchar_t buf[MAX_PATH] = {};
                return GetEnvironmentVariableW(L"VISUALIZER_BENCH_LOG", buf, MAX_PATH) > 0 ? std::wstring(buf) : std::wstring();
            }();
            static float benchTime = 0.0f, benchSum = 0.0f;
            static int benchFrames = 0;
            if (!benchLog.empty()) {
                // Pictures to judge the quality by (taken next frame) at 8, 13, 18 and 23 s.
                const bool shotDue = std::floor((benchTime - 3.0f) / 5.0f) != std::floor((benchTime + dt - 3.0f) / 5.0f) && benchTime > 5.0f;
                benchTime += dt;
                if (shotDue) screenshotPending = true;
                if (benchTime > 5.0f && host.ActiveGpuMs() > 0.0f) { benchSum += host.ActiveGpuMs(); ++benchFrames; }
                if (benchTime > 25.0f) {
                    FILE* f = nullptr;
                    if (_wfopen_s(&f, benchLog.c_str(), L"a") == 0 && f) {
                        fwprintf(f, L"%ls\t%dx%d\t%.3f ms\t%d frames\n", host.Active().Name(), host.ActiveRenderWidth(),
                                 host.ActiveRenderHeight(), benchFrames ? benchSum / benchFrames : 0.0f, benchFrames);
                        fclose(f);
                    }
                    ExitProcess(0);   // no settings save: a benchmark run leaves nothing behind
                }
            }
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

        renderer.FinishTransparentFrame();   // transparent window: brightness becomes alpha (else nothing)
        if (screenshotPending) takeScreenshot(true);   // a PNG with the window's transparency
        switch (renderer.Present()) {
        case PresentResult::Ok:
            break;
        case PresentResult::Occluded:
            Sleep(50);   // fully hidden: vsync won't throttle us, so do it ourselves
            break;
        case PresentResult::DeviceLost:
            host.ReleaseResources();
            filters.ReleaseResources();
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
            settingsWindow.Refresh();   // Random mode changes the filter on its own; updates report in
            fps = frames;
            frames = 0;
            statsTimer = 0.0f;
        }
    }
    updater.ApplyOnExit();   // a downloaded update takes over from the next launch
    return 0;
}
