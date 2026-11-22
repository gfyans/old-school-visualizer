#include "common.hlsli"

// Glass City: flying through an endless cyberspace city of glass. Every tower is a translucent box with
// thin glowing edges, walls of glass tiles that light up with the music, and currents of light running up
// circuit traces; rays carry on through each pane, so towers stack up in layers like the films' "glass
// cities". The floor is a black mirror with data lanes and traces; slabs of glass float overhead; some
// looks close the sky with a ceiling city hanging upside down. Light streaks race past along the avenues.
//
// The city is a grid of 1-unit cells traced exactly with a 2D DDA. Each cell can hold a tower (from the
// floor), a hanging block (from the ceiling) and a piece of a floating plate (plates span several cells).
// Avenues two cells wide run along z every 8 cells (the camera flies down them); streets cross every 12.
// The world repeats every kPeriod cells along z so the flight can go on forever without losing precision.
// The output is HDR; glasscity_post_ps.hlsl adds bloom, speed blur and tone mapping.

cbuffer GlassCity : register(b2) {   // matches GlassCityConstants in GlassCityVisualizer.cpp
    float3 camPos;   float focal;
    float3 camRight; float time;
    float3 camUp;    float speed;      // flight speed, units/s
    float3 camFwd;   float ceiling;    // 0 open sky .. 1 a ceiling city overhead
    float3 edgeCol;  float lights;     // edges and traces; share of tiles lit
    float3 tileCol;  float current;    // lit glass tiles; how busy the currents are
    float3 sparkCol; float sparkle;    // warm accents; hi-hat glints
    float3 floorCol; float buildUp;    // floor rings
    float3 fogCol;   float over;       // haze and the vanishing glow; overdrive 0..1 (after a drop)
    float3 glassCol; float shockR;     // glass tint; the drop's shockwave radius (0 = none)
    float4 ringAge;                    // seconds since the last four beats
    float4 ringHit;                    // their strengths
    float4 ringZ;                      // where each ring started (z, on the camera's avenue)
    float2 shockPos; float laneX;      float tileSeed;   // shockwave centre (xz); the avenue's centre x; changes on snares
    float streakPhase; float streakAmt; float streakLen; float pad;
};

static const float kCeil = 9.0;        // height of the ceiling (and of the slab the DDA walks)
static const int kAvenue = 8;
static const int kStreet = 12;
static const int kPeriod = 3600;       // matches GlassCityVisualizer.cpp (a multiple of kStreet)
static const float kRingSpeed = 15.0;
static const float kFar = 150.0;

int Mod(int a, int m) { return (int)(uint(a + m * 65536) % uint(m)); }   // a >= -65536 m
int2 HashCell(int2 c) { return int2(c.x, Mod(c.y, kPeriod)); }
float FogT(float t) { return exp(-t * 0.032); }

// Beat rings spreading from a point ahead on the avenue, and the drop's shockwave.
float Waves(float3 p, float sharp) {
    float s = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        float d = length(p.xz - float2(laneX, ringZ[i])) - ringAge[i] * kRingSpeed;
        s += ringHit[i] * exp(-d * d * sharp) * exp(-ringAge[i] * 1.1);
    }
    return s * exp(-p.y * 0.18);
}
float Shock(float3 p) {
    if (shockR <= 0.0) return 0.0;
    float d = length(p.xz - shockPos) - shockR;
    return exp(-d * d * 0.05) * exp(-shockR * 0.01) * smoothstep(2.0, 12.0, shockR);   // eases in: no white-out
}

// Environment for reflections and haze: dark, with a horizon line and a glow down the flight path.
float3 Env(float3 d) {
    float3 c = fogCol * (0.12 + 0.5 * exp(-abs(d.y) * 5.0));
    c += fogCol * (0.9 + 1.6 * buildUp + 1.0 * over) * pow(saturate(d.z), 24.0) * exp(-abs(d.y) * 3.0);
    c += edgeCol * 0.06 * exp(-abs(d.y) * 40.0);
    return c;
}

// ---------------------------------------------------------------------------------------------
struct Box { float3 lo, hi; int3 id; };

bool FloorTower(int2 c, out Box b) {
    int2 hc = HashCell(c);
    float3 h = Hash33(int3(hc, 11));
    int mx = Mod(c.x, kAvenue), mz = Mod(c.y, kStreet);
    float inset = 0.07 + 0.15 * h.y;
    float height = 0.35 + 2.6 * h.x * h.x;
    if (h.y > 0.86) height += 2.0 + 3.6 * h.x;   // the odd skyscraper
    // After a drop the city sinks and rezzes back up behind the shockwave, overshooting a little.
    if (shockR > 0.0) {
        float x = shockR - length(float2(c) + 0.5 - shockPos);
        float rise = x < 0.0 ? 0.1 : lerp(0.1, 1.0, smoothstep(0.0, 3.0, x)) + 0.3 * sin(min(x, 15.0) * 0.8) * exp(-x * 0.3);
        height *= rise;
    }
    height = min(height, kCeil - 0.4);
    b.lo = float3(c.x + inset, 0.0, c.y + inset);
    b.hi = float3(c.x + 1.0 - inset, height, c.y + 1.0 - inset);
    b.id = int3(hc, 0);
    return mx > 1 && mz != 0 && h.z > 0.1;
}

bool CeilBlock(int2 c, out Box b) {
    int2 hc = HashCell(c);
    float3 h = Hash33(int3(hc, 23));
    int mx = Mod(c.x, kAvenue), mz = Mod(c.y, kStreet);
    float len = (0.3 + 2.6 * h.x * h.x) * ceiling;
    if (mx <= 1 || mz == 0) len *= 0.35;   // keep the avenues clear for flying
    float inset = 0.06 + 0.16 * h.y;
    b.lo = float3(c.x + inset, kCeil - len, c.y + inset);
    b.hi = float3(c.x + 1.0 - inset, kCeil, c.y + 1.0 - inset);
    b.id = int3(hc, 1);
    return ceiling > 0.01 && h.z < 0.5;
}

// Floating glass plates, several cells wide (each cell tests the whole plate; the caller keeps only the
// hits inside this cell, so a plate is shaded once).
bool Plate(int2 c, out Box b) {
    int2 g = int2(floor(float2(c) / float2(4.0, 3.0)));
    int2 hg = int2(g.x, Mod(g.y, kPeriod / 3));
    float3 h = Hash33(int3(hg, 37));
    float3 h2 = Hash33(int3(hg, 41));
    float2 org = float2(g) * float2(4.0, 3.0);
    float2 lo = org + floor(h2.xy * float2(2.0, 1.5)) + 0.04;
    float2 hi = lo + float2(1.0 + floor(h2.z * 3.0), 1.0 + floor(h.y * 2.0)) - 0.08;
    float y = 1.4 + 6.2 * h.x;
    float th = 0.03 + 0.04 * h.y;
    b.lo = float3(lo.x, y, lo.y);
    b.hi = float3(hi.x, y + th, hi.y);
    b.id = int3(hg, 2);
    return h.z < 0.22 && c.x >= floor(lo.x) && c.x < hi.x && c.y >= floor(lo.y) && c.y < hi.y;
}

bool BoxSpan(float3 ro, float3 inv, float3 lo, float3 hi, out float tn, out float tf, out float3 nn, out float3 nf) {
    float3 t0 = (lo - ro) * inv, t1 = (hi - ro) * inv;
    float3 a = min(t0, t1), b = max(t0, t1);
    tn = max(max(a.x, a.y), a.z);
    tf = min(min(b.x, b.y), b.z);
    float3 s = sign(inv);
    nn = (tn == a.x) ? float3(-s.x, 0, 0) : (tn == a.y) ? float3(0, -s.y, 0) : float3(0, 0, -s.z);
    nf = (tf == b.x) ? float3(s.x, 0, 0) : (tf == b.y) ? float3(0, s.y, 0) : float3(0, 0, s.z);
    return tf >= max(tn, 0.0);
}

// ---------------------------------------------------------------------------------------------
// One face of a glass box: rgb = light it adds, a = how much it hides what's behind it.
float4 ShadeFace(float3 p, float3 n, Box b, float t, float3 rd, bool back, float pix) {
    float2 uv, size;
    int face;
    if (abs(n.y) > 0.5) { uv = p.xz - b.lo.xz; size = b.hi.xz - b.lo.xz; face = n.y > 0 ? 0 : 1; }
    else if (abs(n.x) > 0.5) { uv = float2(p.z - b.lo.z, p.y - b.lo.y); size = float2(b.hi.z - b.lo.z, b.hi.y - b.lo.y); face = n.x > 0 ? 2 : 3; }
    else { uv = float2(p.x - b.lo.x, p.y - b.lo.y); size = float2(b.hi.x - b.lo.x, b.hi.y - b.lo.y); face = n.z > 0 ? 4 : 5; }
    bool roof = face < 2;

    float px = pix * t;   // world size of a pixel here
    float w = max(0.005, px);
    float2 e2 = min(uv, size - uv);
    float e = min(e2.x, e2.y);
    // Lines are ~1.2 cm thick: once a pixel is wider than that they get fainter rather than wider, so the
    // far city doesn't smear into a wash of light.
    float thin = min(1.0, 0.012 / px);
    float edge = (exp(-e / w) + 0.12 * exp(-e / max(0.04, w * 3.0))) * thin;

    float3 hb = Hash33(b.id + int3(0, 0, 100));
    float spec = Spectrum(4 + (int)(hb.x * 44.0));   // each tower listens to its own band
    float wave = Waves(p, 0.7);
    float shock = Shock(p);

    // Glass tiles: a grid of panes, some lit, flickering with the band and refreshing on snares.
    float3 tiles = 0;
    if (b.id.z < 2 && hb.z > 0.5) {
        const float2 ts = float2(0.125, 0.095);
        float2 g = uv / ts;
        int2 id = int2(floor(g));
        float2 f = frac(g);
        float3 h0 = Hash33(int3(id.x + face * 1013 + b.id.x * 7919, id.y + b.id.y * 1031, 7 + b.id.z));
        int refresh = h0.y < 0.3 ? (int)tileSeed : 0;
        float3 h1 = Hash33(int3(id.x * 3 + refresh * 17, id.y + face * 31 + refresh * 7, b.id.x * 17 + b.id.y * 131 + b.id.z));
        float lit = step(h1.x, lights * (0.12 + 0.7 * spec) * (0.4 + 0.8 * hb.y));
        lit *= 1.0 - buildUp * buildUp * 0.8 * step(0.5, frac(h0.x * 13.0 + floor(time * (7.0 + 9.0 * h0.z)) * 0.37));   // build-ups stutter
        float2 fm = min(f, 1.0 - f);
        float shape = smoothstep(0.02, 0.14, min(fm.x, fm.y));
        shape = lerp(0.7, shape, saturate((min(ts.x, ts.y) / px - 1.5) / 3.0));   // fade to the average when tiny
        float3 tc = h1.y > 0.9 ? sparkCol * 1.8 : tileCol;
        tiles = tc * lit * shape * (0.15 + 0.6 * h1.z) * (0.4 + 1.0 * spec) * (1.0 + 3.0 * wave + 0.3 * over);
    }

    // Currents: traces across the face with pulses of light running along them.
    float along = roof ? uv.x : uv.y;
    float across = roof ? uv.y : uv.x;
    float acrossSize = roof ? size.y : size.x;
    float rate = 0.5 + 1.8 * current;
    float cur = 0.0;
    [unroll] for (int k = 0; k < 3; ++k) {
        float3 hk = Hash33(int3(b.id.x * 5 + k, b.id.y * 3 + face, 50 + b.id.z));
        float d = abs(across - (0.15 + 0.7 * hk.x) * acrossSize);
        float trace = exp(-d / w) * step(hk.z, 0.75) * thin;
        float ph = frac(along * (0.25 + 0.2 * hk.y) - time * rate * (0.6 + 0.8 * hk.y) + hk.x * 7.0);
        cur += trace * (0.05 + current * (pow(ph, 10.0) + 2.0 * pow(ph, 60.0)));
    }
    // The edges carry current too, climbing the towers.
    float climb = pow(frac(p.y * 0.22 - time * (0.35 + 0.9 * current) + hb.y * 5.0), 14.0);

    // Hi-hats: glints along the edges.
    float3 hg = Hash33(int3(floor(uv * 9.0) + b.id.xy * 13, (int)(time * 18.0) + face));
    float glint = step(hg.x, sparkle * 0.12) * edge;

    float edgeAmt = edge * (0.3 + 0.5 * spec + 2.5 * wave + 1.0 * over) + edge * climb * current * 2.5;
    float3 emit = edgeCol * (edgeAmt + cur * 1.5) + tiles + glint * (sparkCol + 0.5) * 2.0;
    emit += (edge * 4.0 + 0.5) * shock * float3(1.0, 0.95, 0.9);

    if (back) return float4(emit * 0.4, 0.03);
    float fres = 0.04 + 0.96 * pow(1.0 - saturate(abs(dot(n, rd))), 5.0);
    float3 refl = Env(reflect(rd, n)) * (0.1 + 1.2 * fres);
    // A broad sheen across the glass, catching the vanishing glow.
    float sheen = pow(saturate(dot(reflect(rd, n), float3(0.0, 0.35, 0.94))), 24.0) * 0.4;
    return float4(emit + refl + glassCol * (0.012 + sheen * 0.3) + fogCol * sheen, 0.07 + 0.55 * fres);
}

// Walk the cells front to back, adding up every glass face the ray passes through. T is how much light
// still gets through; tEnd and kind say where the ray ends: 0 sky, 1 floor, 2 ceiling.
float3 TraceCity(float3 ro, float3 rd, int maxSteps, int maxLayers, float pix, inout float3 T, out float tEnd, out int kind) {
    float3 col = 0;
    float3 inv = 1.0 / (abs(rd) < 1e-6 ? (rd >= 0.0 ? 1e-6 : -1e-6) : rd);
    float tFloor = rd.y < 0.0 ? -ro.y / rd.y : 1e9;
    float tCeil = rd.y > 0.0 && ro.y < kCeil ? (kCeil - ro.y) / rd.y : 1e9;   // (from above it, there's no ceiling to hit)
    tEnd = min(min(tFloor, tCeil), kFar);
    kind = tEnd >= kFar ? 0 : tFloor < tCeil ? 1 : (ceiling > 0.02 ? 2 : 0);
    float3 tint = lerp(1.0, glassCol, 0.2);

    int2 cell = int2(floor(ro.xz));
    int2 stp = int2(rd.x >= 0.0 ? 1 : -1, rd.z >= 0.0 ? 1 : -1);
    float2 tDelta = abs(inv.xz);
    float2 tNext = (float2(cell) + (rd.xz >= 0.0 ? 1.0 : 0.0) - ro.xz) * inv.xz;
    int layers = 0;
    [loop] for (int i = 0; i < maxSteps; ++i) {
        [unroll] for (int k = 0; k < 3; ++k) {
            Box b;
            bool ok;   // (not ?: , which evaluates every branch in HLSL, and each one writes b)
            if (k == 0) ok = FloorTower(cell, b);
            else if (k == 1) ok = CeilBlock(cell, b);
            else ok = Plate(cell, b);
            float tn, tf;
            float3 nn, nf;
            if (!ok || !BoxSpan(ro, inv, b.lo, b.hi, tn, tf, nn, nf) || tn > tEnd) continue;
            if (tn > 0.0) {
                float3 p = ro + rd * tn;
                if (k < 2 || all(int2(floor(p.xz)) == cell)) {
                    float4 s = ShadeFace(p, nn, b, tn, rd, false, pix);
                    col += T * s.rgb * FogT(tn);
                    T *= (1.0 - s.a) * tint;
                    ++layers;
                }
            }
            if (tf < tEnd) {
                float3 p = ro + rd * tf;
                if (k < 2 || all(int2(floor(p.xz)) == cell)) {
                    float4 s = ShadeFace(p, nf, b, tf, rd, true, pix);
                    col += T * s.rgb * FogT(tf);
                    T *= 1.0 - s.a;
                }
            }
        }
        if (layers >= maxLayers || max(T.r, max(T.g, T.b)) < 0.03) break;
        if (min(tNext.x, tNext.y) > tEnd) break;
        if (tNext.x < tNext.y) { tNext.x += tDelta.x; cell.x += stp.x; }
        else { tNext.y += tDelta.y; cell.y += stp.y; }
    }
    return col;
}

// ---------------------------------------------------------------------------------------------
// The floor: black glass with a faint grid, data lanes down the avenues, pulsing traces and beat rings.
float3 ShadeFloor(float3 p, float t, float pix) {
    float w = max(0.004, pix * t);
    float fade = exp(-t * 0.035);
    float2 gq = abs(frac(p.xz + 0.5) - 0.5);
    float grid = exp(-min(gq.x, gq.y) / w) * 0.08 * fade;

    // Traces along the grid lines (every other line), with pulses.
    int ix = (int)floor(p.x + 0.5);
    float3 hx = Hash33(int3(ix, 3, 19));
    float trace = exp(-gq.x / w) * step(hx.x, 0.4);
    float tp = pow(frac(p.z * 0.12 - time * (0.6 + 1.2 * current) * (0.5 + hx.y) + hx.z * 9.0), 12.0);

    // The avenue: two lanes each way, with packets of data streaming along them.
    float ax = p.x - floor(p.x / kAvenue) * kAvenue;
    float lanes = 0.0;
    if (ax < 2.0) {
        float d = min(abs(ax - 0.45), abs(ax - 1.55));
        float dir = ax < 1.0 ? 1.0 : -1.0;
        float ph = frac(p.z * 0.07 - dir * time * (0.4 + 0.8 * current) + floor(ax) * 0.37);
        lanes = exp(-d / (w * 1.5)) * (0.12 + current * 2.5 * pow(ph, 18.0));
    }
    float rings = Waves(p, 5.0);
    float3 c = floorCol * 0.012 + edgeCol * (grid + trace * (0.04 + tp * current) * fade + lanes);
    c += floorCol * rings * 1.5 + (0.6 + floorCol) * Shock(p) * 0.8;
    return c;
}

// The ceiling city's underside: panels, some lit.
float3 ShadeCeiling(float3 p, float t, float pix) {
    float w = max(0.004, pix * t);
    float2 g = p.xz * 2.0;
    int2 id = int2(floor(g));
    float2 fm = min(frac(g), 1.0 - frac(g));
    float m = min(fm.x, fm.y);
    float3 h = Hash33(int3(id.x, Mod(id.y, kPeriod * 2), 83));
    float lit = step(h.x, 0.08 + 0.2 * lights) * (0.3 + 0.7 * h.y);
    float shape = lerp(0.7, smoothstep(0.02, 0.1, m), saturate(0.5 / (pix * t) - 1.0));
    return fogCol * 0.05 + edgeCol * exp(-m / (w * 2.0)) * 0.1 * exp(-t * 0.04) + tileCol * lit * shape * 0.35;
}

float3 Sky(float3 rd) {
    float3 c = Env(rd);
    // Distant data columns on the horizon.
    float az = atan2(rd.x, rd.z);
    float k = az * 90.0;
    float3 h = Hash33(int3((int)floor(k) + 4000, 5, 61));
    float column = exp(-abs(frac(k) - 0.5) * 16.0) * step(0.82, h.x) * exp(-max(rd.y, 0.0) * (2.0 + 8.0 * h.y));
    c += lerp(tileCol, edgeCol, h.z) * column * 0.2;
    // Faint points of light.
    float2 sp = float2(az * 110.0, rd.y * 110.0);
    float3 hs = Hash33(int3(floor(sp) + 9000, 71));
    float2 fs = frac(sp) - 0.2 - 0.6 * hs.xy;
    c += step(0.985, hs.z) * exp(-dot(fs, fs) * 300.0) * (0.3 + tileCol * 0.5);
    return c;
}

// The end of a ray that left the glass: floor, ceiling or sky.
float3 Terminal(float3 ro, float3 rd, float t, int kind, float pix) {
    float3 p = ro + rd * t;
    if (kind == 1) return ShadeFloor(p, t, pix);
    if (kind == 2) return lerp(Sky(rd), ShadeCeiling(p, t, pix), ceiling);
    return Sky(rd);
}

// Light streaks racing along the avenues, past the camera: glowing segments parallel to z.
float3 Streaks(float3 ro, float3 rd, float tEnd, float pix) {
    float3 s = 0;
    if (streakAmt < 0.01) return s;
    float denom = max(1.0 - rd.z * rd.z, 1e-4);
    [loop] for (int i = 0; i < 14; ++i) {
        float ph = streakPhase * (0.6 + 0.8 * Hash33(int3(i, 3, 91)).x) + i * 0.618;
        float3 h = Hash33(int3(i, (int)floor(ph), 97));   // a new path every lap
        if (h.z > streakAmt) continue;
        float lane = floor(h.x * 5.0) - 2.0;
        float x = laneX + lane * kAvenue + (frac(h.y * 13.0) < 0.5 ? -0.55 : 0.55);
        float y = h.y < 0.55 ? 0.12 + 1.8 * frac(h.y * 7.3) : 2.5 + 6.0 * frac(h.y * 3.1);
        float head = ro.z + 80.0 - frac(ph) * 115.0;
        float len = streakLen * (0.6 + 0.8 * h.x);
        float3 w0 = ro - float3(x, y, 0.0);
        float d = dot(rd, w0);
        float t = (rd.z * w0.z - d) / denom;
        float sz = (w0.z - rd.z * d) / denom;
        float a = (head - sz) / len;   // 0 at the head, 1 at the tail
        if (t <= 0.0 || t > tEnd + 1.0 || a < 0.0 || a > 1.0) continue;
        float dist = length(w0 + rd * t - float3(0.0, 0.0, sz));
        float wd = 0.01 + pix * t * 0.8;
        float glow = exp(-dist / wd) + 0.12 * exp(-dist / (wd * 6.0));
        s += ((i & 1) ? sparkCol : edgeCol) * glow * pow(1.0 - a, 1.5) * FogT(t) * 2.5;
    }
    return s;
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 ndc = (i.uv * 2.0 - 1.0) * float2(aspect, -1.0);
    float3 rd = normalize(camFwd * focal + camRight * ndc.x + camUp * ndc.y);
    float pix = 2.0 / (iResolution.y * focal);

    float3 T = 1.0;
    float tEnd;
    int kind;
    float3 col = TraceCity(camPos, rd, 160, 5, pix, T, tEnd, kind);
    float3 p = camPos + rd * tEnd;
    float3 surf = Terminal(camPos, rd, tEnd, kind, pix);

    // The floor mirrors the city.
    if (kind == 1 && max(T.r, max(T.g, T.b)) > 0.03) {
        float3 rr = float3(rd.x, -rd.y, rd.z);
        float3 T2 = 1.0;
        float t2;
        int kind2;
        float3 refl = TraceCity(p + rr * 0.001, rr, 80, 3, pix, T2, t2, kind2);
        refl += T2 * lerp(Env(rr), Terminal(p, rr, t2, kind2, pix), FogT(t2));
        float fres = 0.3 + 0.6 * pow(1.0 - abs(rd.y), 5.0);
        surf += refl * fres;
    }
    col += T * lerp(Env(rd), surf, FogT(tEnd));
    col += Streaks(camPos, rd, tEnd, pix);
    // Never let a stray NaN or infinity out: the bloom's mips would spread it into black squares.
    if (any(isnan(col)) || any(isinf(col))) col = 0.0;
    return float4(min(col, 64.0), 1.0);
}
