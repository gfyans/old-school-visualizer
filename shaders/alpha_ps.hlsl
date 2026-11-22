#include "common.hlsli"

// Transparent windows (stream modes for OBS): the finished frame's brightness becomes its opacity.
// Alpha follows the brightest channel, so black is fully see-through, bright light is solid and soft
// glow stays soft, and the colour stays premultiplied (no channel exceeds alpha). Over a game or the
// desktop that reads like the light added on top: no fringe, whatever the visualiser.
// The darkest few percent (dither grain, near-black haze) are cut to fully clear, so they don't lay
// a faint veil over everything behind; above that, alpha rises smoothly from 0.

Texture2D<float4> Frame : register(t0);   // a copy of the back buffer

static const float kBlackCut = 0.03;

float4 main(VSOut i) : SV_Target {
    const float3 c = Frame.Load(int3(i.pos.xy, 0)).rgb;
    const float m = max(c.r, max(c.g, c.b));
    const float a = saturate((m - kBlackCut) / (1.0 - kBlackCut));
    return float4(m > 0.0 ? c * (a / m) : 0.0, a);   // same hue, brightest channel == alpha
}
