#include "filter_common.hlsli"

// Dither: ordered (Bayer) dithering in five shades of the palette, the middle ones in the scene's own
// hue. The shading is shared with Signal (cyber.hlsli).

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float2 size = CyberCellSize(kStyleDither, iResolution.y);
    float2 centre, local;
    CyberCell(px, size, centre, local);
    float3 src = ScenePx(centre + float2(FilterTear(centre.y), 0.0));
    return FilterOut(CyberDither(src, int2(floor(px / size)), FilterPalette()), i.pos.xy);
}
