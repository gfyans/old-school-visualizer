#include "filter_common.hlsli"

// VHS: soft luma, colour smeared to the right, washed out, with a slow wobble, a tracking band
// rolling up the screen, head-switching noise along the bottom and the odd dropout. On drops the
// tracking slips and whole bands of the picture jump sideways.

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float2 uv = i.uv;
    float s = FilterScale();
    float t = filterTime;
    float drop = aStructure.y;
    float3 n = Hash33(int3((int)(px.y / (2.0 * s)), (int)(t * 30.0), 3));

    float wobble = (sin(px.y * 0.021 / s + t * 2.3) * 0.6 + sin(px.y * 0.13 / s + t * 7.0) * 0.25) * s;
    float bandY = frac(-t * 0.035) * 1.3 - 0.15;
    float bd = (uv.y - bandY) / 0.03;
    float band = exp(-bd * bd);
    float bandShift = band * (n.x - 0.5) * (8.0 + 40.0 * drop) * s;
    float head = smoothstep(0.978, 1.0, uv.y);
    float headShift = head * (14.0 + 30.0 * n.y) * s;
    float3 slip = Hash33(int3((int)(px.y / (28.0 * s)), (int)(t * 20.0), 41));
    float dropShift = drop * step(0.55, slip.x) * (slip.y - 0.5) * 160.0 * s;
    float shift = wobble + bandShift + headShift + dropShift + FilterTear(px.y);
    float2 base = float2(px.x + shift, px.y);

    // Luma from three close taps; chroma from six taps reaching to the right.
    float y = 0.0;
    [unroll] for (int a = -1; a <= 1; ++a) y += Luma(ScenePx(base + float2(a * 2.0 * s, 0.0))) / 3.0;
    float2 iq = 0.0;
    [unroll] for (int b = 0; b < 6; ++b) {
        float3 sc = ScenePx(base + float2((b * 3.2 - 3.0) * s, 0.0));
        iq += float2(dot(sc, float3(0.596, -0.274, -0.322)), dot(sc, float3(0.211, -0.523, 0.312))) / 6.0;
    }
    iq = iq * 0.75 + float2(0.012, 0.0);   // washed out, drifting slightly warm
    float3 c = float3(dot(float3(y, iq), float3(1.0, 0.956, 0.621)), dot(float3(y, iq), float3(1.0, -0.272, -0.647)),
                      dot(float3(y, iq), float3(1.0, -1.106, 1.703)));
    c = max(c, 0.0) * 0.92 + float3(0.03, 0.028, 0.04);   // lifted, slightly blue blacks
    c = c / (1.0 + 0.2 * c) * 1.1;                        // soft highlights

    float3 g = Hash33(int3(px, (int)(t * 60.0)));
    c += (g.x - 0.5) * 0.05;
    c += band * step(0.6, g.y) * g.z * 0.35;               // snow in the tracking band
    c += head * g.y * 0.4;
    // Dropouts: short white streaks, more of them when it glitches.
    float3 d = Hash33(int3((int)px.y, (int)(t * 24.0), 11));
    float start = d.y * iResolution.x, len = (20.0 + 60.0 * d.z) * s;
    c += step(0.9988 - 0.004 * glitch, d.x) * step(start, px.x) * step(px.x, start + len) * 0.6;
    c *= 1.0 - 0.07 * (fmod(floor(px.y / max(1.0, round(s))), 2.0));   // faint field lines
    return FilterOut(c, i.pos.xy);
}
