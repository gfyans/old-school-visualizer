#include "filter_common.hlsli"

// Halftone: three rotated screens of dots in cyan, magenta and yellow on a dark ground, each dot's
// area matching how much of that ink the colour needs. Dots swell a little with the bass.

static const float kAngle[3] = { 0.2618, 1.309, 0.0 };   // 15, 75 and 0 degrees
static const float3 kInk[3] = { float3(0.0, 1.0, 1.0), float3(1.0, 0.0, 1.0), float3(1.0, 1.0, 0.0) };

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float cell = 7.0 * FilterScale();
    float swell = 1.0 + 0.08 * aBands.x;
    float tear = FilterTear(px.y);
    float3 col = float3(0.02, 0.02, 0.03);
    [unroll] for (int k = 0; k < 3; ++k) {
        float ca = cos(kAngle[k]), sa = sin(kAngle[k]);
        float2 q = float2(ca * px.x + sa * px.y, -sa * px.x + ca * px.y);
        float2 qc = (floor(q / cell) + 0.5) * cell;
        float2 centre = float2(ca * qc.x - sa * qc.y, sa * qc.x + ca * qc.y);
        float3 src = ScenePx(centre + float2(tear, 0.0));
        // Split the colour into amounts of cyan (g+b), magenta (r+b) and yellow (r+g) light.
        // Faint haze is left out, so dark areas stay clean.
        float3 amt = saturate((float3(src.g + src.b - src.r, src.r + src.b - src.g, src.r + src.g - src.b) * 0.5 - 0.04) * 1.6);
        float a = k == 0 ? amt.x : k == 1 ? amt.y : amt.z;
        float r = cell * sqrt(a / 3.14159) * swell;
        col += kInk[k] * smoothstep(r + 0.7, r - 0.7, length(q - qc)) * saturate(r / 0.7);
    }
    return FilterOut(saturate(col), i.pos.xy);
}
