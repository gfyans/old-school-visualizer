# Visualizer

A modern take on the Winamp-era music visualiser. A single small `.exe` with no installer and no runtime to install.
It listens to whatever your PC is playing (Spotify, a browser, games…) and renders it on the GPU.

* **Nebula**: fly through a glowing volumetric gas cloud that reacts to the music.
* **Spectrum**: classic bar spectrum.

## Keys

| Key | Action |
|---|---|
| `←` `→` | Previous / next visualiser (crossfades) |
| `F`, `Alt+Enter`, double-click | Toggle fullscreen |
| `N` | Nebula: next look (palette + gas shape) |
| `Space` | Nebula: lock / unlock the current look |
| `D` | Cycle audio device: system default → each output → … |
| `F1` | Stats: fps, resolution, GPU time, audio device, level meters |
| `H` | Help |
| `Esc` | Leave fullscreen, or quit |

Window position, fullscreen, the last visualiser and the chosen audio device are remembered in
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
| Beat | Shockwave from the core, brightness kick, slight zoom punch |
| Loudness of the section | Flight speed and overall brightness |
| New section / drop | Blends to a new look; also every ~32 beats |
| Silence | Calm slow drift, dimmer, drops to 30 fps |

## Performance

Measured on an RTX 3080 at 2560×1440 fullscreen:

* Nebula: about 0.9 ms GPU per frame, rendered at half resolution and upscaled.
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
* `build\Release\capture_probe.exe [seconds]` prints what the loopback capture receives.
* `VISUALIZER_GPU_BUDGET_MS=0.1` (environment variable) forces adaptive resolution to kick in.
* `tools\make_icon.ps1` regenerates `res\app.ico`.

## Architecture

```
src/
  main.cpp                  window + loop + keys; RegisterVisualizers() is the list of visualisers
  platform/                 Window (borderless fullscreen), FrameLimiter, Settings
  audio/AudioCapture        WASAPI loopback thread → lock-free ring buffer; device selection
  audio/AudioAnalyzer       FFT → 64 log bands, auto-gain, smoothing, beat detection → AudioFeatures
  render/Renderer           D3D11 device, flip-model swap chain, composite pass, Direct2D overlay target
  render/GpuTimer           non-blocking GPU timestamp queries
  viz/Visualizer.h          the plug-in interface (IVisualizer)
  viz/VisualizerHost        offscreen targets, crossfades, adaptive resolution
  viz/nebula/, viz/spectrum/
  ui/Overlay                messages, F1 stats, H help (DirectWrite)
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
   * Optionally: `RenderScale()` (e.g. `0.5` for soft looks), `OnKey`, `TakeMessage`, `Status`.
2. Add the shader to `CMakeLists.txt` with `compile_shader(...)` and the `.cpp` to the executable sources.
3. Register it in `RegisterVisualizers()` in `main.cpp`. Registration order is the `←`/`→` order.

Every shader can read `iTime`, `iResolution` (b0) and the audio features `aBands`, `aLevel`, `aIntensity`, `aBeat`,
`Spectrum(i)` (b1) from `shaders/common.hlsli`.
