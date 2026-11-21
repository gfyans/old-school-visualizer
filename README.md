# Visualizer

A modern take on the Winamp-era music visualiser. A single small `.exe` with no installer and no runtime to install.
It listens to whatever your PC is playing (Spotify, a browser, games…) and renders it on the GPU.

* **Nebula**: fly through a glowing volumetric gas cloud lit by a pulsing core, with light scattering and bloom. A pulsar's beams sweep in time with the bar, and drops set off a supernova.
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
* **Signal**: cyberpunk glitch art. A cyber skull or a gas-mask head (nodding with the vocals, eyes flaring on
  snares) or a brain turns in the dark, drawn as ASCII glyphs, hard-palette dithering, LED cells
  or torn scan lines, over falling data rain. It alternates between one subject close up (with a dim grid of them
  behind) and a scrolling grid of them filling the screen, each turning in a wave with kicks rippling across. Snares tear it, hi-hats flicker the
  glyphs, build-ups dissolve it, drops flash it inside out and flood the screen.
* **Detonation**: a vast city at night around a glowing spire. Kicks send rings of light across the rooftops,
  build-ups charge the spire and spiral the clouds, and a drop (or a build-up letting go, or a big phrase) erupts a
  white dome that tears at the buildings on its rim and blows a ring through the clouds, then turns to a black
  sphere whose burning rim punches out with every kick before it collapses into a crater the city rebuilds. Night, manga Ink or Dusk looks; the camera cuts between an orbit, an avenue
  flight, a rooftop skim, an overhead view and a distant witness.
* **Spectrum**: the hi-fi scene, an honest 64-band analyser shown two ways: **Studio** (glossy 3D bars on a
  mirror floor in a dark room, with peak caps that fall under gravity and bounce), **Radial** (a ring of bars round a
  glowing core, every kick rippling an echo of it outward). On a drop every peak cap launches like a firework and bursts into sparks.

It also understands the music beyond loudness:
* **Beat grid:** it locks onto the song's tempo and *predicts* beats, so pulses land on the beat instead of just after it.
  Every scene also moves faster for faster songs.
* **Song structure:** it detects build-ups and drops.
* **Instruments:** kick, snare, hi-hats, voice and sustained pads are picked out and drive their own details.
* **Key and mood:** it finds the key and the chords; the picture warms for bright, energetic music, cools for calm or
  minor-key music, and chord changes nudge the hue.
* **Now playing:** it shows the track title and takes colours from the album art (Spotify, browsers, any app that
  shows up in Windows' media flyout).

And it fits around you:
* **Record what you see, with the music**, to MP4: press `R` to record, or turn on instant replay (`I`) and press `S`
  to save the last 30 seconds.
* **Multiple monitors:** fullscreen on any monitor, a different visualiser on every monitor, or one spread across them all.
* **Stream mode** for OBS: clean output with no on-screen text, or just the EQ bars on chroma-key green.

## Keys

Right-click anywhere (or press the menu key) for a menu with every setting, each with its key alongside, so
there's nothing to remember. `H` shows the few keys worth knowing plus the current visualiser's own.

| Key | Action |
|---|---|
| Right-click | The menu: visualiser, scene changes, the visualiser's own actions, effects, display, audio device, recording |
| `←` `→` | Previous / next visualiser (with a random transition: crossfade, glitch cut, zoom-through, iris or pixel dissolve) |
| `C` | Scene changes: off → a new visualiser with each song (or every 4 minutes without track info) → **Random**: a random visualiser and look about every minute, switched at the start of a phrase or on a drop, never in the middle of a blast, supernova or quasar |
| `F`, `Alt+Enter`, double-click | Toggle fullscreen |
| `N` | Next look (Nebula: palette + gas shape; Neon Grid / Aurora: colour scheme; Liquid: preset; Oscilloscope: mode; Galaxy: shape + colours; Fractal flights: world; Signal: subject, style + palette; Detonation: Night / Ink / Dusk; Spectrum: Studio / Radial) |
| `Space` | Lock / unlock the current look |
| `M` | Galaxy, Detonation and Spectrum's Studio: next camera shot. Signal: centre / grid. Spectrum's Radial: colours |
| `Q` | Galaxy: fire the quasar. Fractal flights: hyperspeed. Nebula: supernova. Detonation: detonate. Spectrum: fireworks (all as on a drop) |
| `P` | Nebula: summon a pulsar |
| `B` | Cyberpunk equaliser bars along the bottom (neon LED segments, glitch tears on hard hits) |
| `R` | Record to MP4 (press again to stop and save) |
| `I` / `S` | Instant replay on / off (keeps the last 30 s ready) / save the last 30 s |
| `V` | Choose the folder videos are saved to |
| `W` | Multiple monitors: one monitor → a different visualiser on each → one spanning all |
| `G` | Fullscreen on the next monitor |
| `K` | Mood colour on / off |
| `X` | Glitch filter over any visualiser: ASCII → dither → LED → scan lines → off (the palette changes on drops and songs) |
| `O` | Stream mode: clean (no on-screen text, 1920×1080 window) → EQ bars on green → off |
| `A` | Album-art colours on / off |
| `T` | Track titles on / off |
| `D` | Cycle audio device: system default → each output → … |
| `F1` | Stats: fps, resolution, GPU time, audio device, tempo, level and instrument meters |
| `U` | Check for updates, or restart into one that's been downloaded |
| `H` | Help |
| `Esc` | Leave fullscreen, or quit |

Window position, fullscreen, the monitor setup, the last visualiser, the audio device and the B/A/T/C/I/K toggles
are remembered in `%APPDATA%\Visualizer\settings.ini` (delete it to reset).

## Updates

The app keeps itself up to date from this repository's GitHub releases. At start-up (at most once a day) it checks
for a newer release in the background. If there is one, it downloads the new `Visualizer.exe` next to the current one
and checks it against the release's SHA-256 checksum; only a matching download is kept, and it's checked again
just before it's used. When you close the app the new version takes over, so it starts next time; press `U` to
restart into it straight away (or to check right now). Nothing changes while it's running.

To turn updates off, set `AutoUpdate=0` in `%APPDATA%\Visualizer\settings.ini`. The app has to be able to write to its
own folder (e.g. not inside Program Files) to update itself; otherwise it says so.

## Recording

Videos are MP4 (H.264 video, AAC stereo audio). They go to `Videos\Visualizer` (for example
`C:\Users\you\Videos\Visualizer`) unless you choose another folder with `V`; the message after saving shows the full
path. The audio is exactly what the visualiser
hears, so it's in sync with the picture. On-screen text (messages, stats, help, the REC indicator) isn't recorded; the
visuals and the EQ bars are.

* `R` starts a recording; press it again to stop and save it.
* `I` turns on **instant replay**: the last 30 seconds are always kept (in `%TEMP%\Visualizer`), and `S` saves them.
  It's off until you turn it on, and remembered.
* Recordings are up to 2560×1440 at 60 fps (bigger windows are scaled down). The frames are converted to the encoder's
  format on the GPU and encoded by the GPU's video encoder where there is one, so it costs about 20% of one CPU core
  while recording or buffering.

## Multiple monitors

With more than one monitor, `W` cycles what fullscreen covers: **one monitor**, **every monitor with a different
visualiser** (the arrows change the primary monitor's; choosing one that's already on another monitor swaps them;
with auto-cycle on, every monitor gets a new one each song), or **one visualiser spanning all of them**. `G` moves
single-monitor fullscreen to the next monitor. The EQ bars and on-screen text stay on the primary monitor.

## Stream mode

`O` cycles stream mode, for capturing in OBS (window capture works best):
* **Clean:** no on-screen text at all, and the window becomes exactly 1920×1080 (when not fullscreen).
* **EQ bars on green:** only the equaliser bars, over pure green for a chroma-key filter: overlay them on your stream.
* **Off.** The title bar shows which mode is on.

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
| Drop | A supernova ahead: flash, lens flare and a shockwave that lights the gas as the flight rushes through it; fast switch to a new look |
| Phrase (8 bars) | Blends to a new look on the downbeat; the flight swoops to a new heading; sometimes a pulsar appears |
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
* **Tempo sets the pace.** Every scene animates faster for faster songs and slower for slow ones: ×1 at
  120 BPM, about ×0.75 at 85 and ×1.3 at 170 (clamped to 0.7–1.4). When the grid locks at half-time (drum & bass
  counted at 87), the rate of heard hits reveals the real tempo. `F1` shows the factor next to the BPM.
* **Drums present** (`percussive`): drum hits make many frequency bands jump at once, while tonal notes light up a few.
* **Instruments** (lightweight DSP, no machine learning): the kick is the low onset beat; a **snare/clap** is the
  150–350 Hz body and the 1.5–5 kHz snap jumping together; a **hi-hat** is a jump in 7–16 kHz on its own. Each band
  has its own adaptive threshold and must be audible in the mix. **Voice vs pad**: how tonal the 300–3400 Hz range
  is and whether its loudness pulses at syllable rate (3–8 Hz). Drums are reliable; voice vs synth is rough
  (distorted vocals barely register). Hi-hats drive the sparkle in every scene; snares send smaller ripples and rings;
  vocals brighten the light; pads add drift.
* **Key, chords and mood:** a pitch-class profile (chroma, from spectral peaks) is matched against the
  Krumhansl-Kessler key profiles over ~10 s, and against major and minor triads over ~0.4 s for chord changes.
  A major key can be read as its relative minor (same notes), so the mood leans on it only as far as it's sure.
  The mood mixes major/minor, tempo, how big the section is and how bright it sounds. `K` turns the colour grade off.
* **Build-ups:** the high band rising steadily over several seconds, especially with the bass cut.
* **Drops:** the bass returning after at least 1.5 s away, or a big level jump after a build. These are measured
  in raw dB before auto-gain, so they don't depend on volume. Build-ups and drops are only judged after a 4 s warm-up
  and when drums were present in the last 15 s, so fade-ins and piano crescendos don't count.

### Tuned on real music

The analyzer was checked against real recordings (thrash, boom-bap, techno, electronic, electro/dubstep and solo piano)
with the tools in [`tools/analysis`](tools/analysis/README.md). They replay a recording through the real analyzer and
score it against an offline reference beat tracker. Tempo matched the reference within 0.5 BPM on every song with a beat
(except a 1% drift on one electro track). Beat placement is excellent on steady grooves (F ≈ 0.9 on hip-hop and
electronic) and weaker on thrash metal and syncopated techno, where confidence stays honestly low so visuals don't
lock onto a shaky grid. Trust builds faster than it falls, so the grid locks sooner after a beatless intro (mean grid
F 0.43 → 0.47; the electro track locks 6 s sooner).

## Performance

Measured on an RTX 3080 at 2560×1440 fullscreen:

* Nebula: about 1.65 ms GPU per frame. It renders at 50% resolution with temporal accumulation (TAA): each frame is
  jittered and blended with the reprojected previous result, which removes raymarch grain and recovers sub-pixel
  detail, so it looks the same as 60% did at about 30% less cost.
* Neon Grid: about 1.7 ms GPU per frame (75% resolution, for crisp lines).
* Liquid: about 0.06 ms GPU per frame.
* Oscilloscope: about 0.2 ms GPU per frame (full resolution, for thin beams).
* Aurora: about 1.2 ms GPU per frame (60% resolution).
* Galaxy: about 0.75 ms GPU per frame for a million particles at full resolution. Each star works out its own orbit in
  the vertex shader, so there are no particle buffers or simulation; the smooth galaxy light is drawn analytically.
* Fractal flights: about 0.3–1.3 ms GPU per frame depending on the world (50% resolution).
* Detonation: about 1.5 ms GPU per frame (80% resolution; the city is traced cell by cell, exactly).
* Signal: about 0.2–1.9 ms GPU per frame at full resolution (a model is drawn once per grid tile, up to 64;
  the other subjects take one ray per glyph, dot or LED, and scan lines take three).
* Spectrum: light in every look (85% resolution; Studio traces its 64 bars exactly, cell by cell).
* CPU: under 1% of one core in a window, about 3.5% fullscreen with the stats overlay; about 20% more while
  recording or buffering instant replay.

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
(Direct3D 11, Direct2D, DirectWrite, WASAPI, Media Foundation), and it needs a DirectX 11 GPU (anything since ~2010).

Dev tools:
* `build\Release\vis_record.exe` records what's playing, one WAV per song (git-ignored), and `vis_replay.exe` plus
  `tools\analysis\analyze.py` score the analyzer on those recordings. See [tools/analysis](tools/analysis/README.md).
* `build\Release\capture_probe.exe [seconds]` prints what the loopback capture receives.
* `tools\release.ps1 -Notes notes.md` publishes a release (after bumping the version in `CMakeLists.txt` and
  `res\app.rc`): clean build and tests, then the exe, a zip and the `Visualizer.exe.sha256` checksum the updater needs.
* `VISUALIZER_PRETEND_VERSION=1.0.0` makes the app act as an older build, to try the updater against the latest release.
* `build\Release\vis_mp4check.exe clip.mp4` checks a recorded video's audio for dropouts and timestamp gaps (record a steady tone).
* `VISUALIZER_GPU_BUDGET_MS=0.1` (environment variable) forces adaptive resolution to kick in.
* `VISUALIZER_TAA=0` turns off the Nebula's temporal accumulation, and `VISUALIZER_SEED=<n>` makes its look and
  flight path repeatable. Together they allow before/after comparisons.
* `VISUALIZER_FAKE_MONITORS=2` splits one screen into side-by-side pretend monitors, to try the multi-monitor modes.
* `tools\make_icon.ps1` regenerates `res\app.ico`.
* Signal's skull, gas-mask head and brain are third-party models, so they aren't in the repository.
  `python tools\mesh\stl_to_mesh.py model.stl|model.3mf assets\<name>.mesh` converts a binary STL or a 3MF
  (decimated to about 70–120k triangles, with baked crevice shading; `--eyes` places the glowing eyes, `--turn` and
  `--tilt` fix a model that doesn't face the front). The build embeds `assets\cyberskull.mesh`, `gashead.mesh` and
  `brain.mesh` when they're there (re-run CMake after adding one); without them Signal draws a procedural skull and
  brain and leaves the head out. Its exact commands:
  `stl_to_mesh.py cyberskull.stl assets\cyberskull.mesh`, `stl_to_mesh.py gasheada1m.3mf assets\gashead.mesh --eyes
  0.25,0.08,0.17,0.17` and `stl_to_mesh.py brain.stl assets\brain.mesh --eyes none`.
  `VISUALIZER_SIGNAL_LOOK=<style 1-4>,<subject 0-2>[,<grid rows 0-4>]` fixes Signal's look for comparisons
  (subjects 0 skull, 1 gas mask, 2 brain).
* `VISUALIZER_DETONATION=<look 0-2>,<shot 0-4>[,<seconds into a blast>]` fixes Detonation's look and camera shot
  (and optionally holds one moment of a blast).

## Architecture

```
src/
  main.cpp                  window + loop + keys; RegisterVisualizers() is the list of visualisers
  platform/                 Window (borderless fullscreen), FrameLimiter, Settings
  audio/AudioCapture        WASAPI loopback thread → lock-free ring buffer; device selection
  audio/AudioAnalyzer       FFT → 64 log bands, auto-gain, smoothing, onset beats, instruments, key/chords/mood,
                            build-ups/drops, waveform → AudioFeatures
  audio/BeatTracker         tempo + beat phase: onset envelope, autocorrelation, comb filter, phase-locked oscillator
  media/NowPlaying          Windows media sessions (C++/WinRT): title, artist, album art
  media/TrackPalette        album art → deep / mid / hot palette
  render/Renderer           D3D11 device, flip-model swap chain, composite pass, Direct2D overlay target
  render/GpuTimer           non-blocking GPU timestamp queries
  capture/Recorder          MP4 recording + instant replay: GPU NV12 conversion, Media Foundation encoder thread, segment joining
  update/Updater            auto-update from GitHub releases: WinHTTP, SHA-256 check, swap on exit
  viz/Visualizer.h          the plug-in interface (IVisualizer)
  viz/VisualizerHost        offscreen targets, crossfades, adaptive resolution, one region per monitor
  viz/nebula/, viz/neongrid/, viz/feedback/, viz/scope/, viz/aurora/, viz/galaxy/, viz/fractal/, viz/signal/, viz/spectrum/
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

1. Create `src/viz/<name>/<Name>Visualizer.{h,cpp}` implementing `IVisualizer` (see `DetonationVisualizer` for a complete example):
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
