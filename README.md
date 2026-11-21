# Visualizer

A modern take on the Winamp-era music visualiser. A single small `.exe` with no installer and no runtime to install.
It listens to whatever your PC is playing (Spotify, a browser, games…) and renders it on the GPU.

* **Nebula**: fly through a glowing volumetric gas cloud that reacts to the music.
* **Neon Grid**: a synthwave night drive under neon gates, past low-poly mountains and a city skyline, towards a striped sun. The mountains are built from the music:
  each row of terrain is one moment's spectrum, entering at the horizon.
* **Liquid**: Milkdrop-style feedback. Waveform ink is warped, faded and hue-shifted every frame into
  tunnels, vortices, ripples and kaleidoscopes.
* **Oscilloscope**: a vector display on an old CRT. The real waveform drives an electron beam with phosphor
  afterglow, behind curved glass. Modes: triggered waveform, X/Y vectorscope (the stereo field), dual trace and polar.
* **Aurora**: soft ribbons of northern lights sweeping over a frozen forest lake, mirrored in the cracked ice. The calm one.
* **Galaxy**: about a million stars swirling around a black hole, with gravitational lensing. A camera director
  changes shots on the downbeat, and drops make the black hole fire a quasar.
* **Fractal flights**: fly down a winding tunnel through infinite raymarched fractals: Menger halls, a Mandelbox,
  Apollonian foam, coral and a cathedral, with rings and bands of light on the beat and hyperspeed on drops.
* **Spectrum**: classic bar spectrum.

It also understands the music beyond loudness:
* **Beat grid:** it locks onto the song's tempo and *predicts* beats, so pulses land on the beat instead of just after it.
* **Song structure:** it detects build-ups and drops.
* **Now playing:** it shows the track title and takes colours from the album art (Spotify, browsers, any app that
  shows up in Windows' media flyout).

## Keys

| Key | Action |
|---|---|
| `←` `→` | Previous / next visualiser (with a random transition: crossfade, glitch cut, zoom-through, iris or pixel dissolve) |
| `C` | Auto-cycle: a new visualiser with each song (or every 4 minutes without track info) |
| `F`, `Alt+Enter`, double-click | Toggle fullscreen |
| `N` | Next look (Nebula: palette + gas shape; Neon Grid / Aurora: colour scheme; Liquid: preset; Oscilloscope: mode; Galaxy: shape + colours; Fractal flights: world) |
| `Space` | Lock / unlock the current look |
| `M` | Galaxy: next camera shot |
| `Q` | Galaxy: fire the quasar. Fractal flights: hyperspeed (both as on a drop) |
| `B` | Cyberpunk equaliser bars along the bottom (neon LED segments, glitch tears on hard hits) |
| `A` | Album-art colours on / off |
| `T` | Track titles on / off |
| `D` | Cycle audio device: system default → each output → … |
| `F1` | Stats: fps, resolution, GPU time, audio device, level meters |
| `H` | Help |
| `Esc` | Leave fullscreen, or quit |

Window position, fullscreen, the last visualiser, the audio device and the B/A/T/C toggles are remembered in
`%APPDATA%\Visualizer\settings.ini` (delete it to reset).

**No visuals reacting?** Press `F1`. If it says *(silent)* while music plays, your player is outputting to a
different device than the one being captured. This is common with virtual mixers such as SteelSeries Sonar or Voicemeeter.
Press `D` until the device your music plays on is selected.

## How the Nebula reacts

| Music | Visual |
|---|---|
| Bass | Denser, brighter gas; the light core swells |
| Mids | More turbulence; the gas churns faster |
| Treble | Stars twinkle; sparkles in the gas |
| Beat | Shockwave from the core, brightness kick, gentle zoom swell. Locked to the predicted beat once the tempo is found |
| Loudness of the section | Flight speed and overall brightness |
| Build-up | Flight accelerates, gas thins and churns faster, light intensifies |
| Drop | Big shockwave and flash, fast switch to a new look |
| Phrase (8 bars) | Blends to a new look, on the downbeat |
| New song | New look; with album colours on, palettes come from the cover art |
| Silence | Calm slow drift, dimmer, drops to 30 fps |

## Listening: beat grid and structure

* **Tempo and phase.** A 100 Hz onset envelope (spectral flux pooled over 40 log-spaced bands, like a mel onset
  strength) is autocorrelated over 5 s to find the beat period. A comb filter finds where the beats fall, and a
  phase-locked oscillator predicts them, compensating for the ~20 ms it takes to detect an onset.
  Synthetic tests: predicted beats land within 6–19 ms of the kicks at 120–140 BPM; 174 BPM DnB locks at half-time (87).
* **Confidence** comes from phase consistency (a real beat keeps agreeing with itself), scaled down when there are no
  drums. Visuals only follow the grid above ~0.4. `F1` shows BPM and confidence; the lower beat lamp flashes on the
  predicted beat.
* **Drums present** (`percussive`): drum hits make many frequency bands jump at once, while tonal notes light up a few.
* **Build-ups:** the high band rising steadily over several seconds, especially with the bass cut.
* **Drops:** the bass returning after at least 1.5 s away, or a big level jump after a build. These are measured
  in raw dB before auto-gain, so they don't depend on volume. Build-ups and drops are only judged after a 4 s warm-up
  and when drums were present in the last 15 s, so fade-ins and piano crescendos don't count.

### Tuned on real music

The analyzer was checked against real recordings (thrash, boom-bap, techno, electronic, electro/dubstep and solo piano)
with the tools in [`tools/analysis`](tools/analysis/README.md). They replay a recording through the real analyzer and
score it against an offline reference beat tracker. Tempo matched the reference within 0.5 BPM on every song with a beat.
Beat placement is excellent on steady grooves (F ≈ 0.9 on hip-hop and electronic) and weaker on thrash metal and
syncopated techno, where confidence stays honestly low so visuals don't lock onto a shaky grid.

## Performance

Measured on an RTX 3080 at 2560×1440 fullscreen:

* Nebula: about 1.2 ms GPU per frame. It renders at 50% resolution with temporal accumulation (TAA): each frame is
  jittered and blended with the reprojected previous result, which removes raymarch grain and recovers sub-pixel
  detail, so it looks the same as 60% did at about 30% less cost.
* Neon Grid: about 1.7 ms GPU per frame (75% resolution, for crisp lines).
* Liquid: about 0.06 ms GPU per frame.
* Oscilloscope: about 0.2 ms GPU per frame (full resolution, for thin beams).
* Aurora: about 1.2 ms GPU per frame (60% resolution).
* Galaxy: about 0.75 ms GPU per frame for a million particles at full resolution. Each star works out its own orbit in
  the vertex shader, so there are no particle buffers or simulation; the smooth galaxy light is drawn analytically.
* Fractal flights: about 0.3–1.3 ms GPU per frame depending on the world (50% resolution).
* Spectrum: 0.01 ms GPU per frame.
* CPU: under 1% of one core in a window, about 3.5% fullscreen with the stats overlay.

How it stays light:

* **Frame cap:** frames are capped at an even fraction of the display refresh (72 fps on 144 Hz, 60 on 60 Hz).
  The app doesn't depend on vsync, because drivers can force it off.
* **Idle mode:** it drops to 30 fps after 5 s of silence and stops rendering entirely when minimised.
* **Adaptive resolution:** each visualiser is timed with GPU queries. If one exceeds 3 ms per frame, its internal
  resolution drops (down to 30%), so weak integrated GPUs stay smooth.

## Building

Requires Visual Studio 2022+ with **Desktop development with C++**, which includes CMake, Ninja, the Windows SDK and `fxc`.

```powershell
.\build.ps1                 # Release → bin\Visualizer.exe
.\build.ps1 -Run            # build and launch
.\build.ps1 -Test           # build and run the audio-analysis tests
.\build.ps1 -Config Debug   # bin\Visualizer-debug.exe (D3D debug layer if installed)
```

The exe statically links the C runtime and embeds all shaders. Its only dependencies are DLLs that ship with Windows
(Direct3D 11, Direct2D, DirectWrite, WASAPI), and it needs a DirectX 11 GPU (anything since ~2010).

Dev tools:
* `build\Release\vis_record.exe` records what's playing, one WAV per song (git-ignored), and `vis_replay.exe` plus
  `tools\analysis\analyze.py` score the analyzer on those recordings. See [tools/analysis](tools/analysis/README.md).
* `build\Release\capture_probe.exe [seconds]` prints what the loopback capture receives.
* `VISUALIZER_GPU_BUDGET_MS=0.1` (environment variable) forces adaptive resolution to kick in.
* `VISUALIZER_TAA=0` turns off the Nebula's temporal accumulation, and `VISUALIZER_SEED=<n>` makes its look and
  flight path repeatable. Together they allow before/after comparisons.
* `tools\make_icon.ps1` regenerates `res\app.ico`.

## Architecture

```
src/
  main.cpp                  window + loop + keys; RegisterVisualizers() is the list of visualisers
  platform/                 Window (borderless fullscreen), FrameLimiter, Settings
  audio/AudioCapture        WASAPI loopback thread → lock-free ring buffer; device selection
  audio/AudioAnalyzer       FFT → 64 log bands, auto-gain, smoothing, onset beats, build-ups/drops, waveform → AudioFeatures
  audio/BeatTracker         tempo + beat phase: onset envelope, autocorrelation, comb filter, phase-locked oscillator
  media/NowPlaying          Windows media sessions (C++/WinRT): title, artist, album art
  media/TrackPalette        album art → deep / mid / hot palette
  render/Renderer           D3D11 device, flip-model swap chain, composite pass, Direct2D overlay target
  render/GpuTimer           non-blocking GPU timestamp queries
  viz/Visualizer.h          the plug-in interface (IVisualizer)
  viz/VisualizerHost        offscreen targets, crossfades, adaptive resolution
  viz/nebula/, viz/neongrid/, viz/feedback/, viz/scope/, viz/aurora/, viz/galaxy/, viz/fractal/, viz/spectrum/
  ui/Overlay                messages, now-playing caption, F1 stats, H help (DirectWrite)
  ui/EqBars                 optional equaliser bars drawn over any visualiser
shaders/                    HLSL, compiled at build time into headers
tests/                      analyzer_test (synthetic signals), capture_probe
```

Per frame:
1. The capture thread fills the ring buffer.
2. The analyzer reads the latest 2048 samples and produces `AudioFeatures`.
3. The host calls the visualiser's `Update` and then `Render`, drawing into its own offscreen target.
4. The composite pass scales the result to the window, dithers it, and crossfades if a switch is in progress.
5. The overlay is drawn only if it has something to show, then the frame is presented.

### Adding a visualiser

1. Create `src/viz/<name>/<Name>Visualizer.{h,cpp}` implementing `IVisualizer` (see `SpectrumVisualizer` for the minimum):
   * `CreateResources` / `ReleaseResources`: create your shaders and buffers (also called again after GPU device loss).
   * `Update(audio, dt)`: animate from `AudioFeatures` (all values 0–1).
   * `Render(renderer, time, dt)`: bind your resources (slots `b2+`, `t0+`, `s0+` are free) and call `renderer.DrawFullscreen(ps)`.
   * Optionally: `RenderScale()` (e.g. `0.5` for soft looks), `OnKey`, `TakeMessage`, `Status`,
     `AccentColours` (tints the EQ bars), `SetTrackPalette` (album colours, song changes).
   * Multi-pass effects can render into their own textures and restore `renderer.BoundTarget()`
     (see `FeedbackVisualizer`).
2. Add the shader to `CMakeLists.txt` with `compile_shader(...)` and the `.cpp` to the executable sources.
3. Register it in `RegisterVisualizers()` in `main.cpp`. Registration order is the `←`/`→` order.

Every shader can read `iTime`, `iResolution` (b0) and the audio features (b1) from `shaders/common.hlsli`:
`aBands`, `aLevel`, `aIntensity`, `aBeat`, `aTempo` (bpm, beat phase, bar phase, confidence),
`aStructure` (build-up, drop, …), `Spectrum(i)` / `SpectrumAt(x)` and `Waveform(i)` / `WaveformAt(x)`.
