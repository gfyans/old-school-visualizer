#include "common.hlsli"

// Equaliser bars along the bottom: rounded glass bars with a palette gradient,
// glowing rims, falling peak caps and a faint reflection. Output is
// premultiplied alpha (alpha = how much of the scene is covered; rgb may add glow).

cbuffer Eq : register(b2) {   // matches EqConstants in EqBars.cpp
    float4 eqValues[12];      // 48 bars, 0..1
    float4 eqPeaks[12];
    float3 eqLo;  float eqOpacity;
    float3 eqHi;  float eqBeat;
};

static const int kBars = 48;

float BarValue(int i) { return Pick(eqValues[i >> 2], i & 3); }
float PeakValue(int i) { return Pick(eqPeaks[i >> 2], i & 3); }

// Signed distance to a box with rounded corners (radius r), centred at the origin.
float SdRoundBox(float2 p, float2 halfSize, float r) {
    float2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 main(VSOut i) : SV_Target {
    float2 px = i.uv * iResolution;
    float scale = iResolution.y / 1080.0;            // layout authored at 1080p
    float marginX = iResolution.x * 0.05;
    float areaW = iResolution.x - 2.0 * marginX;
    float baseY = iResolution.y - 64.0 * scale;      // where bars stand
    float maxH = iResolution.y * 0.16;

    // Cheap early-out for everything well above the bars.
    if (px.y < baseY - maxH - 40.0 * scale) return 0;

    float slot = areaW / kBars;
    float barW = slot * 0.62;
    int b = clamp((int)floor((px.x - marginX) / slot), 0, kBars - 1);
    float cx = marginX + (b + 0.5) * slot;
    float v = BarValue(b);
    float h = max(v * maxH, 3.0 * scale);
    float radius = barW * 0.5;
    float aa = 1.0;   // ~1 px anti-aliasing

    float3 rgb = 0;
    float alpha = 0;

    // --- Bar body: translucent glass with a vertical gradient ---
    float sd = SdRoundBox(px - float2(cx, baseY - h * 0.5), float2(barW * 0.5, h * 0.5), min(radius, h * 0.5));
    float inside = smoothstep(aa, -aa, sd);
    float t = saturate((baseY - px.y) / maxH);             // 0 at base, 1 at full height
    float3 col = lerp(eqLo, eqHi, t);
    float bodyA = (0.38 + 0.4 * t) * inside;
    // Inner sheen: a soft highlight down the left third, like light on glass.
    float sheen = smoothstep(0.0, 0.5, 1.0 - abs((px.x - (cx - barW * 0.18)) / (barW * 0.25)));
    float3 body = col * (0.85 + 0.35 * sheen) * (1.0 + 0.5 * eqBeat);
    rgb += body * bodyA;
    alpha += bodyA;

    // --- Rim: thin bright edge ---
    float rim = smoothstep(1.5 * scale, 0.0, abs(sd)) * (0.35 + 0.65 * t);
    rgb += lerp(col, 1.0.xxx, 0.5) * rim * 0.8;

    // --- Glow around the bar (additive) ---
    float glow = exp(-max(sd, 0.0) / (7.0 * scale)) * (1.0 - inside) * (0.15 + 0.35 * v) * (1.0 + eqBeat);
    rgb += col * glow * 0.45;

    // --- Peak cap: small floating pill that falls after the bar drops ---
    float peakY = baseY - max(PeakValue(b) * maxH, h) - 7.0 * scale;
    float sdCap = SdRoundBox(px - float2(cx, peakY), float2(barW * 0.5, 2.0 * scale), 2.0 * scale);
    float cap = smoothstep(aa, -aa, sdCap);
    float3 capCol = lerp(eqHi, 1.0.xxx, 0.55);
    rgb = lerp(rgb, capCol, cap * 0.95);
    alpha = max(alpha, cap * 0.95);
    rgb += capCol * exp(-max(sdCap, 0.0) / (4.0 * scale)) * (1.0 - cap) * 0.25;

    // --- Reflection under the baseline ---
    if (px.y > baseY) {
        float depth = px.y - baseY;
        float2 mirrored = float2(px.x, baseY - depth);
        float sdR = SdRoundBox(mirrored - float2(cx, baseY - h * 0.5), float2(barW * 0.5, h * 0.5), min(radius, h * 0.5));
        float fade = saturate(1.0 - depth / (40.0 * scale)) * 0.22;
        float r = smoothstep(aa, -aa, sdR) * fade;
        rgb += col * r;
        alpha += r * 0.5;
    }

    // --- Baseline: a fine glowing line across the bar area ---
    float lineFade = smoothstep(0.0, 0.08, i.uv.x) * smoothstep(1.0, 0.92, i.uv.x);
    float baseline = exp(-abs(px.y - baseY - 1.5 * scale) / (1.2 * scale)) * lineFade;
    rgb += lerp(eqLo, eqHi, 0.5) * baseline * (0.35 + 0.25 * eqBeat);

    return float4(rgb, saturate(alpha)) * eqOpacity;
}
