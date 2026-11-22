#include "filter_common.hlsli"

// Polaroid: softened, faded colour. Highlights roll off to cream, blacks lift to a teal-green, warm
// vignette and film grain. Drops flare a warm light leak in from one edge. (No tearing: it's a photo.)

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float2 uv = i.uv;
    float s = FilterScale();
    float3 c = Scene(uv) * 0.5;
    float2 o = 1.5 * s / iResolution;
    c += (Scene(uv + float2(o.x, 0)) + Scene(uv - float2(o.x, 0)) + Scene(uv + float2(0, o.y)) + Scene(uv - float2(0, o.y))) * 0.125;

    c = (1.0 - exp(-c * 1.5)) * 1.2;
    c = lerp(Luma(c), c, 0.75);
    float3 shadow = float3(0.07, 0.10, 0.095), highlight = float3(1.0, 0.95, 0.82);
    c = shadow + saturate(c) * (highlight - shadow);
    float l = Luma(c);
    c += float3(0.03, 0.01, -0.03) * l * (1.0 - l) * 4.0;   // warm mids

    float aspect = iResolution.x / iResolution.y;
    float v = length((uv - 0.5) * float2(aspect * 0.75, 1.0));
    c *= lerp(1.0, float3(0.72, 0.64, 0.56), smoothstep(0.35, 0.95, v));

    float side = Hash33(int3((int)aStructure.w, 7, 13)).x;   // a new edge for each drop
    float edge = side < 0.5 ? uv.x : 1.0 - uv.x;
    c += aStructure.y * exp(-edge * 5.0) * float3(1.0, 0.45, 0.15) * 0.55;

    float grain = Hash33(int3(floor(px / max(1.0, 1.5 * s)), (int)(filterTime * 24.0))).x - 0.5;
    return FilterOut(max(c + grain * 0.045, 0.0), i.pos.xy);
}
