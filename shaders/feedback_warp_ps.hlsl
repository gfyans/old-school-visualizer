#include "common.hlsli"

// Milkdrop-style feedback, pass 1: read the previous frame through a warp
// (zoom, rotation, sine ripples, optional kaleidoscope fold), fade it and shift
// its hue, then draw fresh "ink" on top: a waveform ring or line, beat rings and
// treble sparks. Repeating this every frame turns the ink into trails, tunnels
// and liquid motion. Values are linear and may exceed 1 (16-bit float target).

cbuffer Feedback : register(b2) {   // matches FeedbackConstants in FeedbackVisualizer.cpp
    float zoom;        float rotation;   float warpAmp;    float warpFreq;
    float decay;       float hueShift;   float mirror;     float inkShape;
    float3 inkA;       float inkGlow;
    float3 inkB;       float ringRadius;
    float waveScale;   float warpPhase;  float sparks;     float flash;
};

Texture2D Prev : register(t0);
SamplerState Border : register(s0);   // black outside the image

static const float kTau = 6.28318531;

float3 HueRotate(float3 c, float angle) {   // rotate around the grey axis
    const float3 k = 0.57735027;
    float ca = cos(angle), sa = sin(angle);
    return c * ca + cross(k, c) * sa + k * dot(k, c) * (1.0 - ca);
}

float2 Fold(float2 p, float segments) {      // kaleidoscope: mirror into one wedge
    float a = atan2(p.y, p.x);
    float r = length(p);
    float seg = kTau / segments;
    a = abs(fmod(a + 64.0 * seg, seg) - seg * 0.5);
    return r * float2(cos(a), sin(a));
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 p = (i.uv - 0.5) * float2(aspect, 1.0);   // centred, aspect-correct
    if (mirror > 1.5) p = Fold(p, mirror);

    // --- Warp the previous frame ---
    float c = cos(rotation), s = sin(rotation);
    float2 q = float2(c * p.x - s * p.y, s * p.x + c * p.y) * zoom;
    q += warpAmp * float2(sin(q.y * warpFreq + warpPhase), sin(q.x * warpFreq * 1.3 - warpPhase * 1.1));
    float2 uv = q / float2(aspect, 1.0) + 0.5;
    float3 col = Prev.SampleLevel(Border, uv, 0).rgb;
    col = HueRotate(col, hueShift) * decay;

    // --- Fresh ink ---
    float px = 1.0 / iResolution.y;               // one pixel in these units
    float thick = (1.6 + 2.5 * inkGlow) * px;
    float3 ink = 0;
    if (inkShape < 0.5) {
        // Waveform ring: radius modulated by the waveform; mirrored so the seam never shows.
        float t = abs(atan2(p.y, p.x) / kTau * 2.0);   // 0..1..0 around the circle
        float r = ringRadius + waveScale * WaveformAt(t);
        float d = abs(length(p) - r);
        ink = lerp(inkA, inkB, t) * exp(-d * d / (thick * thick));
    } else {
        // Waveform line across the screen, mirrored top/bottom for symmetry.
        float x = i.uv.x;
        float y = waveScale * WaveformAt(x);
        float d = min(abs(p.y - y - 0.12), abs(p.y + y + 0.12));
        ink = lerp(inkA, inkB, x) * exp(-d * d / (thick * thick));
    }
    ink *= 0.7 + 1.8 * inkGlow;

    // Treble sparks: random cells light up for a moment; the feedback turns them into streaks.
    int3 cell = int3(floor(i.pos.xy / 3.0), (int)floor(iTime * 30.0));
    float3 h = Hash33(cell + 9137);
    ink += lerp(inkB, 1.0.xxx, 0.5) * step(1.0 - sparks * 0.0008, h.x) * 2.0;

    // Drop flash: a soft burst from the centre.
    ink += lerp(inkA, inkB, 0.5) * flash * exp(-dot(p, p) * 12.0) * 0.6;

    return float4(col + ink, 1.0);
}
