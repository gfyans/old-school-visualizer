#include "common.hlsli"

// Neon Grid: a cyberpunk flight over a wireframe landscape built from the music.
// Each terrain row is one moment's spectrum (bass beside the road, treble further
// out), pushed in at the horizon and scrolling towards the camera, so the song
// literally forms the mountains. Above: a striped sun behind a neon city skyline.
// Rendering: per-pixel heightfield raymarch, analytic anti-aliased grid lines.

cbuffer Neon : register(b2) {   // matches NeonConstants in NeonGridVisualizer.cpp
    float travel;      float camHeight;   float pitch;       float roll;
    float rowSpacing;  float farDist;     float maxHeight;   float tanHalfFov;
    float pulse;       float buildUp;     float glitch;      float glitchSeed;
    float sunRadius;   float roadLights;  float treble;      float horizonGlow;
    float3 skyTop;     float flash;
    float3 skyHorizon; float sunStripes;
    float3 sunTop;     float pulseZ;      // distance of the beat ring travelling towards us
    float3 sunBottom;  float pad1;
    float3 gridA;      float pad2;        // lines running into the distance
    float3 gridB;      float pad3;        // lines across
    float3 groundCol;  float pad4;
};

Texture2D<float> History : register(t0);   // 64 bands x 128 rows (rows = distance)
SamplerState Wrap : register(s0);

static const float kRoad = 1.6;        // half-width of the road
static const float kFlat = 4.5;        // flat neon grid either side of the road, out to here
static const float kSideWidth = 26.0;  // terrain width each side: treble near, bass far
static const int kRows = 128;

float Spectrum2D(float band01, float z) {
    float row = (z - farDist) / rowSpacing;   // row k sits at k*spacing + farDist
    return History.SampleLevel(Wrap, float2(band01 * (63.0 / 64.0) + 0.5 / 64.0, (row + 0.5) / kRows), 0);
}

float Height(float x, float zWorld) {
    float ax = abs(x);
    float side = smoothstep(kFlat, kFlat + 3.0, ax);
    if (side <= 0.0) return 0.0;
    float d = saturate((ax - kFlat) / kSideWidth);   // 0 near the road .. 1 far out
    // Treble makes low ridges near the road; bass raises the big mountains far out.
    // Left and right read slightly different moments so the valley isn't a mirror image.
    float s = Spectrum2D(1.0 - d, zWorld + (x < 0.0 ? 0.5 : 0.0));
    float hills = 0.5 + 0.5 * sin(x * 0.45 + zWorld * 0.13) * sin(zWorld * 0.31 - x * 0.2);
    return maxHeight * side * (0.08 * hills + pow(saturate(s), 1.4) * (0.15 + 1.1 * d * d));
}

float Line(float coord, float width) {   // distance to the nearest integer line -> intensity
    float d = abs(frac(coord + 0.5) - 0.5);
    return smoothstep(width, 0.0, d) + 0.35 * exp(-d / (width * 3.0));
}

float3 Sky(float3 rd, float2 pix) {
    float elev = rd.y;
    float3 col = lerp(skyHorizon, skyTop, saturate(elev * 2.2));
    col += skyHorizon * horizonGlow * exp(-abs(elev) * 18.0);

    // Stars in the upper sky.
    if (elev > 0.08) {
        float2 q = rd.xy / max(rd.z, 0.05) * 90.0;
        int3 cell = int3(floor(q), 3);
        float3 h = Hash33(cell + 5000);
        float d = length(frac(q) - (0.2 + 0.6 * h.yz));
        float tw = 0.6 + 0.4 * sin(iTime * (2.0 + 5.0 * h.y) + h.z * 40.0);
        col += step(0.93, h.x) * smoothstep(0.12, 0.0, d) * tw * (0.5 + treble) * saturate((elev - 0.08) * 6.0);
    }

    // Sun: gradient disc, horizontal gaps in its lower half that scroll downwards.
    float2 s = float2(rd.x / rd.z, rd.y / rd.z) - float2(0.0, 0.13);
    float r = length(s);
    if (r < sunRadius) {
        float v = s.y / sunRadius;   // -1 bottom .. 1 top
        float3 sun = lerp(sunBottom, sunTop, saturate(v * 0.5 + 0.5));
        float gapZone = saturate(-v * 1.1 + 0.1);                    // from just above the middle down
        float stripe = frac(v * sunStripes - iTime * 0.25);
        float gap = step(stripe, 0.08 + gapZone * 0.5);              // gaps widen towards the bottom
        gap *= step(v, 0.1);
        col = lerp(col, sun * 1.4, 1.0 - gap);
    }
    col += lerp(sunBottom, sunTop, 0.4) * exp(-max(r - sunRadius, 0.0) * 9.0) * 0.35 * (1.0 + 0.4 * pulse);

    // City skyline in front of the sun.
    float sx = rd.x / rd.z;
    float sy = rd.y / rd.z;
    float colW = 0.035;
    float id = floor(sx / colW);
    float3 hb = Hash33(int3((int)id, 11, 7));
    float bh = 0.012 + 0.09 * hb.x * hb.x * (1.0 - smoothstep(0.3, 1.4, abs(sx)));
    if (sy < bh && sy > -0.01) {
        float3 building = skyTop * 0.6;
        // Windows: a sparse grid of lit cells.
        int3 w = int3((int)floor(sx / (colW * 0.18)), (int)floor(sy / 0.006), 23);
        float3 hw = Hash33(w);
        building += lerp(gridB, sunTop, hw.y) * step(0.82, hw.x) * 0.6;
        col = building;
        // Neon edge along the rooftop.
        col += gridA * exp(-(bh - sy) * 900.0) * 0.8;
    }
    return col;
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((i.uv.x * 2.0 - 1.0) * aspect, 1.0 - i.uv.y * 2.0);

    // Drop glitch: some horizontal slices jump sideways for a moment.
    float slice = floor(i.uv.y * 40.0);
    float3 g = Hash33(int3((int)slice, (int)(glitchSeed * 1000.0), 77));
    sp.x += step(1.0 - 0.5 * glitch, g.x) * (g.y - 0.5) * 0.3 * glitch;

    // Camera: looking down the road, slight pitch and roll.
    float cr = cos(roll), sr = sin(roll);
    sp = float2(cr * sp.x - sr * sp.y, sr * sp.x + cr * sp.y);
    float cp = cos(pitch), spch = sin(pitch);
    float3 fwd = float3(0.0, spch, cp);
    float3 up = float3(0.0, cp, -spch);
    float3 rd = normalize(fwd + (sp.x * float3(1, 0, 0) + sp.y * up) * tanHalfFov);
    float3 ro = float3(0.0, camHeight, 0.0);

    float3 col = Sky(rd, i.pos.xy);

    // --- Heightfield raymarch (z relative to the camera; world z = travel + z) ---
    float t = 0.3;
    bool hit = false;
    float prevT = t;
    [loop] for (int s = 0; s < 90; ++s) {
        float3 p = ro + rd * t;
        float h = Height(p.x, travel + p.z);
        float gap = p.y - h;
        if (gap < 0.0) { hit = true; break; }
        prevT = t;
        t += clamp(gap * 0.55, 0.04, 1.5);
        if (t > farDist) break;
    }
    if (hit) {
        // Refine between the last point above and the first below the surface.
        float a = prevT, b = t;
        [unroll] for (int k = 0; k < 5; ++k) {
            float m = 0.5 * (a + b);
            float3 p = ro + rd * m;
            if (p.y < Height(p.x, travel + p.z)) b = m; else a = m;
        }
        t = b;
        float3 p = ro + rd * t;
        float zWorld = travel + p.z;
        float h = Height(p.x, zWorld);

        // Line width grows with distance (about one pixel), so lines stay crisp but don't alias.
        float width = t * (2.0 * tanHalfFov / iResolution.y) * 1.3 + 0.004;
        float ax = abs(p.x);
        float3 surface = groundCol * (0.6 + 2.5 * h / maxHeight);
        float alongLines = Line(p.x, width);                    // run into the distance
        float acrossLines = Line(zWorld / rowSpacing, width / rowSpacing);
        float3 lines = gridA * alongLines + gridB * acrossLines * (1.0 + 1.2 * pulse);

        // Beat ring: a bright band rushing towards the camera on each beat.
        float ring = exp(-abs(p.z - pulseZ) * 1.5) * pulse;
        lines += gridB * ring * 2.0;

        if (ax < kRoad) {
            // The road: dark, dashed centre line, neon edges, strobing edge lights.
            surface = groundCol * 0.4;
            lines = 0;
            float dash = step(frac(zWorld * 0.25), 0.5) * smoothstep(0.06, 0.03, ax);
            float edge = exp(-abs(ax - kRoad + 0.05) / (width * 1.5));
            float lightsOn = step(frac(zWorld * 0.25 + 0.125), 0.12) * exp(-abs(ax - kRoad + 0.15) * 20.0);
            lines = float3(0.9, 0.9, 1.0) * dash * 0.6 + gridA * edge * 1.2 + sunTop * lightsOn * roadLights * 3.0;
        }
        // A faint sheen of the horizon glow on the ground, strongest far away.
        surface += skyHorizon * 0.18 * smoothstep(5.0, farDist, t) * horizonGlow;
        float3 ground = surface + lines;
        float fog = smoothstep(farDist * 0.35, farDist * 0.95, t);
        col = lerp(ground, skyHorizon * (0.6 + horizonGlow), fog);
    }

    col += lerp(gridA, float3(1, 1, 1), 0.5) * flash * 0.35;
    col *= 0.92 + 0.08 * step(0.5, frac(i.pos.y * 0.5));      // faint scanlines
    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.6;                              // vignette
    col = 1.0 - exp(-col * 1.25);                              // soft clip
    return float4(col, 1);
}
