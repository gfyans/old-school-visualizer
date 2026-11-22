#include "filter_common.hlsli"

// ASCII: glyphs from a brightness ramp (hi-hats flicker cells to 0s and 1s), inked partly in the
// palette and partly in the scene's own colour, over a dim copy of the scene, so you can still tell
// which visualiser is underneath. The shading is shared with Signal (cyber.hlsli).

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float2 size = CyberCellSize(kStyleAscii, iResolution.y);
    float2 centre, local;
    CyberCell(px, size, centre, local);
    float tear = FilterTear(centre.y);
    float3 src = SceneCell(centre + float2(tear, 0.0), size);
    float3 backdrop = ScenePx(px + float2(tear, 0.0));
    return FilterOut(CyberAscii(src, backdrop, local, floor(px / size), glitch, filterTime, FilterPalette()), i.pos.xy);
}
