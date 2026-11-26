#include "common.hlsli"

// Nebula post pass, after the TAA resolve: bloom from the resolved image's mips (a soft
// glow around the core, lit rims and events), lens flares for the events (the supernova's
// glare, starburst and anamorphic streak; the pulsar's diffraction spikes; the build-up star's
// glow), each dimmed by the gas in front of it, the drop flash, and the vignette.

cbuffer Post : register(b3) {   // matches PostConstants in NebulaVisualizer.cpp
    float2 snScreen;     float snFlare;      float bloom;    // supernova position (uv) and flare strength
    float3 flareCol;     float flash;
    float2 pulsarScreen; float pulsarFlare;  float starFlare;   // pulsar position (uv) and flare; build-up star's glow
    float3 pulsarFlareCol; float postPad;
    float3 starFlareCol;   float postPad2;
};

Texture2D Image : register(t0);        // resolved display image, with mips
Texture2D Visibility : register(t1);   // 1x1: r = light reaching us from the pulsar, g = from the supernova
SamplerState Linear : register(s0);

// Mips are box-filtered: a bright spot turns into a visible square. Four bilinear taps
// half a texel apart smooth that into a soft tent.
float3 SoftMip(float2 uv, int m) {
    float2 size;
    Image.GetDimensions(size.x, size.y);
    float2 o = 0.5 * exp2(m) / size;
    return (Image.SampleLevel(Linear, uv + float2(o.x, o.y), m) + Image.SampleLevel(Linear, uv + float2(-o.x, o.y), m) +
            Image.SampleLevel(Linear, uv + float2(o.x, -o.y), m) + Image.SampleLevel(Linear, uv - o, m)).rgb * 0.25;
}

float4 main(VSOut i) : SV_Target {
    float3 col = Image.SampleLevel(Linear, i.uv, 0).rgb;

    // Bloom: only what's brighter than a threshold spreads (bright gas everywhere would
    // otherwise wash the whole image out).
    float3 b = 0;
    [unroll] for (int m = 1; m <= 6; ++m) {
        float3 s = m <= 2 ? Image.SampleLevel(Linear, i.uv, m).rgb : SoftMip(i.uv, m);
        float3 over = max(s - 0.55, 0.0);
        b += over * over * (m <= 2 ? 0.8 : 1.2);
    }
    col += b * bloom;

    float aspect = iResolution.x / iResolution.y;
    float2 vis = Visibility.Load(int3(0, 0, 0)).rg;

    // Supernova lens flare. Gas in front dims it, but never hides a supernova completely.
    if (snFlare > 0.001) {
        float2 d = (i.uv - snScreen) * float2(aspect, 1.0);
        float r = length(d);
        float glare = exp(-r / 0.035) * 0.9 + exp(-r / 0.18) * 0.2;
        float rays = pow(abs(cos(atan2(d.y, d.x) * 3.0)), 60.0) * exp(-r / 0.25) * 0.8;
        float streak = exp(-abs(d.y) / 0.004) * exp(-abs(d.x) / 0.7) * 0.7;
        col += flareCol * (glare + rays + streak) * snFlare * lerp(0.35, 1.0, vis.g);
    }

    // The build-up star (and later the remnant): a soft glow that tightens as it charges.
    if (starFlare > 0.001) {
        float r = length((i.uv - snScreen) * float2(aspect, 1.0));
        col += starFlareCol * (exp(-r / 0.02) * 0.5 + exp(-r / 0.09) * 0.12) * starFlare * vis.g;
    }

    // Pulsar: four diffraction spikes and a little glare; it flares as a beam sweeps past us.
    if (pulsarFlare > 0.001) {
        float2 d = (i.uv - pulsarScreen) * float2(aspect, 1.0);
        float r = length(d);
        float spikes = (exp(-abs(d.x) / 0.0025) * exp(-abs(d.y) / 0.09) + exp(-abs(d.y) / 0.0025) * exp(-abs(d.x) / 0.09)) * 0.7;
        float diag = pow(abs(sin(2.0 * atan2(d.y, d.x))), 80.0) * exp(-r / 0.05) * 0.25;
        float glare = exp(-r / 0.012) * 0.6 + exp(-r / 0.07) * 0.08;
        col += pulsarFlareCol * (spikes + diag + glare) * pulsarFlare * vis.r;
    }

    col += flash;
    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.7;   // vignette
    return float4(saturate(col), 1);
}
