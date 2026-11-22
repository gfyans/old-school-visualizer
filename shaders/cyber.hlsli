#ifndef CYBER_HLSLI
#define CYBER_HLSLI
#include "common.hlsli"

// Cyberpunk glitch styles, shared by the Signal visualiser and the ASCII, dither, LED and scan-line
// filters (shaders/filter_*_ps.hlsl), so the two always look alike. Each style quantises the picture
// into cells; the caller samples (or renders) the source at the cell's centre, then shades the cell here.
//   1 ASCII     glyphs from a brightness ramp, partly in the source's colour, over a dim copy of it
//   2 Dither    ordered (Bayer) dithering in five shades of a four-colour palette
//   3 LED       rounded lamps, saturated, in a few brightness steps
//   4 Scan      thin horizontal scan lines, torn sideways with RGB fringes

static const int kStyleOff = 0, kStyleAscii = 1, kStyleDither = 2, kStyleLed = 3, kStyleScan = 4;

// Cell size in pixels for each style, at 1080p (scaled with the picture's height). Snapped to whole
// pixels so glyphs and lamps stay even (scan lines aren't: their spacing was tuned unsnapped).
float2 CyberCellSize(int style, float height) {
    float s = height / 1080.0;
    float2 size = float2(1.0, 1.0);
    if (style == kStyleAscii) size = float2(6.0, 9.0) * s;
    else if (style == kStyleDither) size = float2(2.0, 2.0) * s;
    else if (style == kStyleLed) size = float2(6.0, 6.0) * s;
    else if (style == kStyleScan) return float2(1.0, 4.0) * s;
    return max(1.0, round(size));
}

// Snap a pixel to its cell: the centre (pixels) and where in the cell it is (0..1).
void CyberCell(float2 pixel, float2 size, out float2 centre, out float2 local) {
    float2 cell = floor(pixel / size);
    centre = (cell + 0.5) * size;
    local = (pixel - cell * size) / size;
}

// 5x5 glyphs, top row in the high bits: " .:-=+*#%@" then "0" and "1" for data flicker.
static const int kGlyphs[12] = { 0, 4, 131200, 14336, 1016800, 4357252, 22511061, 11512810, 27070835, 15455951, 15324974, 4591758 };

float GlyphMask(int glyph, float2 local) {
    // A one-pixel margin around the 5x5 glyph inside each cell (7 x 7 grid).
    int2 p = int2(floor(local * float2(7.0, 7.0))) - 1;
    if (any(p < 0) || any(p > 4)) return 0.0;
    int bit = (4 - p.y) * 5 + (4 - p.x);
    return (kGlyphs[glyph] >> bit) & 1;
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// Ordered dithering threshold, 0..1, from an 8x8 Bayer matrix.
float Bayer8(int2 p) {
    p &= 7;
    int a = p.x ^ p.y;
    // Interleave bits of (x ^ y) and y: the standard Bayer construction.
    int v = ((a & 1) << 5) | ((p.y & 1) << 4) | ((a & 2) << 2) | ((p.y & 2) << 1) | ((a & 4) >> 1) | ((p.y & 4) >> 2);
    return (v + 0.5) / 64.0;
}

struct CyberPalette { float3 bg, mid, hi, accent; };

// --- Shading a cell, given the source colour at its centre -----------------------------------------

// The glyph for a cell at brightness l (0..1), and whether this pixel is inked.
float CyberAsciiMask(float l, float2 local, float2 cell, float flicker, float time) {
    int glyph = (int)round(l * 9.0);
    // Hi-hats make cells flicker to 0s and 1s.
    float3 h = Hash33(int3(cell, (int)(time * 12.0)));
    if (h.x < flicker * 0.35 && l > 0.08) glyph = 10 + (h.y > 0.5);
    return GlyphMask(glyph, local);
}

// Palette ink for a glyph: bright glyphs go ice-white, mid ones take the main colour; cool areas of
// the source pick the accent.
float3 CyberAsciiInk(float3 src, float l, CyberPalette pal) {
    float cool = saturate((src.b - src.r) * 3.0);
    float3 ink = lerp(lerp(pal.mid, pal.accent, cool), pal.hi, smoothstep(0.55, 0.95, l));
    return ink * (0.6 + 0.6 * l);
}

float3 Hue(float3 c) { return c / max(max(c.r, max(c.g, c.b)), 1e-3); }

// ASCII: glyphs from the source's (lifted) brightness, inked partly in the palette and partly in the
// source's own colour, over a dim copy of the picture (backdrop), so the subject stays easy to read.
float3 CyberAscii(float3 src, float3 backdrop, float2 local, float2 cell, float flicker, float time, CyberPalette pal) {
    float l = saturate(pow(max(Luma(src), 0.0), 0.8) * 1.15);
    float m = CyberAsciiMask(l, local, cell, flicker, time);
    float3 ink = lerp(CyberAsciiInk(src, l, pal), Hue(src) * (0.35 + 0.9 * l), 0.55);
    return lerp(pal.bg + backdrop * 0.16, ink, m);
}

// Dither: ordered (Bayer) dithering in five shades from the palette's background to its highlight.
// Brightness is lifted first, above a black floor, so dark areas stay background but dim detail
// still shows; the middle shades take the source's own hue.
float3 CyberDither(float3 src, int2 cellIndex, CyberPalette pal) {
    float l = pow(saturate((Luma(src) * 1.3 - 0.06) / 0.94), 0.8);
    float cool = saturate((src.b - max(src.r, src.g)) * 4.0);
    float3 mid = lerp(lerp(pal.mid, pal.accent, step(0.5, cool)), Hue(src) * 0.95, 0.5);
    int level = min((int)floor(l * 4.0 + Bayer8(cellIndex)), 4);
    if (level <= 0) return pal.bg;
    if (level == 1) return lerp(pal.bg, mid, 0.3);
    if (level == 2) return mid;
    if (level == 3) return lerp(mid, pal.hi, 0.5);
    return pal.hi;
}

float3 CyberLed(float3 src, float2 local) {
    float2 d = abs(local - 0.5);
    float box = max(d.x, d.y);
    float lamp = smoothstep(0.44, 0.36, box + 0.06 * length(d));   // rounded square with a gap
    // Saturated colour in six brightness steps; unlit cells glow faintly.
    float l = Luma(src);
    float3 hue = src / max(max(src.r, max(src.g, src.b)), 1e-3);
    float level = floor(saturate(l * 1.3) * 6.0) / 6.0;
    return hue * lamp * max(level, 0.06) * 1.2;
}

// Scan slices: how far this row is torn sideways (in pixels), from glitch strength 0..1. On hits,
// whole bands rip across; `idle` is the share of rows slightly off even when the music is quiet
// (Signal keeps a few; the filters pass 0 so they only tear on hits).
float CyberTear(float row, float time, float glitch, float idle = 0.04) {
    float band = floor(row / 6.0);
    float3 h = Hash33(int3((int)band, (int)(time * 15.0), 77));
    float torn = step(1.0 - (idle + glitch * 0.35), h.x);
    return (h.y - 0.5) * (12.0 + 140.0 * glitch) * torn;
}

// Scan lines: thin bright lines with dark gaps, RGB-split, colours quantised a little, with
// streaks smeared sideways out of bright areas and coloured speckle. srcR/G/B: the source sampled
// with the red / green / blue channel offsets; streak: the brightest of a few samples to the left.
float3 CyberScan(float3 srcR, float3 srcG, float3 srcB, float3 streak, float2 local, float2 cell, float time, float glitch) {
    float3 c = float3(srcR.r, srcG.g, srcB.b);
    c = max(c, streak * 0.55);
    c = floor(c * 6.0 + 0.5) / 6.0;
    float scan = smoothstep(0.5, 0.15, abs(local.y - 0.35));   // a line in the top half, dark below
    float3 col = c * scan * 1.6;
    float3 h = Hash33(int3(cell, (int)(time * 30.0)));
    // Speckle only where there's something to see, or when it glitches.
    const float present = saturate(Luma(c) * 4.0 + glitch);
    col += step(0.996 - glitch * 0.03, h.x) * float3(h.y, h.z, 1.0 - h.y) * scan * 1.5 * present;
    return col;
}

#endif
