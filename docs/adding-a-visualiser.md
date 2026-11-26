# Adding a visualiser

Each visualiser is a class that implements `IVisualizer` (in `src/viz/Visualizer.h`), usually with a pixel shader
doing most of the work. The host takes care of the rest: render targets, crossfades, scaling and adaptive resolution.

`DetonationVisualizer` is a good complete example to copy from.

## 1. Write the class

Create `src/viz/<name>/<Name>Visualizer.h` and `.cpp`. You need to implement:

* `Name()`: the name shown in the menu.
* `CreateResources(device)` / `ReleaseResources()`: create and free your shaders and buffers. These are also called
  again if the GPU device is lost.
* `Update(audio, dt)`: move things along using the `AudioFeatures` (all values are 0–1).
* `Render(renderer, time, dt)`: bind your resources and call `renderer.DrawFullscreen(ps)`. Slots `b2` and up,
  `t0` and up, and `s0` and up are yours.

There are optional ones too:

* `RenderScale()`: the resolution to start at (e.g. `0.75`) if it's heavy; variable resolution then finds what
  the GPU can sustain.
* `MaxRenderScale()`: never draw above this (e.g. `0.75`), even with Full resolution, if the look is soft enough
  that more pixels don't show.
* `OnKey(vk)` and `Actions()`: handle keys like `N`, `M` and `Q`, and list them so they show up in the menu and the
  help screen.
* `Randomise(seed)` and `Busy()`: used by Random mode. `Busy()` returning true stops it switching away in the middle
  of a big moment.
* `TakeMessage()` / `Status()`: on-screen messages and a line for the `F1` stats.
* `SetTrackPalette()`: album-art colours and song changes. `AccentColours()` tints the EQ bars.
* `AllowsFilter()`: return false if the filters (`X`) shouldn't be drawn over your visualiser.

To add a filter instead, see [adding-a-filter.md](adding-a-filter.md).

If you need several passes, render into your own textures and then restore `renderer.BoundTarget()` before the
final draw. `LiquidVisualizer` does this.

## 2. Add it to the build

In `CMakeLists.txt`, add your shader with `compile_shader(...)` next to the others, and add your `.cpp` to the
executable's sources.

## 3. Register it

Add it to `RegisterVisualizers()` in `src/main.cpp`. The order there is the order `←` and `→` go through them.

## What shaders get

Include `shaders/common.hlsli` and you get the time and resolution (`iTime`, `iResolution`) plus the audio:

* `aLevel`, `aIntensity` and `aBands`: how loud it is overall and per band.
* `aBeat`: the beat pulse.
* `aTempo`: BPM, beat phase, bar phase and how confident the tracker is.
* `aStructure`: build-ups and drops.
* `Spectrum(i)` / `SpectrumAt(x)` and `Waveform(i)` / `WaveformAt(x)`: the spectrum and the raw waveform.
