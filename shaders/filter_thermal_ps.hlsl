#include "filter_common.hlsli"

// Thermal: brightness through a heat-camera ramp (black, violet, magenta, orange, yellow, white),
// slightly blurred like a low-resolution sensor. Drops run the whole picture hotter.

static const float3 kRamp[6] = { float3(0.0, 0.0, 0.02), float3(0.15, 0.0, 0.45), float3(0.6, 0.0, 0.6),
                                 float3(0.95, 0.25, 0.15), float3(1.0, 0.7, 0.05), float3(1.0, 1.0, 0.85) };

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float s = FilterScale();
    float2 uv = (px + float2(FilterTear(px.y), 0.0)) / iResolution;
    float2 o = 2.0 * s / iResolution;
    float3 c = Scene(uv) * 0.4 + (Scene(uv + float2(o.x, 0)) + Scene(uv - float2(o.x, 0)) +
                                  Scene(uv + float2(0, o.y)) + Scene(uv - float2(0, o.y))) * 0.15;
    float heat = saturate(0.06 + pow(max(Luma(c), 0.0), 0.7) * 1.05 + aStructure.y * 0.15);
    heat += (Hash33(int3(floor(px / max(1.0, 2.0 * s)), (int)(filterTime * 30.0))).x - 0.5) * 0.02;
    float f = saturate(heat) * 5.0;
    int k = min((int)f, 4);
    return FilterOut(lerp(kRamp[k], kRamp[k + 1], f - k), i.pos.xy);
}
