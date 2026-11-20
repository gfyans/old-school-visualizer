#include "common.hlsli"

// Neon Grid: a synthwave night drive. A wet four-lane road under rushing neon gates,
// flat neon grid either side, low-poly wireframe mountains built from the music
// (each terrain row is one moment's spectrum, entering at the horizon), a dense
// skyline sitting on the horizon and a big striped sun in the middle.
// Two-tone throughout: colour A on the left, colour B on the right.

cbuffer Neon : register(b2) {   // matches NeonConstants in NeonGridVisualizer.cpp
    float travel;      float camHeight;   float pitch;       float roll;
    float rowSpacing;  float farDist;     float maxHeight;   float tanHalfFov;
    float pulse;       float buildUp;     float glitch;      float glitchSeed;
    float sunRadius;   float roadLights;  float treble;      float horizonGlow;
    float3 skyTop;     float flash;
    float3 skyHorizon; float sunStripes;
    float3 sunTop;     float pulseZ;      // distance of the beat wave travelling towards us
    float3 sunBottom;  float gateGlow;
    float3 gridA;      float bass;        // left-hand neon
    float3 gridB;      float gateSpacing; // right-hand neon
    float3 groundCol;  float neonPad;
};

Texture2D<float> History : register(t0);   // 64 bands x 128 rows (rows = distance)

static const float kRoad = 2.4;        // road half-width (four lanes)
static const float kFlat = 6.5;        // flat neon grid out to here; mountains beyond
static const float kSideWidth = 24.0;  // mountain range width: treble near, bass far out
static const int kRows = 128;
static const float kGateHalfW = 3.0;
static const float kGateH = 3.4;
#define kTerrainFar (farDist * 2.0)    // mountains are drawn this far (rows beyond farDist are older music)

float3 SideColour(float x) { return lerp(gridA, gridB, smoothstep(-1.0, 1.0, x)); }
float PixelSize(float t) { return t * 2.0 * tanHalfFov / iResolution.y; }   // world size of a pixel at distance t

// ---------------------------------------------------------------- terrain (low-poly mesh)

// Height of a mesh vertex on the integer grid. Mountains only beyond the flat zone.
float VertexHeight(int ix, int iz) {
    float ax = abs((float)ix);
    float side = smoothstep(kFlat, kFlat + 3.0, ax);
    if (side <= 0.0) return 0.0;
    float d = saturate((ax - kFlat) / kSideWidth);              // 0 near the road .. 1 far out
    int band = (int)((1.0 - d) * 63.0 + 0.5);                  // treble near, bass far
    int zi = iz + (ix < 0 ? 1 : 0);                             // left/right read different moments
    int row = (int)((uint)(zi - (int)farDist + 1024) & (uint)(kRows - 1));   // row k sits at k + farDist
    float s = History.Load(int3(band, row, 0));
    float jitter = Hash33(int3(ix, iz & 1023, 99)).x;          // low-poly irregularity
    return maxHeight * side * ((0.08 + 0.22 * jitter) * (0.4 + d) + pow(saturate(s), 1.4) * (0.15 + 1.1 * d * d));
}

// Piecewise-linear over triangles (each grid cell split along its diagonal): a real mesh.
float Height(float x, float z) {
    if (abs(x) < kFlat - 1.0) return 0.0;
    int ix = (int)floor(x), iz = (int)floor(z);
    float2 f = float2(x - ix, z - iz);
    float v00 = VertexHeight(ix, iz), v10 = VertexHeight(ix + 1, iz);
    float v01 = VertexHeight(ix, iz + 1), v11 = VertexHeight(ix + 1, iz + 1);
    return (f.x + f.y < 1.0) ? v00 + (v10 - v00) * f.x + (v01 - v00) * f.y
                             : v11 + (v01 - v11) * (1.0 - f.x) + (v10 - v11) * (1.0 - f.y);
}

float3 FacetNormal(float x, float z) {
    int ix = (int)floor(x), iz = (int)floor(z);
    float2 f = float2(x - ix, z - iz);
    float v00 = VertexHeight(ix, iz), v10 = VertexHeight(ix + 1, iz);
    float v01 = VertexHeight(ix, iz + 1), v11 = VertexHeight(ix + 1, iz + 1);
    float3 a, b;
    if (f.x + f.y < 1.0) { a = float3(1, v10 - v00, 0); b = float3(0, v01 - v00, 1); }
    else                 { a = float3(-1, v01 - v11, 0); b = float3(0, v10 - v11, -1); }
    float3 n = normalize(cross(b, a));
    return n.y < 0.0 ? -n : n;
}

// ---------------------------------------------------------------- neon gates

// Glow of the gates along a ray (also used for reflections in the wet road).
float3 Gates(float3 o, float3 d, float tMax, float spread) {
    float3 acc = 0;
    if (d.z <= 1e-3) return acc;
    float phase = fmod(travel, gateSpacing);
    [unroll] for (int k = 1; k <= 7; ++k) {
        float zk = k * gateSpacing - phase;          // gate distance ahead of the camera
        float tg = (zk - o.z) / d.z;
        if (tg <= 0.0 || tg > tMax) continue;
        float3 q = o + d * tg;
        float ax = abs(q.x);
        float dPost = length(float2(ax - kGateHalfW, max(q.y - kGateH, 0.0) + max(-q.y, 0.0)));
        float dTop = length(float2(max(ax - kGateHalfW, 0.0), q.y - kGateH));
        float dd = min(dPost, dTop);
        float core = smoothstep(PixelSize(tg) * 1.5 + 0.025, 0.0, dd);
        float halo = exp(-dd / (0.14 + spread)) * 0.45;
        float fade = 1.0 - smoothstep(farDist * 0.35, farDist * 0.9, tg);
        float wave = 1.0 + 2.5 * pulse * exp(-pow(zk - pulseZ, 2.0) * 0.04);   // flashes as the beat passes
        acc += SideColour(q.x / kGateHalfW) * (core * 1.6 + halo) * fade * gateGlow * wave;
    }
    return acc;
}

// ---------------------------------------------------------------- sky, sun, city

float3 City(float sx, float sy, float3 col) {
    [unroll] for (int layer = 0; layer < 2; ++layer) {
        float colW = layer == 0 ? 0.016 : 0.045;
        float offset = layer * 13.7;
        float u = sx / colW + offset;
        int id = (int)floor(u);
        float local = u - floor(u);
        float3 h = Hash33(int3(id, 31 + layer * 17, 5));
        // Lower in the middle so the sun shows; towers climb towards the sides.
        float centreDip = 0.35 + 0.65 * smoothstep(0.05, 0.45, abs(sx));
        float height = (layer == 0 ? 0.05 + 0.12 * h.x * h.x : 0.03 + 0.27 * h.x * h.x) * centreDip;
        bool gapColumn = layer == 1 && h.y < 0.25;  // gaps in the front row reveal the back row
        // Stepped setbacks: the top part of taller buildings is narrower.
        float inset = 0.04 + (sy > height * 0.72 && h.x > 0.45 ? 0.2 : 0.0) + (sy > height * 0.9 && h.x > 0.7 ? 0.12 : 0.0);
        bool inside = !gapColumn && local > inset && local < 1.0 - inset && sy < height;
        if (inside) {
            float3 body = layer == 0 ? lerp(skyTop, skyHorizon, 0.35) * 0.55 : skyTop * 0.55;
            // Windows: a grid of cells, some lit.
            float cols = layer == 0 ? 3.0 : 5.0, rows = layer == 0 ? 260.0 : 170.0;
            float2 w = float2(local * cols, sy * rows);
            float3 hw = Hash33(int3(id * 37 + (int)floor(w.x), (int)floor(w.y), 7 + layer));
            float2 fw = frac(w);
            float lit = step(0.28, fw.x) * step(fw.x, 0.72) * step(0.3, fw.y) * step(fw.y, 0.78) * step(0.58, hw.x);
            float3 wc = lerp(float3(1.0, 0.82, 0.55), lerp(gridA, gridB, hw.y), 0.55 * hw.z);
            body += wc * lit * (layer == 0 ? 0.35 : 0.85);
            // Neon roof trim and the odd vertical sign.
            body += SideColour(sx * 3.0) * exp(-(height - sy) * (layer == 0 ? 1600.0 : 700.0)) * (layer == 0 ? 0.35 : 1.0);
            float sign = step(0.82, h.z) * exp(-abs(local - 0.5) * 50.0) * step(height * 0.35, sy) * step(sy, height * 0.85);
            body += lerp(gridA, gridB, h.y) * sign * (layer == 0 ? 0.5 : 1.3);
            // The city is the farthest thing in the scene, so it carries the most haze (that's what
            // puts it visibly *behind* the mountains), and its base dissolves into the horizon glow.
            float3 haze = skyHorizon * (1.0 + 0.8 * horizonGlow);
            body = lerp(body, haze, layer == 0 ? 0.55 : 0.35);
            col = lerp(haze, body, smoothstep(-0.002, layer == 0 ? 0.03 : 0.018, sy));
        }
        // Blinking antenna lights on the tallest towers.
        if (layer == 1 && h.x > 0.8) {
            float2 a = float2((local - 0.5) * colW, sy - height - 0.008);
            float blink = step(0.5, frac(iTime * 0.7 + h.y));
            col += float3(1.0, 0.15, 0.1) * smoothstep(0.0025, 0.0, length(a)) * blink * 1.5;
        }
    }
    return col;
}

float3 Sky(float3 rd) {
    float sx = rd.x / max(rd.z, 0.05), sy = rd.y / max(rd.z, 0.05);
    float3 col = lerp(skyHorizon, skyTop, saturate(rd.y * 2.4));
    col += skyHorizon * horizonGlow * 0.8 * exp(-abs(rd.y) * 14.0);

    // Stars.
    if (rd.y > 0.1) {
        float2 q = float2(sx, sy) * 90.0;
        float3 h = Hash33(int3(floor(q), 3) + 5000);
        float d = length(frac(q) - (0.2 + 0.6 * h.yz));
        float tw = 0.6 + 0.4 * sin(iTime * (2.0 + 5.0 * h.y) + h.z * 40.0);
        col += step(0.93, h.x) * smoothstep(0.12, 0.0, d) * tw * (0.5 + treble) * saturate((rd.y - 0.1) * 6.0);
    }

    // The sun: centred, hot-pink to gold, stripes scrolling down its lower half, big halo.
    float2 s = float2(sx, sy) - float2(0.0, 0.11);
    float r = length(s);
    col += lerp(sunBottom, sunTop, 0.45) * exp(-max(r - sunRadius, 0.0) * 5.5) * (0.5 + 0.35 * bass);
    if (r < sunRadius) {
        float v = s.y / sunRadius;                                      // -1 bottom .. 1 top
        // Saturated gradient: hot colour at the bottom climbing to a bright top.
        // (Its lower part is below the horizon, so the gradient spans the visible part.)
        float3 sun = lerp(sunBottom, sunTop, smoothstep(-0.25, 0.85, v));
        sun *= 0.85 + 0.25 * smoothstep(0.6, 1.0, r / sunRadius);       // slightly brighter rim
        float zone = saturate((0.45 - v) * 1.2);                        // stripes from above the middle down
        float stripe = frac(v * sunStripes - iTime * 0.3);
        float gap = step(stripe, 0.1 + zone * 0.5) * step(v, 0.45);
        col = lerp(col, sun, 1.0 - gap);
    }
    // Horizon flare: a thin hot line across the sky where the sun meets the land.
    col += lerp(sunBottom, sunTop, 0.3) * exp(-abs(sy) * 350.0) * exp(-abs(sx) * 1.2) * 0.7;

    if (sy > -0.003) col = City(sx, sy, col);
    return col;
}

// ---------------------------------------------------------------- main

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((i.uv.x * 2.0 - 1.0) * aspect, 1.0 - i.uv.y * 2.0);

    // Drop glitch: horizontal slices jump sideways for a moment.
    float3 g = Hash33(int3((int)floor(i.uv.y * 40.0), (int)(glitchSeed * 1000.0), 77));
    sp.x += step(1.0 - 0.5 * glitch, g.x) * (g.y - 0.5) * 0.3 * glitch;

    float cr = cos(roll), sr = sin(roll);
    sp = float2(cr * sp.x - sr * sp.y, sr * sp.x + cr * sp.y);
    float cp = cos(pitch), sn = sin(pitch);
    float3 up = float3(0.0, cp, -sn);
    float3 rd = normalize(float3(0.0, sn, cp) + (sp.x * float3(1, 0, 0) + sp.y * up) * tanHalfFov);
    float3 ro = float3(0.0, camHeight, 0.0);

    float3 fogCol = skyHorizon * (1.0 + 0.8 * horizonGlow);   // equals the sky right at the horizon
    float3 col = Sky(rd);
    float tScene = farDist;

    // --- Terrain: exact plane hits over the flat zone, careful steps over the mountains ---
    if (rd.y < 0.3) {
        float t = 0.2, prevT = 0.2;
        bool hit = false, flat = false;
        [loop] for (int s = 0; s < 180; ++s) {
            float3 p = ro + rd * t;
            if (abs(p.x) < kFlat - 1.0) {
                // Nothing but flat ground here: jump straight to it, or to where the ray leaves the zone.
                float toPlane = rd.y < 0.0 ? p.y / -rd.y : 1e9;
                float toExit = (kFlat - 1.0 - abs(p.x)) / max(abs(rd.x), 1e-4);
                if (toPlane <= toExit) { t += toPlane; hit = true; flat = true; break; }
                prevT = t;
                t += toExit + 0.01;
            } else {
                float gap = p.y - Height(p.x, travel + p.z);
                if (gap < 0.0) { hit = true; break; }
                prevT = t;
                // Conservative for steep slopes (no holes), coarser far away where a pixel is bigger.
                t += clamp(gap * 0.35, 0.015 + t * 0.001, 0.6 + t * 0.012);
            }
            if (t > kTerrainFar) break;
        }
        // All terrain is at or above ground level, so a downward ray that got this far without
        // a hit slipped between samples (or ran out of steps). Land it where it meets the
        // ground: never a hole showing the sky or city through the mountains.
        if (!hit && rd.y < 0.0) {
            float tPlane = ro.y / -rd.y;
            if (tPlane < kTerrainFar) { t = tPlane; hit = true; flat = true; }
        }
        if (hit && !flat) {
            float a = prevT, b = t;
            [unroll] for (int k = 0; k < 6; ++k) {
                float m = 0.5 * (a + b);
                float3 p = ro + rd * m;
                if (p.y < Height(p.x, travel + p.z)) b = m; else a = m;
            }
            t = b;
        }

        if (hit && t < kTerrainFar) {
            tScene = t;
            float3 p = ro + rd * t;
            float zW = travel + p.z;
            float ax = abs(p.x);
            float width = PixelSize(t) * 1.3 + 0.004;
            float3 side = SideColour(p.x);
            float3 shade;

            if (ax < kRoad) {
                // Wet road: dark gloss, dashed lanes, neon edges, and reflections of the neon.
                shade = groundCol * 0.6 + skyHorizon * 0.05;
                float lane = min(ax, abs(ax - kRoad * 0.5));
                float dash = step(frac(zW * 0.18), 0.45);
                shade += float3(0.85, 0.85, 1.0) * smoothstep(0.045 + width, 0.02, lane) * dash * 0.7;
                float3 rr = float3(rd.x, -rd.y, rd.z);
                float wet = 0.35 * (1.0 - smoothstep(10.0, farDist * 0.8, t));
                shade += Gates(p, rr, farDist, 0.25) * wet;
                float2 rs = float2(rr.x / rr.z, rr.y / rr.z) - float2(0.0, 0.11);
                shade += lerp(sunBottom, sunTop, 0.4) * exp(-max(length(rs) - sunRadius, 0.0) * 5.0) * exp(-abs(rs.x) * 4.0) * wet * 0.8;
                float edgeD = abs(ax - kRoad + 0.06);
                shade += side * (smoothstep(0.05 + width, 0.02, edgeD) * 1.8 + exp(-edgeD / 0.25) * 0.35) * (1.0 + 1.2 * roadLights);
            } else if (ax < kFlat) {
                // Flat neon grid: bright squares.
                float lx = abs(frac(p.x + 0.5) - 0.5), lz = abs(frac(zW + 0.5) - 0.5);
                float w2 = width + 0.012;   // a touch thicker: these lines should really glow
                float lines = smoothstep(w2, 0.0, lx) + smoothstep(w2, 0.0, lz) * (1.0 + pulse);
                float halo = exp(-min(lx, lz) / (w2 * 5.0)) * 0.45;
                shade = groundCol + side * (lines * 1.8 + halo);
            } else {
                // Mountains: flat-shaded low-poly facets with glowing wireframe edges.
                float3 n = FacetNormal(p.x, zW);
                float facet = 0.25 + 0.75 * saturate(dot(n, normalize(float3(-0.2 * sign(p.x), 0.6, 1.0))));
                float h01 = saturate(p.y / maxHeight);
                float2 f = float2(frac(p.x), frac(zW));
                float edgeD = min(min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y)), abs(f.x + f.y - 1.0) * 0.7071);
                float edge = smoothstep(width, 0.0, edgeD) + exp(-edgeD / (width * 3.0)) * 0.25;
                shade = groundCol * (1.0 + 3.0 * facet) + side * edge * (0.7 + 1.3 * h01);
            }
            // Capped fog: even the farthest ridges stay solid silhouettes in front of the city.
            float fog = smoothstep(farDist * 0.35, kTerrainFar, t) * 0.62;
            col = lerp(shade, fogCol, fog);
        } else {
            // The ray ran out of terrain without hitting anything. Out at the sides, a ray that
            // skims between ridges would really hit mountains further away, so fill a hazy
            // distant range there: otherwise the city behind shows through the gaps.
            float sx = rd.x / max(rd.z, 0.05), sy = rd.y / max(rd.z, 0.05);
            float sideMask = smoothstep(0.05, 0.11, abs(sx));    // mountains start beyond the flat zone
            float ridge = (0.018 + 0.016 * (0.5 + 0.5 * sin(sx * 23.0) * sin(sx * 9.0 + 1.3))) * sideMask;
            if (sy < ridge) {
                float3 farMountain = groundCol * 3.0 + SideColour(sx) * 0.12;
                col = lerp(farMountain, fogCol, 0.62);           // same haze as the farthest terrain
            } else if (rd.y < 0.0) {
                col = fogCol;   // flat ground beyond the terrain's range, all the way to the horizon
            }
        }
    }

    col += Gates(ro, rd, tScene, 0.0);
    col += lerp(gridA, gridB, 0.5) * flash * 0.3;
    col *= 0.93 + 0.07 * step(0.5, frac(i.pos.y * 0.5));    // faint scanlines
    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.55;                           // vignette
    col = 1.0 - exp(-col * 1.2);                             // soft clip
    return float4(col, 1);
}
