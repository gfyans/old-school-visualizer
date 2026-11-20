#include "common.hlsli"

// Cyberpunk equaliser along the bottom: slanted columns of neon LED segments
// (cyan -> magenta -> warning yellow) over dim "ghost" segments, a floating peak
// segment, chromatic aberration, scanlines and a rising scan band, glitch
// slices that tear sideways on hard hits, and a HUD baseline with ticks and
// corner brackets. Output is premultiplied alpha.

cbuffer Eq : register(b2) {   // matches EqConstants in EqBars.cpp
    float4 eqValues[12];      // 48 bars, 0..1
    float4 eqPeaks[12];
    float3 eqLo;  float eqOpacity;
    float3 eqHi;  float eqBeat;
    float eqGlitch; float eqSeed; float eqTime; float eqPad;
};

static const int kBars = 48;
static const float kSkew = 0.22;   // columns lean right as they rise

float BarValue(int i) { return Pick(eqValues[i >> 2], i & 3); }
float PeakValue(int i) { return Pick(eqPeaks[i >> 2], i & 3); }

struct Layout {
    float scale, marginX, areaW, baseY, maxH, segH, gap, slot, barW;
    int segments;
};

Layout MakeLayout() {
    Layout L;
    L.scale = iResolution.y / 1080.0;                // authored at 1080p
    L.marginX = iResolution.x * 0.06;
    L.areaW = iResolution.x - 2.0 * L.marginX;
    L.baseY = iResolution.y - 60.0 * L.scale;
    L.segH = 7.0 * L.scale;                          // one LED segment incl. gap
    L.gap = 2.0 * L.scale;
    L.segments = (int)(iResolution.y * 0.17 / L.segH);
    L.maxH = L.segments * L.segH;
    L.slot = L.areaW / kBars;
    L.barW = L.slot * 0.68;
    return L;
}

float3 Neon(float t) {   // height through the column -> neon colour
    const float3 cyan = float3(0.0, 0.95, 1.0);
    const float3 magenta = float3(1.0, 0.1, 0.75);
    const float3 warning = float3(1.0, 0.85, 0.15);
    float3 c = lerp(cyan, magenta, smoothstep(0.15, 0.7, t));
    return lerp(c, warning, smoothstep(0.8, 0.95, t));
}

// Colour (premultiplied) and coverage of the LED columns at a pixel.
float4 Columns(float2 px, Layout L) {
    float height = L.baseY - px.y;                   // above the baseline
    if (height < 0.0 || height > L.maxH + L.segH) return 0;

    float x = px.x - height * kSkew - L.marginX;      // undo the lean
    float slotPos = x / L.slot;
    int b = (int)floor(slotPos);
    if (b < 0 || b >= kBars) return 0;
    float lx = (frac(slotPos) - 0.5) * L.slot;        // -slot/2 .. slot/2 within the column
    float dx = abs(lx) - L.barW * 0.5;                // > 0 outside the column
    float inColumn = smoothstep(1.0, -1.0, dx);

    int seg = (int)floor(height / L.segH);
    float within = height - seg * L.segH;
    float inSegment = smoothstep(L.gap - 0.5, L.gap + 0.5, within) * inColumn;
    float t = (seg + 0.5) / L.segments;

    float lit = BarValue(b) * L.segments;             // how many segments are on
    int peakSeg = (int)floor(PeakValue(b) * L.segments);
    float3 tint = lerp(eqLo, eqHi, t);

    float4 o = 0;
    if (seg < lit) {
        float top = seg >= lit - 1.0 ? 1.6 : 1.0;     // the leading segment burns brightest
        float3 c = lerp(Neon(t), tint, 0.2) * top * (1.0 + 0.6 * eqBeat);
        o += float4(c, 0.9) * inSegment;
    } else if (seg == peakSeg && peakSeg > 0) {
        o += float4(float3(1.0, 0.95, 0.6) * 1.8, 1.0) * inSegment;   // floating peak
    } else if (seg < L.segments) {
        o += float4(Neon(t) * 0.06, 0.18) * inSegment;                // ghost (unlit) segment
    }

    // Neon bloom spilling sideways from the lit part of the column.
    float glowH = saturate(lit / L.segments) * L.maxH;
    if (height < glowH && dx > 0.0)
        o.rgb += lerp(Neon(t), tint, 0.2) * exp(-dx / (5.0 * L.scale)) * 0.35 * (1.0 + eqBeat);
    return o;
}

float4 main(VSOut i) : SV_Target {
    Layout L = MakeLayout();
    float2 px = i.uv * iResolution;
    if (px.y < L.baseY - L.maxH - 30.0 * L.scale) return 0;   // well above the bars

    // --- Glitch: on hard hits, horizontal slices tear sideways for a moment ---
    float slice = floor(px.y / (9.0 * L.scale));
    float3 g = Hash33(int3((int)slice, (int)(eqSeed * 977.0), 31));
    float tear = step(1.0 - 0.4 * eqGlitch, g.x) * (g.y - 0.5) * 70.0 * L.scale * eqGlitch;
    px.x += tear;

    // --- Columns with chromatic aberration (red and blue sampled slightly apart) ---
    float ca = (1.2 + 3.5 * eqBeat + 6.0 * eqGlitch) * L.scale;
    float4 mid = Columns(px, L);
    float r = Columns(px + float2(ca, 0), L).r;
    float b = Columns(px - float2(ca, 0), L).b;
    float3 rgb = float3(r, mid.g, b);
    float alpha = mid.a;

    // --- Reflection: mirrored below the baseline, faded and scanlined ---
    float depth = px.y - L.baseY;
    if (depth > 0.0) {
        float4 refl = Columns(float2(px.x, L.baseY - depth * 1.6), L);
        float fade = saturate(1.0 - depth / (34.0 * L.scale)) * 0.22;
        rgb += refl.rgb * fade;
        alpha += refl.a * fade * 0.5;
    }

    // --- CRT feel: scanlines, a scan band rising through the bars, subtle flicker ---
    rgb *= 0.78 + 0.22 * step(0.5, frac(px.y / (3.0 * L.scale)));
    float scanY = L.baseY - frac(eqTime * 0.45) * (L.maxH + 60.0 * L.scale);
    rgb *= 1.0 + 0.6 * exp(-pow((px.y - scanY) / (5.0 * L.scale), 2.0));

    // --- HUD: baseline, tick marks every 4 columns, corner brackets ---
    float3 hud = float3(0.0, 0.9, 1.0);
    float lineY = L.baseY + 2.0 * L.scale;
    float left = L.marginX - 14.0 * L.scale, right = L.marginX + L.areaW + 14.0 * L.scale;
    float onSpan = step(left, px.x) * step(px.x, right);
    float baseLine = exp(-abs(px.y - lineY) / (0.8 * L.scale)) * onSpan;
    float tickX = frac((px.x - L.marginX) / (L.slot * 4.0) + 0.5 / 4.0);
    float tick = step(abs(tickX - 0.5) * L.slot * 4.0, 0.8 * L.scale) * step(lineY, px.y) * step(px.y, lineY + 7.0 * L.scale) * onSpan;
    // Brackets: short vertical strokes rising from each end, with an angled cut.
    float bracketH = 26.0 * L.scale;
    float nearEnd = min(abs(px.x - left), abs(px.x - right));
    float bracket = step(nearEnd, 1.2 * L.scale) * step(lineY - bracketH, px.y) * step(px.y, lineY);
    float cap = step(abs(px.y - (lineY - bracketH)), 1.0 * L.scale) *
                step(min(px.x - left, right - px.x), 10.0 * L.scale) * onSpan;
    float hudA = saturate(baseLine * 0.8 + tick * 0.6 + bracket + cap) * (0.55 + 0.45 * eqBeat);
    rgb += hud * hudA;
    alpha = max(alpha, hudA * 0.6);

    float flicker = 0.95 + 0.05 * Hash33(int3((int)(eqTime * 50.0), 7, 7)).x;
    return float4(rgb * flicker, saturate(alpha)) * eqOpacity;
}
