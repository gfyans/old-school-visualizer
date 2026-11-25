#include "common.hlsli"

// Glass City temporal upscaling. The city is traced at a fraction of the output resolution, each frame
// from a different sub-pixel position (Halton jitter). This pass runs at full output resolution: it finds
// where each pixel's point was last frame (from the camera's move and the distance to the first glass),
// fetches the accumulated history there, clamps it to the colours the new frame has around this spot (so
// moving lights, tiles changing and newly revealed towers never ghost), and blends in this frame's
// nearest sample, more strongly the closer that sample landed to the pixel's centre. Over a few frames
// the jittered samples fill in full-resolution detail: the thin edges and tile grids stay sharp.

cbuffer GlassCity : register(b2) {   // only the camera is used here (matches glasscity_ps.hlsl)
    float3 camPos;   float focal;
    float3 camRight; float time;
    float3 camUp;    float speed;
    float3 camFwd;   float ceiling;
};

cbuffer GlassTaa : register(b3) {    // matches GlassTaaConstants in GlassCityVisualizer.cpp
    float3 prevRight; float prevFocal;
    float3 prevUp;    float blend;          // weight of a new sample landing right on the pixel
    float3 prevFwd;   float historyValid;   // 0 on the first frame, after a resize or a jump
    float3 camDelta;  float taaPad0;        // camera position now minus last frame
    float2 jitter;    float2 taaPad1;       // this frame's sub-pixel offset, in trace pixels
};

Texture2D Current : register(t0);   // the trace: HDR rgb, distance to the first glass in alpha
Texture2D History : register(t1);   // accumulated, at output resolution
SamplerState Linear : register(s0);

// Bicubic (Catmull-Rom) history fetch in 9 bilinear taps: plain bilinear would soften the image a little
// more every frame; Catmull-Rom keeps the accumulated detail crisp.
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

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

struct Output {
    float4 display : SV_Target0;   // into the resolved image (then bloom, speed blur, tone mapping)
    float4 history : SV_Target1;   // kept for next frame
};

Output main(VSOut i) {
    float2 traceSize;
    Current.GetDimensions(traceSize.x, traceSize.y);
    const int2 last = int2(traceSize) - 1;

    // This frame's sample nearest the pixel: trace pixel k was shot through (k + 0.5 + jitter) / size.
    float2 tp = i.uv * traceSize - 0.5 - jitter;
    int2 k = clamp(int2(round(tp)), 0, last);
    float2 off = tp - float2(k);   // how far from this pixel's centre it landed (trace pixels)
    float4 cur = Current.Load(int3(k, 0));

    // The colours around here this frame (3x3 box). The trace isn't noisy, so the plain min/max box
    // keeps thin lines that a tighter (variance) clip would cut.
    float3 lo = cur.rgb, hi = cur.rgb;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float3 n = Current.Load(int3(clamp(k + int2(x, y), 0, last), 0)).rgb;
        lo = min(lo, n);
        hi = max(hi, n);
    }

    // Where was this point last frame?
    float aspect = iResolution.x / iResolution.y;
    float2 ndc = (i.uv * 2.0 - 1.0) * float2(aspect, -1.0);
    float3 rd = normalize(camFwd * focal + camRight * ndc.x + camUp * ndc.y);
    float3 rel = rd * cur.a + camDelta;   // relative to last frame's camera
    float z = dot(rel, prevFwd);
    float2 pndc = float2(dot(rel, prevRight), dot(rel, prevUp)) * prevFocal / max(z, 1e-3);
    float2 puv = float2((pndc.x / aspect + 1.0) * 0.5, (1.0 - pndc.y) * 0.5);
    bool valid = historyValid > 0.5 && z > 0.0 && all(puv > 0.0) && all(puv < 1.0);

    float3 hist = clamp(SampleHistory(puv), lo, hi);
    // New sample's weight: full blend when it landed on the pixel, fading fast as it lands further off
    // (measured in output pixels: a sample from the neighbouring pixel would only blur this one).
    float2 offOut = off * (iResolution.xy / traceSize);
    float w = valid ? blend * exp(-dot(offOut, offOut) / (2.0 * 0.35 * 0.35)) : 1.0;
    // Blend weighted by 1 / (1 + brightness), so a few very bright samples don't flicker.
    float wc = w / (1.0 + Luma(cur.rgb)), wh = (1.0 - w) / (1.0 + Luma(hist));
    float3 result = (cur.rgb * wc + hist * wh) / max(wc + wh, 1e-6);
    if (any(isnan(result)) || any(isinf(result))) result = cur.rgb;

    Output o;
    o.history = float4(result, 1.0);
    o.display = float4(result, 1.0);
    return o;
}
