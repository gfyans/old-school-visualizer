#include "dots.hlsli"

// Dot Field output: a dark backdrop, the dots, a glow from the dot image's mips, and a tone curve.
Texture2D<float4> DotImage : register(t0);
SamplerState Linear : register(s0);

float4 main(VSOut i) : SV_Target {
    float2 v = i.uv - 0.5;
    float3 col = bgCol * (1.0 - 1.3 * dot(v, v)) * (1.0 + 0.6 * buildUp);
    // RGB split: always a hair at the edges, wide while glitching.
    float2 split = float2((0.0015 * dot(v, v) * 4.0 + 0.006 * glitch), 0.0);
    float3 dots = float3(DotImage.SampleLevel(Linear, i.uv + split, 0).r, DotImage.SampleLevel(Linear, i.uv, 0).g,
                         DotImage.SampleLevel(Linear, i.uv - split, 0).b);
    col += max(dots, 0.0);
    float3 bloom = DotImage.SampleLevel(Linear, i.uv, 2).rgb * 0.25;
    [unroll] for (int m = 3; m <= 7; ++m) bloom += DotImage.SampleLevel(Linear, i.uv, m).rgb * 0.14;
    col += max(bloom, 0.0) * (0.5 + 0.6 * flash + 0.4 * buildUp);
    col += peakCol * flash * 0.15;
    col *= 1.0 - dot(v, v) * 0.6;
    col *= 0.93 + 0.07 * sin(i.pos.y * 3.14159 * 0.5);   // faint CRT scan lines
    col = 1.0 - exp(-col * 1.1);

    col += (InterleavedGradientNoise(i.pos.xy) - 0.5) / 255.0;
    return float4(col, 1);
}
