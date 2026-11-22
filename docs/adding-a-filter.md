# Adding a filter

A filter is a look laid over whichever visualiser is showing: ASCII, VHS, Thermal and so on. `X` cycles through them
and they're listed under **Filter** in the right-click menu.

Most filters are a single pixel shader. The host draws the finished scene into a texture, and your shader turns it
into the picture on screen.

`shaders/filter_vhs_ps.hlsl` is a good complete example to copy from.

## 1. Write the shader

Create `shaders/filter_<name>_ps.hlsl`:

```hlsl
#include "filter_common.hlsli"

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;              // this pixel, in the area being drawn
    float3 c = ScenePx(px + float2(FilterTear(px.y), 0.0));
    c = 1.0 - c;                                 // your look here
    return FilterOut(c, i.pos.xy);
}
```

The scene is the visualiser's picture after any transition, without the mood colour grade, since the filter decides
the colours itself. Read it with:

* `Scene(uv)`: `uv` goes from 0 to 1 across the area being drawn (one monitor, or the whole window).
* `ScenePx(px)`: the same, taking a pixel position.
* `SceneCell(centre, size)`: the brightest of five samples across a cell, so a filter built from cells doesn't lose
  thin lines.

You also get:

* `glitch`: how hard the music is hitting right now, from 0 to 1 (snares, build-ups and drops), falling away fast.
* `filterTime`: seconds, wrapping every 1000.
* `FilterPalette()`: four colours (`bg`, `mid`, `hi`, `accent`) that change with each song and each drop.
* `FilterTear(row)`: how far to tear a row sideways on hits. It's 0 when the music is quiet. Leave it out if your
  look shouldn't tear (Polaroid doesn't).
* `FilterScale()`: multiply sizes by this so they look the same at any resolution (sizes are given at 1080p).
* `Luma`, `Hue`, `Bayer8`, `Hash33` and the glyph, LED and scan-line helpers in `cyber.hlsli`.
* Everything from `common.hlsli`: `iResolution` and all the audio (`aBands`, `aBeat`, `aStructure` and the rest).
  See the "What shaders get" section of [adding-a-visualiser.md](adding-a-visualiser.md).

End with `FilterOut(colour, i.pos.xy)`, which adds a touch of noise to hide banding in dark gradients.

Keep big moments for drops (`aStructure.y` is the drop pulse) rather than every kick.

## 2. Add it to the build

In `CMakeLists.txt`, add your name to the `foreach(filter ...)` list next to the other filters. It's compiled to
`g_filter_<name>_ps`.

## 3. Register it

Include `g_filter_<name>_ps.h` in `src/main.cpp` and add a line to `RegisterFilters()`:

```cpp
shader(L"Negative", g_filter_negative_ps, sizeof(g_filter_negative_ps));
```

The order there is the order `X` goes through them. The name is shown in the menu, on screen and in the settings
file, so keep it the same once it has shipped.

## Filters that need more than a shader

Implement `IFilter` (in `src/filter/Filter.h`) yourself, or subclass `ShaderFilter`, and register it with
`filters.Add(std::make_unique<YourFilter>())`. You can:

* keep state in `Update(audio, dt)`, which is called every frame while your filter is selected
* bind your own constant buffers and textures in `Apply(renderer)` before `renderer.DrawFullscreen(ps)`. Slots
  `b3` and up, `t1` and up, and `s1` and up are yours.
* create and free them in `CreateResources(device)` and `ReleaseResources()`, which are called again if the GPU
  device is lost
* return false from `InRandom()` to keep Random mode from switching to your filter

## Visualisers that shouldn't be filtered

A visualiser that already has a look of its own can return false from `IVisualizer::AllowsFilter()`. Signal does this.
