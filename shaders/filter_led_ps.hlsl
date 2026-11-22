#include "filter_common.hlsli"

// LED: a grid of rounded lamps in the scene's colours, saturated, in six brightness steps.

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float2 size = CyberCellSize(kStyleLed, iResolution.y);
    float2 centre, local;
    CyberCell(px, size, centre, local);
    float3 src = SceneCell(centre + float2(FilterTear(centre.y), 0.0), size);
    return FilterOut(CyberLed(src, local), i.pos.xy);
}
