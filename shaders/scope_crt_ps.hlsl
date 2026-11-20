#include "common.hlsli"

// Oscilloscope CRT. Built up the way a real tube looks:
//   bezel   moulded plastic lit from above, a bevelled inner edge catching the light and the
//           trace's glow spilling onto it
//   recess  the glass sits back from the bezel: inner shadow where the bezel overhangs
//   glass   curved (barrel distortion, darker towards the corners), with a soft window
//           reflection, a highlight where the glass curves up towards the ceiling and a
//           thin bright rim at its edge
//   phosphor faintly grainy grey-green when unlit; the trace is a white-hot core fading to
//           a coloured afterglow, with bloom and wide halation in the thick glass

cbuffer Crt : register(b2) {   // matches CrtConstants in ScopeVisualizer.cpp
    float3 coreCol;   float bloom;
    float3 trailCol;  float curvature;
    float3 gridCol;   float flicker;
    float glare;      float crtPad0; float crtPad1; float crtPad2;
};

Texture2D<float2> Phosphor : register(t0);   // R slow afterglow, G fast core (with mips for bloom)
SamplerState Linear : register(s0);

float SdRoundBox(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float px = 2.0 / iResolution.y;                     // one pixel in these units

    // Window -> glass coordinates: the glass is inset inside the bezel, then bulges (barrel).
    float2 w = (i.uv * 2.0 - 1.0) * float2(aspect, 1.0);    // aspect-correct, height units
    float2 c = (i.uv * 2.0 - 1.0) * 1.08;
    float2 cb = c * (1.0 + curvature * float2(c.y * c.y, c.x * c.x));
    float2 suv = cb * 0.5 + 0.5;
    float2 q = cb * float2(aspect, 1.0);

    float sdGlass = SdRoundBox(q, float2(aspect, 1.0), 0.14);   // < 0 on the glass
    float glassMask = smoothstep(px, -px, sdGlass);

    // ---------------- glass ----------------
    float3 glass = 0;
    {
        // Unlit phosphor: very dark grey-green with a fine grain.
        float grain = Hash33(int3(i.pos.xy, 41)).x;
        glass = float3(0.020, 0.024, 0.022) * (0.9 + 0.2 * grain);

        float2 p = Phosphor.SampleLevel(Linear, suv, 0);
        float glowR = 0.0, glowG = 0.0, halo = 0.0;
        [unroll] for (int m = 2; m <= 5; ++m) {
            float2 g = Phosphor.SampleLevel(Linear, suv, m);
            float wgt = 1.0 / (m - 1);
            glowR += g.r * wgt;
            glowG += g.g * wgt;
        }
        halo = Phosphor.SampleLevel(Linear, suv, 6).r + Phosphor.SampleLevel(Linear, suv, 7).r;   // halation in thick glass
        glass += trailCol * p.r * 0.9 + coreCol * p.g * 0.7;
        glass += (trailCol * glowR + coreCol * glowG * 0.3) * bloom + trailCol * halo * 0.35;

        // Graticule (etched on the inside of the glass, so it bends with it).
        float2 g = suv * float2(10.0, 8.0);
        float2 gd = abs(frac(g + 0.5) - 0.5) / float2(10.0, 8.0) * iResolution;
        float grid = max(smoothstep(1.0, 0.0, gd.x), smoothstep(1.0, 0.0, gd.y));
        float2 cc = abs(suv - 0.5) * iResolution;
        float cross = max(smoothstep(1.2, 0.0, cc.x), smoothstep(1.2, 0.0, cc.y));
        float2 tick = abs(frac(suv * float2(50.0, 40.0) + 0.5) - 0.5) / float2(50.0, 40.0) * iResolution;
        float ticks = smoothstep(1.0, 0.0, tick.x) * step(cc.y, 5.0) + smoothstep(1.0, 0.0, tick.y) * step(cc.x, 5.0);
        glass += gridCol * (grid * 0.2 + saturate(cross + ticks) * 0.26);

        glass *= 0.86 + 0.14 * step(0.5, frac(i.pos.y * 0.5));      // scanlines
        glass *= 1.0 - 0.45 * pow(saturate(dot(c, c) * 0.5), 1.5);   // curvature: corners fall away into shadow
        glass *= 1.0 + flicker;

        // Inner shadow where the bezel overhangs the recessed glass (heavier at the top).
        float edgeDist = -sdGlass;
        float shadowW = 0.05 + 0.03 * saturate(-c.y);
        glass *= lerp(0.35, 1.0, smoothstep(0.0, shadowW, edgeDist));

        // Reflections on the glass.
        // (Glass coordinates run downwards: negative y is the top of the screen.)
        float2 wr = q - float2(-aspect * 0.45, -0.55);                           // a soft window, upper left
        float windowRefl = smoothstep(0.25, -0.05, SdRoundBox(wr, float2(0.32, 0.18), 0.15));
        float frame = max(smoothstep(0.03, 0.0, abs(wr.x + 0.02)), smoothstep(0.03, 0.0, abs(wr.y - 0.02)));
        windowRefl *= 1.0 - 0.75 * frame;                                         // window panes: a soft cross-bar
        float ceiling = exp(-max(1.0 + cb.y, 0.0) * 9.0) * smoothstep(aspect, 0.0, abs(q.x));   // top edge curving up
        float rim = exp(-edgeDist / (3.0 * px)) * 0.6 + exp(-edgeDist / 0.02) * 0.15;          // light along the glass edge
        float sheen = 0.015 * (1.0 - suv.y);                                                   // faint room light
        // A broad diagonal sheen: the curved glass catching light across its face.
        float diag = dot(q, normalize(float2(1.0, -0.7)));
        float streak = exp(-pow((diag + 0.35) / 0.32, 2.0)) * smoothstep(0.0, 0.4, edgeDist);
        glass += float3(0.85, 0.9, 1.0) * glare * (windowRefl * 0.10 + ceiling * 0.08 + rim * 0.22 + streak * 0.035 + sheen);
    }

    // ---------------- bezel ----------------
    float3 bezel;
    {
        float sdOuter = sdGlass;                                  // distance out from the glass edge
        float bevelW = 0.06;
        // Moulded plastic, lit from above, with a fine texture.
        float grain = Hash33(int3(i.pos.xy / 2.0, 9)).x;
        bezel = float3(0.085, 0.085, 0.09) * (0.7 + 0.45 * (1.0 - i.uv.y)) * (0.94 + 0.12 * grain);
        // Bevel: the inner lip slopes down towards the glass. Its normal points back towards
        // the glass edge, so light from above brightens the bottom lip and the top lip falls in shadow.
        if (sdOuter > 0.0 && sdOuter < bevelW) {
            float2 n = normalize(float2(ddx(sdOuter), ddy(sdOuter)) + 1e-6);   // outward in pixel space (y down)
            float lit = 0.5 + 0.5 * dot(-n, float2(0.0, -1.0));                // light coming from above
            float slope = 1.0 - sdOuter / bevelW;
            bezel *= lerp(1.0, 0.35 + 2.0 * lit, slope);
            // The trace's glow spills onto the lip nearest the glass.
            float spill = Phosphor.SampleLevel(Linear, saturate(suv), 7).r;
            bezel += trailCol * spill * 0.8 * slope * slope;
        }
        // Thin highlight on the outer edge of the bevel, and a crisp dark seam at the glass.
        bezel += float3(0.09, 0.09, 0.1) * exp(-abs(sdOuter - bevelW) / (1.5 * px)) * (0.5 + 0.5 * (1.0 - i.uv.y));
        bezel *= lerp(0.25, 1.0, smoothstep(0.0, 4.0 * px, sdOuter));
    }

    float3 col = lerp(bezel, glass, glassMask);
    col = 1.0 - exp(-col * 1.3);
    return float4(col, 1);
}
