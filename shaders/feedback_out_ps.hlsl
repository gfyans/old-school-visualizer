#include "common.hlsli"

// Milkdrop-style feedback, pass 2: present the accumulated (linear, unbounded)
// feedback buffer with a soft tonemap and a gentle vignette.

Texture2D Feedback : register(t0);
SamplerState Linear : register(s0);

float4 main(VSOut i) : SV_Target {
    float3 c = Feedback.SampleLevel(Linear, i.uv, 0).rgb;
    c = 1.0 - exp(-c * 1.3);                  // soft clip: trails can stack without blowing out
    float2 v = i.uv - 0.5;
    c *= 1.0 - dot(v, v) * 0.5;
    return float4(c, 1.0);
}
