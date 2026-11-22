#include "filter_common.hlsli"

// Scan lines: thin bright lines with dark gaps, RGB fringes (wider when it glitches) and streaks
// smeared out of bright areas to the left.

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float2 size = CyberCellSize(kStyleScan, iResolution.y);
    float2 centre, local;
    CyberCell(px, size, centre, local);
    float2 at = centre + float2(FilterTear(centre.y), 0.0);

    float split = 3.0 + 12.0 * glitch;
    float reach = 8.0 + 30.0 * Hash33(int3((int)(centre.y / 4.0), 5, 9)).x;
    float3 streak = 0;
    [unroll] for (int k = 1; k <= 4; ++k) streak = max(streak, ScenePx(at - float2(reach * k, 0)) * (1.0 - k * 0.2));
    float3 c = CyberScan(ScenePx(at + float2(split, 0)), ScenePx(at), ScenePx(at - float2(split, 0)), streak,
                         local, floor(px / size), filterTime, glitch);
    return FilterOut(c, i.pos.xy);
}
