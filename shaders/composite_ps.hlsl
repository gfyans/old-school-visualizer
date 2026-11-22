#include "cyber.hlsli"

// Final pass: scales the visualiser image(s) to the window, plays the transition
// when switching visualisers, and dithers to hide 8-bit banding in dark gradients.
// Transition styles: 0 crossfade, 1 glitch cut, 2 zoom-through, 3 iris, 4 pixel dissolve.

Texture2D LayerA : register(t0);   // outgoing (or the only layer)
Texture2D LayerB : register(t1);   // incoming
SamplerState Linear : register(s0);

cbuffer Composite : register(b2) {
    float2 uvScaleA;
    float2 uvMaxA;
    float2 uvScaleB;
    float2 uvMaxB;
    float mixAmount;   // 0 -> 1 through the transition
    float hasB;
    float style;
    float seed;        // changes per transition, for randomised effects
    float warmth;      // mood colour: -1 cool .. +1 warm
    float hueShift;    // radians: nudged by chord changes
    float gradeAmount; // 0 = off
    float gradePad;
    float filterStyle; // glitch filter over any visualiser: 0 off, 1 ASCII .. 8 thermal (cyber.hlsli)
    float glitch;      // 0..1: how hard rows tear (snares, build-ups, drops)
    float filterTime;
    float filterPad;
    float3 palBg;   float palPad0;
    float3 palMid;  float palPad1;
    float3 palHi;   float palPad2;
    float3 palAccent; float palPad3;
};

// Mood colour grade: a gentle warm/cool white balance and a small hue rotation (in YIQ space,
// which keeps brightness while turning the colours).
float3 Grade(float3 c) {
    if (gradeAmount <= 0.0) return c;
    float3 yiq = float3(dot(c, float3(0.299, 0.587, 0.114)), dot(c, float3(0.596, -0.274, -0.322)), dot(c, float3(0.211, -0.523, 0.312)));
    float a = hueShift * gradeAmount, ca = cos(a), sa = sin(a);
    yiq.yz = float2(yiq.y * ca - yiq.z * sa, yiq.y * sa + yiq.z * ca);
    float3 rgb = float3(dot(yiq, float3(1.0, 0.956, 0.621)), dot(yiq, float3(1.0, -0.272, -0.647)), dot(yiq, float3(1.0, -1.106, 1.703)));
    rgb *= 1.0 + warmth * gradeAmount * float3(0.08, 0.0, -0.09);
    return max(rgb, 0.0);
}

float3 SampleA(float2 uv) { return LayerA.SampleLevel(Linear, min(saturate(uv) * uvScaleA, uvMaxA), 0).rgb; }
float3 SampleB(float2 uv) { return LayerB.SampleLevel(Linear, min(saturate(uv) * uvScaleB, uvMaxB), 0).rgb; }

float3 Transition(float2 uv, float2 pixel) {
    float t = saturate(mixAmount);
    int s = (int)style;
    float aspect = iResolution.x / iResolution.y;
    float3 result = 0;

    [branch] if (s == 1) {
        // Glitch cut: horizontal slices flip to the new scene at random moments, with an
        // RGB split and slice jitter that peak half-way through.
        float slice = floor(uv.y * 28.0);
        float3 h = Hash33(int3((int)slice, (int)(seed * 9973.0), 5));
        float chaos = sin(t * 3.14159);
        float2 jitter = float2((h.y - 0.5) * 0.08 * chaos, 0.0);
        bool useB = h.x < t * 1.25 - 0.1;
        float split = 0.012 * chaos;
        float3 c = 0;
        if (useB) c = float3(SampleB(uv + jitter + float2(split, 0)).r, SampleB(uv + jitter).g, SampleB(uv + jitter - float2(split, 0)).b);
        else      c = float3(SampleA(uv + jitter + float2(split, 0)).r, SampleA(uv + jitter).g, SampleA(uv + jitter - float2(split, 0)).b);
        result = c * (1.0 + 0.5 * chaos * step(0.92, h.z));   // occasional bright slice
    } else if (s == 2) {
        // Zoom-through: fly into the old scene while the new one arrives from behind.
        float2 c = uv - 0.5;
        float3 a = SampleA(0.5 + c / (1.0 + 2.5 * t * t));
        float3 b = SampleB(0.5 + c * (1.6 - 0.6 * t));
        float k = smoothstep(0.35, 0.75, t);
        result = lerp(a, b, k) + 0.25 * exp(-pow((t - 0.55) * 6.0, 2.0));   // flash at the crossing
    } else if (s == 3) {
        // Iris: the new scene opens from the centre behind a glowing neon ring.
        float2 p = (uv - 0.5) * float2(aspect, 1.0);
        float radius = t * t * (0.6 * aspect + 0.6);
        float d = length(p) - radius;
        float3 c = d < 0.0 ? SampleB(uv) : SampleA(uv);
        float ring = exp(-abs(d) * 90.0) * (1.0 - t);
        result = c + float3(0.4, 0.9, 1.0) * ring * 1.5;
    } else if (s == 4) {
        // Pixel dissolve: blocks grow, the scene swaps at the coarsest point, then resolves.
        float blocks = lerp(1.0, 48.0, sin(t * 3.14159));
        float2 cell = floor(pixel / blocks) * blocks + blocks * 0.5;
        float2 q = cell / iResolution;
        float3 h = Hash33(int3(floor(pixel / blocks), (int)(seed * 977.0)));
        bool useB = h.x < smoothstep(0.3, 0.7, t);
        result = useB ? SampleB(q) : SampleA(q);
    } else {
        result = lerp(SampleA(uv), SampleB(uv), smoothstep(0.0, 1.0, t));   // crossfade
    }
    return result;
}

float3 Fetch(float2 uv, float2 pixel) { return hasB > 0.5 ? Transition(uv, pixel) : SampleA(uv); }

// The brightest of five samples across a cell (its centre and four points around it), so thin
// bright lines that miss the centre still light the cell instead of flickering in and out.
float3 FetchCell(float2 cuv, float2 centre, float2 size) {
    float2 o = size * 0.3 / iResolution;
    float3 best = Fetch(cuv, centre);
    float bestL = Luma(best);
    [unroll] for (int k = 0; k < 4; ++k) {
        float2 d = float2((k & 1) ? o.x : -o.x, (k & 2) ? o.y : -o.y);
        float3 c = Fetch(cuv + d, centre);
        float l = Luma(c);
        if (l > bestL) { best = c; bestL = l; }
    }
    return best;
}

float3 Hue(float3 c) { return c / max(max(c.r, max(c.g, c.b)), 1e-3); }

// ASCII: glyphs inked partly in the palette and partly in the scene's own colour, over a dim copy
// of the scene, so you can still tell which visualiser is underneath.
float3 FilterAscii(float2 px, float2 uv, float2 size, float tear, CyberPalette pal) {
    float2 cell = floor(px / size);
    float2 centre = (cell + 0.5) * size;
    float2 local = (px - cell * size) / size;
    float3 src = FetchCell((centre + float2(tear, 0.0)) / iResolution, centre, size);
    float l = saturate(pow(Luma(src), 0.8) * 1.15);
    float m = CyberAsciiMask(l, local, cell, glitch, filterTime);
    float3 ink = lerp(CyberAsciiInk(src, l, pal), Hue(src) * (0.35 + 0.9 * l), 0.55);
    float3 backdrop = pal.bg + Fetch(uv + float2(tear / iResolution.x, 0.0), px) * 0.16;
    return lerp(backdrop, ink, m);
}

// Dither: five shades from the background to the highlight (brightness lifted first, so dark
// scenes don't vanish into the background), with the middle shades taking the scene's hue.
float3 FilterDither(float3 src, int2 cellIndex, CyberPalette pal) {
    float l = pow(saturate((Luma(src) * 1.3 - 0.06) / 0.94), 0.8);   // near-black stays background
    float cool = saturate((src.b - max(src.r, src.g)) * 4.0);
    float3 mid = lerp(lerp(pal.mid, pal.accent, step(0.5, cool)), Hue(src) * 0.95, 0.5);
    int level = min((int)floor(l * 4.0 + Bayer8(cellIndex)), 4);
    if (level <= 0) return pal.bg;
    if (level == 1) return lerp(pal.bg, mid, 0.3);
    if (level == 2) return mid;
    if (level == 3) return lerp(mid, pal.hi, 0.5);
    return pal.hi;
}

// VHS: soft luma, colour smeared to the right, washed out, with a slow wobble, a tracking band
// rolling up the screen, head-switching noise along the bottom and the odd dropout. On drops the
// tracking slips and whole bands of the picture jump sideways.
float3 FilterVhs(float2 px, float tear) {
    float s = iResolution.y / 1080.0;
    float t = filterTime;
    float2 uv = px / iResolution;
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
    float shift = wobble + bandShift + headShift + dropShift + tear;
    float2 base = float2(px.x + shift, px.y);

    // Luma from three close taps; chroma from six taps reaching to the right.
    float y = 0.0;
    [unroll] for (int a = -1; a <= 1; ++a) y += Luma(Fetch((base + float2(a * 2.0 * s, 0.0)) / iResolution, px)) / 3.0;
    float2 iq = 0.0;
    [unroll] for (int b = 0; b < 6; ++b) {
        float3 c = Fetch((base + float2((b * 3.2 - 3.0) * s, 0.0)) / iResolution, px);
        iq += float2(dot(c, float3(0.596, -0.274, -0.322)), dot(c, float3(0.211, -0.523, 0.312))) / 6.0;
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
    return c;
}

// Polaroid: softened, faded colour. Highlights roll off to cream, blacks lift to a teal-green,
// warm vignette and film grain. Drops flare a warm light leak in from one edge.
float3 FilterPolaroid(float2 px) {
    float s = iResolution.y / 1080.0;
    float2 uv = px / iResolution;
    float3 c = Fetch(uv, px) * 0.5;
    float2 o = 1.5 * s / iResolution;
    c += (Fetch(uv + float2(o.x, 0), px) + Fetch(uv - float2(o.x, 0), px) + Fetch(uv + float2(0, o.y), px) + Fetch(uv - float2(0, o.y), px)) * 0.125;

    c = (1.0 - exp(-c * 1.5)) * 1.2;
    c = lerp(Luma(c), c, 0.75);
    float3 shadow = float3(0.07, 0.10, 0.095), highlight = float3(1.0, 0.95, 0.82);
    c = shadow + saturate(c) * (highlight - shadow);
    float l = Luma(c);
    c += float3(0.03, 0.01, -0.03) * l * (1.0 - l) * 4.0;   // warm mids

    float aspect = iResolution.x / iResolution.y;
    float v = length((uv - 0.5) * float2(aspect * 0.75, 1.0));
    c *= lerp(1.0, float3(0.72, 0.64, 0.56), smoothstep(0.35, 0.95, v));

    float side = Hash33(int3((int)aStructure.w, 7, 13)).x;
    float edge = side < 0.5 ? uv.x : 1.0 - uv.x;
    c += aStructure.y * exp(-edge * 5.0) * float3(1.0, 0.45, 0.15) * 0.55;

    float grain = Hash33(int3(floor(px / max(1.0, 1.5 * s)), (int)(filterTime * 24.0))).x - 0.5;
    return max(c + grain * 0.045, 0.0);
}

// Halftone: three rotated screens of dots in cyan, magenta and yellow on a dark ground, each dot's
// area matching how much of that ink the colour needs. Dots swell a little with the bass.
float3 FilterHalftone(float2 px, float tear) {
    float s = iResolution.y / 1080.0;
    float cell = 7.0 * s;
    float swell = 1.0 + 0.08 * aBands.x;
    static const float kAngle[3] = { 0.2618, 1.309, 0.0 };   // 15, 75 and 0 degrees
    static const float3 kInk[3] = { float3(0.0, 1.0, 1.0), float3(1.0, 0.0, 1.0), float3(1.0, 1.0, 0.0) };
    float3 col = float3(0.02, 0.02, 0.03);
    [unroll] for (int k = 0; k < 3; ++k) {
        float ca = cos(kAngle[k]), sa = sin(kAngle[k]);
        float2 q = float2(ca * px.x + sa * px.y, -sa * px.x + ca * px.y);
        float2 qc = (floor(q / cell) + 0.5) * cell;
        float2 centre = float2(ca * qc.x - sa * qc.y, sa * qc.x + ca * qc.y);
        float3 src = Fetch((centre + float2(tear, 0.0)) / iResolution, centre);
        // Split the colour into amounts of cyan (g+b), magenta (r+b) and yellow (r+g) light.
        // Faint haze is left out, so dark areas stay clean.
        float3 amt = saturate((float3(src.g + src.b - src.r, src.r + src.b - src.g, src.r + src.g - src.b) * 0.5 - 0.04) * 1.6);
        float a = k == 0 ? amt.x : k == 1 ? amt.y : amt.z;
        float r = cell * sqrt(a / 3.14159) * swell;
        col += kInk[k] * smoothstep(r + 0.7, r - 0.7, length(q - qc)) * saturate(r / 0.7);
    }
    return saturate(col);
}

// Thermal: brightness through a heat-camera ramp (black, violet, magenta, orange, yellow, white),
// slightly blurred like a low-resolution sensor. Drops run the whole picture hotter.
float3 FilterThermal(float2 px, float tear) {
    float s = iResolution.y / 1080.0;
    float2 uv = (px + float2(tear, 0.0)) / iResolution;
    float2 o = 2.0 * s / iResolution;
    float3 c = Fetch(uv, px) * 0.4 + (Fetch(uv + float2(o.x, 0), px) + Fetch(uv - float2(o.x, 0), px) +
                                      Fetch(uv + float2(0, o.y), px) + Fetch(uv - float2(0, o.y), px)) * 0.15;
    float heat = saturate(0.06 + pow(Luma(c), 0.7) * 1.05 + aStructure.y * 0.15);
    heat += (Hash33(int3(floor(px / max(1.0, 2.0 * s)), (int)(filterTime * 30.0))).x - 0.5) * 0.02;
    static const float3 kRamp[6] = { float3(0.0, 0.0, 0.02), float3(0.15, 0.0, 0.45), float3(0.6, 0.0, 0.6),
                                     float3(0.95, 0.25, 0.15), float3(1.0, 0.7, 0.05), float3(1.0, 1.0, 0.85) };
    float f = saturate(heat) * 5.0;
    int i = min((int)f, 4);
    return lerp(kRamp[i], kRamp[i + 1], f - i);
}

// The glitch filter: restyle the finished picture.
float3 Glitched(float2 uv) {
    int style = (int)filterStyle;
    float2 px = uv * iResolution;
    CyberPalette pal;
    pal.bg = palBg; pal.mid = palMid; pal.hi = palHi; pal.accent = palAccent;
    float rowTear = CyberTear(px.y, filterTime, glitch, 0.0);   // tears only on hits
    if (style == kStyleVhs) return FilterVhs(px, rowTear);
    if (style == kStylePolaroid) return FilterPolaroid(px);
    if (style == kStyleHalftone) return FilterHalftone(px, rowTear);
    if (style == kStyleThermal) return FilterThermal(px, rowTear);

    // Cell styles: snap cells to whole pixels so the grid stays even (scan lines as they always were).
    float2 size = CyberCellSize(style, iResolution.y);
    if (style != kStyleScan) size = max(1.0, round(size));
    float2 centre, local;
    CyberCell(px, size, centre, local);
    float tear = CyberTear(centre.y, filterTime, glitch, 0.0);
    float2 cuv = (centre + float2(tear, 0.0)) / iResolution;
    float3 result = 0;
    if (style == kStyleAscii) result = FilterAscii(px, uv, size, tear, pal);
    else if (style == kStyleDither) result = FilterDither(Fetch(cuv, centre), int2(floor(px / size)), pal);
    else if (style == kStyleLed) result = CyberLed(FetchCell(cuv, centre, size), local);
    else {
        // Scan: RGB fringes (wider when it glitches) and streaks smeared out of bright areas to the left.
        float split = (3.0 + 12.0 * glitch) / iResolution.x;
        float3 streak = 0;
        float reach = (8.0 + 30.0 * Hash33(int3((int)(centre.y / 4.0), 5, 9)).x) / iResolution.x;
        [unroll] for (int k = 1; k <= 4; ++k) streak = max(streak, Fetch(cuv - float2(reach * k, 0), centre) * (1.0 - k * 0.2));
        result = CyberScan(Fetch(cuv + float2(split, 0), centre), Fetch(cuv, centre), Fetch(cuv - float2(split, 0), centre), streak,
                           local, floor(px / size), filterTime, glitch);
    }
    return result;
}

float4 main(VSOut i) : SV_Target {
    float3 c;
    if (filterStyle > 0.5) c = Glitched(i.uv);
    else c = Grade(Fetch(i.uv, i.pos.xy));
    c += (InterleavedGradientNoise(i.pos.xy) - 0.5) / 255.0;
    return float4(c, 1);
}
