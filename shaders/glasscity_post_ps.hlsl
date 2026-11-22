#include "common.hlsli"

// Glass City post pass: speed blur towards the vanishing point (subtle when cruising, a tunnel of light at
// hyperspeed) with colour fringes, bloom from the scene's mips, tone mapping, the drop flash, vignette
// and a little grain.

cbuffer GlassPost : register(b3) {   // matches GlassPostConstants in GlassCityVisualizer.cpp
    float2 vanish;   float blur;   float bloom;       // vanishing point (uv); blur length (share of the way there)
    float flash;     float aberr;  float exposure;  float grain;
};

Texture2D Image : register(t0);   // HDR scene, with mips
SamplerState Linear : register(s0);

// Mips are box-filtered; four bilinear taps half a texel apart soften them into a tent.
float3 SoftMip(float2 uv, int m) {
    float2 size;
    Image.GetDimensions(size.x, size.y);
    float2 o = 0.5 * exp2(m) / size;
    return (Image.SampleLevel(Linear, uv + float2(o.x, o.y), m) + Image.SampleLevel(Linear, uv + float2(-o.x, o.y), m) +
            Image.SampleLevel(Linear, uv + float2(o.x, -o.y), m) + Image.SampleLevel(Linear, uv - o, m)).rgb * 0.25;
}

float3 Aces(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 main(VSOut i) : SV_Target {
    float2 d = i.uv - vanish;
    float jitter = InterleavedGradientNoise(i.pos.xy);
    float2 ca = d * aberr;
    float3 col = 0;
    const int kTaps = 10;
    [unroll] for (int k = 0; k < kTaps; ++k) {
        float2 uv = vanish + d * (1.0 - blur * (k + jitter) / kTaps);
        col.r += Image.SampleLevel(Linear, uv + ca, 0).r;
        col.g += Image.SampleLevel(Linear, uv, 0).g;
        col.b += Image.SampleLevel(Linear, uv - ca, 0).b;
    }
    col /= kTaps;

    float3 b = 0;
    [unroll] for (int m = 1; m <= 6; ++m) {
        float3 s = m <= 2 ? Image.SampleLevel(Linear, i.uv, m).rgb : SoftMip(i.uv, m);
        b += max(s - 0.5, 0.0) * (m <= 2 ? 0.2 : 0.3);
    }
    col += b * bloom;

    col = Aces(col * exposure);
    col += flash;
    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.6;
    col += (InterleavedGradientNoise(i.pos.xy + frac(iTime * 7.13) * 97.0) - 0.5) * grain;
    return float4(saturate(col), 1.0);
}
