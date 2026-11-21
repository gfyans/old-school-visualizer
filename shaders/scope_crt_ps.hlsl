#include "common.hlsli"

// Oscilloscope CRT. The glass fills the whole window (no monitor round it):
//   phosphor faintly grainy grey-green when unlit; the trace is a white-hot core fading to a coloured
//            afterglow, with bloom and wide halation in the thick glass
//   grid     a neon graticule in the phosphor's colour, bulging like a tube's face: thin bright lines with a soft glow, the centre
//            cross and its ticks brighter, all swelling a little on the beat
//   glass    darker towards the corners, scanlines, and a faint diagonal sheen

cbuffer Crt : register(b2) {   // matches CrtConstants in ScopeVisualizer.cpp
    float3 coreCol;   float bloom;
    float3 trailCol;  float curvature;
    float3 gridCol;   float flicker;
    float glare;      float gridPulse;  float crtPad1; float crtPad2;
};

Texture2D<float2> Phosphor : register(t0);   // R slow afterglow, G fast core (with mips for bloom)
SamplerState Linear : register(s0);

// Distance in pixels from the nearest line of a grid with `cells` divisions across the screen.
float2 GridDistance(float2 uv, float2 cells) {
    return abs(frac(uv * cells + 0.5) - 0.5) / cells * iResolution;
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 uv = i.uv;
    float2 c = uv * 2.0 - 1.0;
    float2 q = c * float2(aspect, 1.0);

    // Unlit phosphor: very dark grey-green with a fine grain.
    float grain = Hash33(int3(i.pos.xy, 41)).x;
    float3 col = float3(0.018, 0.022, 0.020) * (0.9 + 0.2 * grain);

    // The trace: core, afterglow, bloom and halation.
    float2 p = Phosphor.SampleLevel(Linear, uv, 0);
    float glowR = 0.0, glowG = 0.0;
    [unroll] for (int m = 2; m <= 5; ++m) {
        float2 g = Phosphor.SampleLevel(Linear, uv, m);
        float wgt = 1.0 / (m - 1);
        glowR += g.r * wgt;
        glowG += g.g * wgt;
    }
    float halo = Phosphor.SampleLevel(Linear, uv, 6).r + Phosphor.SampleLevel(Linear, uv, 7).r;
    col += trailCol * p.r * 0.9 + coreCol * p.g * 0.7;
    col += (trailCol * glowR + coreCol * glowG * 0.3) * bloom + trailCol * halo * 0.35;

    // The neon graticule: 10 x 8 divisions, the centre cross with fine ticks. It's etched on the curved
    // glass, so it bulges like a tube's face (barrel distortion), bending away towards the edges.
    float3 neon = trailCol / max(max(trailCol.r, max(trailCol.g, trailCol.b)), 1e-3);
    float2 guv = c * (1.0 + curvature * float2(c.y * c.y, c.x * c.x)) * 0.5 + 0.5;
    float2 gd = GridDistance(guv, float2(10.0, 8.0));
    float d = min(gd.x, gd.y);
    float2 cc = abs(guv - 0.5) * iResolution;
    float dc = min(cc.x, cc.y);
    float2 tick = GridDistance(guv, float2(50.0, 40.0));
    float dt = min(tick.x + step(5.0, cc.y) * 1e3, tick.y + step(5.0, cc.x) * 1e3);
    float lines = smoothstep(1.2, 0.0, d) * 0.6 + exp(-d / 5.0) * 0.2 + exp(-d / 22.0) * 0.06;
    float cross = smoothstep(1.4, 0.0, dc) * 0.85 + exp(-dc / 6.0) * 0.3 + exp(-dc / 30.0) * 0.08;
    float ticks = smoothstep(1.2, 0.0, dt) * 0.5;
    col += neon * (lines + cross + ticks) * (0.55 + 0.35 * gridPulse) * (0.75 + 0.25 * glare);

    col *= 0.88 + 0.12 * step(0.5, frac(i.pos.y * 0.5));                  // scanlines
    col *= 1.0 - 0.35 * pow(saturate(dot(c, c) * 0.5), 1.5);              // darker towards the corners
    col *= 1.0 + flicker;

    // A faint diagonal sheen across the glass.
    float diag = dot(q, normalize(float2(1.0, -0.7)));
    col += float3(0.85, 0.9, 1.0) * glare * (exp(-pow((diag + 0.35) / 0.32, 2.0)) * 0.025 + 0.012 * (1.0 - uv.y));

    col = 1.0 - exp(-col * 1.3);
    return float4(col, 1);
}
