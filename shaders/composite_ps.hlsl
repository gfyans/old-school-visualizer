#include "common.hlsli"

// Final pass: scales the visualiser image(s) to the window, crossfades during a
// switch, and dithers to hide 8-bit banding in dark gradients.

Texture2D LayerA : register(t0);
Texture2D LayerB : register(t1);
SamplerState Linear : register(s0);

cbuffer Composite : register(b2) {
    float2 uvScaleA;
    float2 uvMaxA;
    float2 uvScaleB;
    float2 uvMaxB;
    float mixAmount;
    float hasB;
    float2 compositePad;
};

float4 main(VSOut i) : SV_Target {
    float3 c = LayerA.SampleLevel(Linear, min(i.uv * uvScaleA, uvMaxA), 0).rgb;
    [branch] if (hasB > 0.5) {
        float3 b = LayerB.SampleLevel(Linear, min(i.uv * uvScaleB, uvMaxB), 0).rgb;
        c = lerp(c, b, smoothstep(0.0, 1.0, mixAmount));
    }
    c += (InterleavedGradientNoise(i.pos.xy) - 0.5) / 255.0;
    return float4(c, 1);
}
