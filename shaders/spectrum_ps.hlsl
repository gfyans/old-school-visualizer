#include "common.hlsli"

// 64 spectrum bars standing on a horizon line, with a soft reflection below
// and a gentle glow that swells on beats.

float3 BarColour(float t) {
    return lerp(float3(0.10, 0.45, 1.0), float3(1.0, 0.25, 0.65), saturate(t));
}

float4 main(VSOut i) : SV_Target {
    const float horizon = 0.72;   // y (from top) where bars stand
    const float maxH = 0.6;
    float2 uv = i.uv;

    // Background: dark gradient that brightens slightly with loudness / beats.
    float3 col = lerp(float3(0.02, 0.02, 0.05), float3(0.06, 0.03, 0.10), uv.y);
    col += float3(0.06, 0.04, 0.10) * aBeat;

    float x = uv.x * 64.0;
    int band = min((int)x, 63);
    float fx = frac(x);
    float h = Spectrum(band) * maxH;
    float inBar = smoothstep(0.08, 0.14, fx) * smoothstep(0.92, 0.86, fx);

    if (uv.y < horizon) {
        float y = horizon - uv.y;   // height above horizon
        if (y < h) {
            float t = y / maxH;
            col = lerp(col, BarColour(t) * (0.8 + 0.4 * aBeat), inBar);
        }
        // Soft glow around each bar.
        float glow = exp(-max(y - h, 0.0) * 18.0) * inBar * 0.25 * Spectrum(band);
        col += BarColour(h / maxH) * glow;
    } else {
        float y = uv.y - horizon;   // depth below horizon: reflection
        if (y < h * 0.5) {
            float fade = (1.0 - y / (h * 0.5 + 1e-4)) * 0.3;
            col = lerp(col, BarColour(y / maxH), inBar * fade);
        }
    }
    // Horizon line.
    col += float3(0.3, 0.3, 0.5) * exp(-abs(uv.y - horizon) * iResolution.y * 0.5) * 0.5;
    return float4(col, 1);
}
