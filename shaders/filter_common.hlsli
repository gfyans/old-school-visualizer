#ifndef FILTER_COMMON_HLSLI
#define FILTER_COMMON_HLSLI
#include "cyber.hlsli"   // common.hlsli, plus the glyph, dither, LED and tearing helpers shared with Signal

// What every filter shader gets (see src/filter/Filter.h). The finished scene, without mood grade:
Texture2D SceneTexture : register(t0);
SamplerState Linear : register(s0);

// Matches FilterConstants in src/filter/FilterHost.cpp.
cbuffer Filter : register(b2) {
    float2 sceneUvScale;   // this area of the screen within the scene texture
    float2 sceneUvMax;
    float glitch;          // 0..1: how hard the music hits right now (snares, build-ups, drops)
    float filterTime;      // seconds, wrapping every 1000
    float2 filterPad;
    float3 palBg;     float palPad0;   // a four-colour palette that changes with each song and drop
    float3 palMid;    float palPad1;
    float3 palHi;     float palPad2;
    float3 palAccent; float palPad3;
};

// The scene at uv (0..1 across this area of the screen; clamped at the edges).
float3 Scene(float2 uv) { return SceneTexture.SampleLevel(Linear, min(saturate(uv) * sceneUvScale, sceneUvMax), 0).rgb; }
// The scene at a pixel position in this area (iResolution is its size).
float3 ScenePx(float2 px) { return Scene(px / iResolution); }

// The brightest of five samples across a cell (its centre and four points around it), so thin
// bright lines that miss the centre still light the cell instead of flickering in and out.
float3 SceneCell(float2 centrePx, float2 size) {
    float3 best = ScenePx(centrePx);
    float bestL = Luma(best);
    [unroll] for (int k = 0; k < 4; ++k) {
        float2 d = float2((k & 1) ? 0.3 : -0.3, (k & 2) ? 0.3 : -0.3) * size;
        float3 c = ScenePx(centrePx + d);
        float l = Luma(c);
        if (l > bestL) { best = c; bestL = l; }
    }
    return best;
}

CyberPalette FilterPalette() {
    CyberPalette pal;
    pal.bg = palBg; pal.mid = palMid; pal.hi = palHi; pal.accent = palAccent;
    return pal;
}

// How far a row is torn sideways on hits, in pixels (0 when the music is quiet).
float FilterTear(float row) { return CyberTear(row, filterTime, glitch, 0.0); }

// Scale for sizes given at 1080p.
float FilterScale() { return iResolution.y / 1080.0; }

// Every filter ends with this: a touch of noise hides 8-bit banding in dark gradients.
float4 FilterOut(float3 c, float2 screenPx) {
    return float4(c + (InterleavedGradientNoise(screenPx) - 0.5) / 255.0, 1.0);
}

#endif
