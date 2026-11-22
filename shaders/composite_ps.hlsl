#include "common.hlsli"

// Final pass: scales the visualiser image(s) to the window, plays the transition
// when switching visualisers, and dithers to hide 8-bit banding in dark gradients. With a filter on,
// it draws into the filter's scene texture instead (src/filter/FilterHost.h), with no grade.
// Transition styles: 0 crossfade, 1 glitch cut, 2 zoom-through, 3 iris, 4 pixel dissolve.

Texture2D LayerA : register(t0);   // outgoing (or the only layer)
Texture2D LayerB : register(t1);   // incoming
SamplerState Linear : register(s0);

cbuffer Composite : register(b2) {
    float2 uvScaleA;
    float2 uvMaxA;
    float2 uvScaleB;
    float2 uvMaxB;
    float mixAmount;   // 0 -> 1 through the transition
    float hasB;
    float style;
    float seed;        // changes per transition, for randomised effects
    float warmth;      // mood colour: -1 cool .. +1 warm
    float hueShift;    // radians: nudged by chord changes
    float gradeAmount; // 0 = off
    float gradePad;
};

// Mood colour grade: a gentle warm/cool white balance and a small hue rotation (in YIQ space,
// which keeps brightness while turning the colours).
float3 Grade(float3 c) {
    if (gradeAmount <= 0.0) return c;
    float3 yiq = float3(dot(c, float3(0.299, 0.587, 0.114)), dot(c, float3(0.596, -0.274, -0.322)), dot(c, float3(0.211, -0.523, 0.312)));
    float a = hueShift * gradeAmount, ca = cos(a), sa = sin(a);
    yiq.yz = float2(yiq.y * ca - yiq.z * sa, yiq.y * sa + yiq.z * ca);
    float3 rgb = float3(dot(yiq, float3(1.0, 0.956, 0.621)), dot(yiq, float3(1.0, -0.272, -0.647)), dot(yiq, float3(1.0, -1.106, 1.703)));
    rgb *= 1.0 + warmth * gradeAmount * float3(0.08, 0.0, -0.09);
    return max(rgb, 0.0);
}

float3 SampleA(float2 uv) { return LayerA.SampleLevel(Linear, min(saturate(uv) * uvScaleA, uvMaxA), 0).rgb; }
float3 SampleB(float2 uv) { return LayerB.SampleLevel(Linear, min(saturate(uv) * uvScaleB, uvMaxB), 0).rgb; }

float3 Transition(float2 uv, float2 pixel) {
    float t = saturate(mixAmount);
    int s = (int)style;
    float aspect = iResolution.x / iResolution.y;
    float3 result = 0;

    [branch] if (s == 1) {
        // Glitch cut: horizontal slices flip to the new scene at random moments, with an
        // RGB split and slice jitter that peak half-way through.
        float slice = floor(uv.y * 28.0);
        float3 h = Hash33(int3((int)slice, (int)(seed * 9973.0), 5));
        float chaos = sin(t * 3.14159);
        float2 jitter = float2((h.y - 0.5) * 0.08 * chaos, 0.0);
        bool useB = h.x < t * 1.25 - 0.1;
        float split = 0.012 * chaos;
        float3 c = 0;
        if (useB) c = float3(SampleB(uv + jitter + float2(split, 0)).r, SampleB(uv + jitter).g, SampleB(uv + jitter - float2(split, 0)).b);
        else      c = float3(SampleA(uv + jitter + float2(split, 0)).r, SampleA(uv + jitter).g, SampleA(uv + jitter - float2(split, 0)).b);
        result = c * (1.0 + 0.5 * chaos * step(0.92, h.z));   // occasional bright slice
    } else if (s == 2) {
        // Zoom-through: fly into the old scene while the new one arrives from behind.
        float2 c = uv - 0.5;
        float3 a = SampleA(0.5 + c / (1.0 + 2.5 * t * t));
        float3 b = SampleB(0.5 + c * (1.6 - 0.6 * t));
        float k = smoothstep(0.35, 0.75, t);
        result = lerp(a, b, k) + 0.25 * exp(-pow((t - 0.55) * 6.0, 2.0));   // flash at the crossing
    } else if (s == 3) {
        // Iris: the new scene opens from the centre behind a glowing neon ring.
        float2 p = (uv - 0.5) * float2(aspect, 1.0);
        float radius = t * t * (0.6 * aspect + 0.6);
        float d = length(p) - radius;
        float3 c = d < 0.0 ? SampleB(uv) : SampleA(uv);
        float ring = exp(-abs(d) * 90.0) * (1.0 - t);
        result = c + float3(0.4, 0.9, 1.0) * ring * 1.5;
    } else if (s == 4) {
        // Pixel dissolve: blocks grow, the scene swaps at the coarsest point, then resolves.
        float blocks = lerp(1.0, 48.0, sin(t * 3.14159));
        float2 cell = floor(pixel / blocks) * blocks + blocks * 0.5;
        float2 q = cell / iResolution;
        float3 h = Hash33(int3(floor(pixel / blocks), (int)(seed * 977.0)));
        bool useB = h.x < smoothstep(0.3, 0.7, t);
        result = useB ? SampleB(q) : SampleA(q);
    } else {
        result = lerp(SampleA(uv), SampleB(uv), smoothstep(0.0, 1.0, t));   // crossfade
    }
    return result;
}

float3 Fetch(float2 uv, float2 pixel) { return hasB > 0.5 ? Transition(uv, pixel) : SampleA(uv); }

float4 main(VSOut i) : SV_Target {
    float3 c = Grade(Fetch(i.uv, i.pos.xy));
    c += (InterleavedGradientNoise(i.pos.xy) - 0.5) / 255.0;
    return float4(c, 1);
}
