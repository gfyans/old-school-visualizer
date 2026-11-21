#include "common.hlsli"

// Nebula temporal accumulation (TAA) resolve. Reprojects last frame's result to
// where it now sits on screen (using the gas distance and how the camera moved),
// clamps it to the current frame's neighbourhood so moving/changing gas never
// ghosts, and blends in a little of the new, jittered frame. Over a few frames the
// raymarch grain averages away and the jitter adds sub-pixel detail.

cbuffer Nebula : register(b2) {   // only the camera fields are used here (matches nebula_ps.hlsl)
    float3 camPos;      float tanHalfFov;
    float3 camRight;    float exposure;
    float3 camUp;       float starBoost;
    float3 camFwd;      float sparkle;
};

cbuffer Taa : register(b3) {      // matches TaaConstants in NebulaVisualizer.cpp
    float3 prevRight;   float prevTanHalfFov;
    float3 prevUp;      float blend;          // weight of the new frame (e.g. 0.15)
    float3 prevFwd;     float historyValid;   // 0 on the first frame / after a resize
    float3 camDelta;    float taaPad;         // camera position now minus last frame
};

Texture2D Current : register(t0);   // rgb + gas distance in alpha
Texture2D History : register(t1);
SamplerState Linear : register(s0);

// Bicubic (Catmull-Rom) history fetch in 9 bilinear taps. Plain bilinear would soften the
// image a little more every frame; Catmull-Rom keeps the accumulated detail crisp.
float3 SampleHistory(float2 uv) {
    float2 size;
    History.GetDimensions(size.x, size.y);
    float2 pos = uv * size;
    float2 p1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - p1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 p0 = (p1 - 1.0) / size, p3 = (p1 + 2.0) / size, p12 = (p1 + w2 / w12) / size;
    float3 r = 0;
    r += History.SampleLevel(Linear, float2(p0.x, p0.y), 0).rgb * (w0.x * w0.y);
    r += History.SampleLevel(Linear, float2(p12.x, p0.y), 0).rgb * (w12.x * w0.y);
    r += History.SampleLevel(Linear, float2(p3.x, p0.y), 0).rgb * (w3.x * w0.y);
    r += History.SampleLevel(Linear, float2(p0.x, p12.y), 0).rgb * (w0.x * w12.y);
    r += History.SampleLevel(Linear, float2(p12.x, p12.y), 0).rgb * (w12.x * w12.y);
    r += History.SampleLevel(Linear, float2(p3.x, p12.y), 0).rgb * (w3.x * w12.y);
    r += History.SampleLevel(Linear, float2(p0.x, p3.y), 0).rgb * (w0.x * w3.y);
    r += History.SampleLevel(Linear, float2(p12.x, p3.y), 0).rgb * (w12.x * w3.y);
    r += History.SampleLevel(Linear, float2(p3.x, p3.y), 0).rgb * (w3.x * w3.y);
    return max(r, 0.0);
}

struct Output {
    float4 display : SV_Target0;   // into the resolved image (then the post pass)
    float4 history : SV_Target1;   // kept for next frame
};

Output main(VSOut i) {
    int2 p = int2(i.pos.xy);
    float4 cur = Current.Load(int3(p, 0));

    // Neighbourhood colour range (3x3) for clamping. (Reprojection uses this pixel's own gas
    // distance: the usual "nearest depth" trick suits solid edges, but on volumetric gas it
    // makes stars next to a wisp reproject as if near, and smear.)
    // Variance clipping (mean +- 1 sigma) rather than min/max: each new frame is deliberately
    // noisy, so its min/max box is too loose to stop a smeared history slipping through.
    float3 m1 = 0, m2 = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float3 n = Current.Load(int3(p + int2(x, y), 0)).rgb;
        m1 += n;
        m2 += n * n;
    }
    m1 /= 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));
    float3 lo = m1 - sigma, hi = m1 + sigma;
    float depth = cur.a;

    // Where was this point last frame?
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((i.uv.x * 2.0 - 1.0) * aspect, 1.0 - i.uv.y * 2.0);
    float3 rd = normalize(camFwd + (sp.x * camRight + sp.y * camUp) * tanHalfFov);
    float3 rel = rd * depth + camDelta;                 // relative to last frame's camera
    float z = dot(rel, prevFwd);
    float2 psp = float2(dot(rel, prevRight), dot(rel, prevUp)) / (max(z, 1e-3) * prevTanHalfFov);
    float2 puv = float2((psp.x / aspect + 1.0) * 0.5, (1.0 - psp.y) * 0.5);
    bool valid = historyValid > 0.5 && z > 0.0 && all(puv > 0.0) && all(puv < 1.0);

    float3 hist = clamp(SampleHistory(puv), lo, hi);
    float3 result = valid ? lerp(hist, cur.rgb, blend) : cur.rgb;

    Output o;
    o.history = float4(result, 1.0);
    o.display = float4(result, 1.0);   // bloom and vignette follow in nebula_post_ps
    return o;
}
