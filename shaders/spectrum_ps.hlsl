#include "common.hlsli"

// Spectrum: the hi-fi scene. One 64-band analyser shown two ways (see SpectrumVisualizer.h):
//   0 Studio   - glossy 3D bars on a mirror floor, traced exactly cell by cell, with peak caps and sparks
//   1 Radial   - the bars bent into a ring round a glowing core, with echoes of the ring rippling out
// The bars, caps and sparks are simulated on the CPU in "bar space" (x band 0..64, y height, z depth/side).

cbuffer Spectrum : register(b2) {   // matches SpectrumVisualizer::Constants
    float3 camPos;   float focal;
    float3 camRight; float time;
    float3 camUp;    float look;
    float3 camFwd;   float kick;
    float buildUp;   float flash;   float intensity; float sparkCount;
    float rotation;  float3 pad1;
    float3 lo;       float fireworks;
    float3 hi;       float pad0;
    float4 echoAge4[2];
    float4 bars4[16];
    float4 caps4[16];
    float4 echo4[128];     // 8 echoes x 64 bars
    float4 sparks[128];    // bar-space position, brightness
};

static const float kPi = 3.14159265;
static const float kH = 22.0;   // Studio: world height of a full bar

float Bar(int i) { i = clamp(i, 0, 63); return Pick(bars4[i >> 2], i & 3); }
float Cap(int i) { i = clamp(i, 0, 63); return Pick(caps4[i >> 2], i & 3); }
float EchoAge(int e) { return Pick(echoAge4[e >> 2], e & 3); }
float Echo(int e, int i) { i = clamp(i, 0, 63); return Pick(echo4[e * 16 + (i >> 2)], i & 3); }
float BarAt(float x) {   // smooth, x in bands
    float f = clamp(x - 0.5, 0.0, 63.0);
    int i = (int)f;
    return lerp(Bar(i), Bar(i + 1), f - i);
}
float EchoAt(int e, float x) {
    float f = clamp(x - 0.5, 0.0, 63.0);
    int i = (int)f;
    return lerp(Echo(e, i), Echo(e, i + 1), f - i);
}

// The bar colour by height: the palette's low colour rising to its hot colour.
float3 Ramp(float t) {
    t = saturate(t);
    return lerp(lo * 0.9, hi * 1.25, smoothstep(0.0, 1.0, t)) + hi * 0.4 * smoothstep(0.85, 1.0, t);
}

float Glow(float2 d, float r) { return exp(-dot(d, d) / (r * r)); }

// =============================================================================================
// Studio
bool HitBox(float3 ro, float3 inv, float3 bmin, float3 bmax, out float t, out float3 n) {
    float3 t0 = (bmin - ro) * inv, t1 = (bmax - ro) * inv;
    float3 tn = min(t0, t1), tf = max(t0, t1);
    float a = max(max(tn.x, tn.y), tn.z), b = min(min(tf.x, tf.y), tf.z);
    t = a;
    n = (a == tn.x) ? float3(-sign(inv.x), 0, 0) : (a == tn.y) ? float3(0, -sign(inv.y), 0) : float3(0, 0, -sign(inv.z));
    return b >= max(a, 0.0);
}

struct BarHit { float t; int kind; int index; float3 n; float3 bmin, bmax; };   // kind 0 none, 1 bar, 2 cap

void BarBox(int i, out float3 bmin, out float3 bmax) {
    bmin = float3(i - 32 + 0.14, 0.0, -0.36);
    bmax = float3(i - 31 - 0.14, max(Bar(i) * kH, 0.02), 0.36);
}
void CapBox(int i, out float3 bmin, out float3 bmax) {
    float y = Cap(i) * kH;
    bmin = float3(i - 32 + 0.14, y + 0.18, -0.36);
    bmax = float3(i - 31 - 0.14, y + 0.5, 0.36);
}

BarHit TraceBars(float3 ro, float3 rd, float tMax) {
    BarHit h;
    h.t = tMax; h.kind = 0; h.index = 0; h.n = float3(0, 1, 0); h.bmin = 0; h.bmax = 0;
    float3 inv = 1.0 / (abs(rd) < 1e-6 ? 1e-6 * (rd >= 0 ? 1 : -1) : rd);
    float tb; float3 nb;
    // The whole row's bounds (caps can fly well above the bars).
    float3 t0 = (float3(-32, 0, -0.36) - ro) * inv, t1 = (float3(32, kH * 2.6, 0.36) - ro) * inv;
    float3 tn = min(t0, t1), tf = max(t0, t1);
    float tEnter = max(max(max(tn.x, tn.y), tn.z), 0.0), tExit = min(min(min(tf.x, tf.y), tf.z), tMax);
    if (tEnter > tExit) return h;
    float x = ro.x + rd.x * (tEnter + 1e-4);
    int cell = clamp((int)floor(x) + 32, 0, 63);
    int stepDir = rd.x >= 0.0 ? 1 : -1;
    [loop] for (int k = 0; k < 66; ++k) {
        float3 bmin, bmax;
        BarBox(cell, bmin, bmax);
        if (HitBox(ro, inv, bmin, bmax, tb, nb) && tb < h.t && tb > 0.0) { h.t = tb; h.kind = 1; h.index = cell; h.n = nb; h.bmin = bmin; h.bmax = bmax; }
        CapBox(cell, bmin, bmax);
        if (HitBox(ro, inv, bmin, bmax, tb, nb) && tb < h.t && tb > 0.0) { h.t = tb; h.kind = 2; h.index = cell; h.n = nb; h.bmin = bmin; h.bmax = bmax; }
        if (h.kind != 0) break;   // cells don't overlap: the first hit is the nearest
        float edge = (stepDir > 0 ? cell - 31.0 : cell - 32.0);
        float tNext = (edge - ro.x) * inv.x;
        if (tNext > tExit || abs(rd.x) < 1e-5) break;
        cell += stepDir;
        if (cell < 0 || cell > 63) break;
    }
    return h;
}

// The colour of a bar or cap surface.
float3 ShadeBar(BarHit h, float3 p, float3 rd, bool cheap) {
    float3 n = h.n;
    float hf = p.y / kH;
    if (h.kind == 2) return lerp(hi, 1.0, 0.45) * (2.0 + 0.8 * kick) + flash * 0.5;
    // Distance to the face's nearest edge: the glass glows along its edges.
    float3 dl = p - h.bmin, dh = h.bmax - p;
    float3 de = min(dl, dh);
    float ed = abs(n.x) > 0.5 ? min(de.y, de.z) : abs(n.y) > 0.5 ? min(de.x, de.z) : min(de.x, de.y);
    float3 ramp = Ramp(hf);
    float glass = 0.4 + 0.9 * exp(-ed / 0.07);
    float faceLight = abs(n.y) > 0.5 ? 1.7 : abs(n.x) > 0.5 ? 0.55 : 1.0;
    float3 col = ramp * glass * faceLight * (0.85 + 0.35 * kick + 0.35 * buildUp + 0.25 * intensity);
    if (!cheap) {
        float3 r = reflect(rd, n);
        col += pow(saturate(dot(r, normalize(float3(0.25, 1.0, -0.35)))), 30.0) * 0.6;   // a softbox overhead
        col += pow(1.0 - saturate(-dot(rd, n)), 4.0) * 0.25 * hi;                           // lacquer sheen
    }
    return col + flash * 0.3;
}

float3 Studio(float2 sp) {
    float3 ro = camPos;
    float3 rd = normalize(camFwd * focal + camRight * sp.x + camUp * sp.y);
    const float tFar = 400.0;
    float tFloor = rd.y < 0.0 ? -ro.y / rd.y : tFar;
    BarHit h = TraceBars(ro, rd, min(tFloor, tFar));
    float3 room = lerp(float3(0.012, 0.01, 0.025), float3(0.035, 0.025, 0.06), saturate(rd.y * 2.0 + 0.3));
    room += lo * 0.03 * (0.5 + intensity);
    float3 col = room;
    float tHit = tFar;
    if (h.kind != 0) {
        tHit = h.t;
        col = ShadeBar(h, ro + rd * h.t, rd, false);
    } else if (tFloor < tFar) {
        tHit = tFloor;
        float3 p = ro + rd * tFloor;
        col = float3(0.006, 0.006, 0.01);
        // The mirror: the bars reflected, softer and dimmer further from the floor.
        float3 rr = float3(rd.x, -rd.y, rd.z);
        BarHit m = TraceBars(p + rr * 0.01, rr, 200.0);
        if (m.kind != 0) {
            float3 q = p + rr * (m.t + 0.01);
            col += ShadeBar(m, q, rr, true) * 0.42 * exp(-q.y * 0.035);
        }
        // Light spilling onto the floor from the nearby bars.
        int c0 = (int)floor(p.x) + 32;
        float3 spill = 0;
        [loop] for (int j = c0 - 7; j <= c0 + 7; ++j) {
            if (j < 0 || j > 63) continue;
            float b = Bar(j);
            float2 d = float2(p.x - (j - 31.5), p.z);
            spill += Ramp(b * 0.6) * b / (1.0 + dot(d, d) * 0.35);
        }
        col += spill * (0.22 + 0.12 * kick);
    }
    // The glow behind and above the bars, where the ray passes the row.
    if (abs(rd.z) > 1e-4) {
        float tz = -ro.z / rd.z;
        if (tz > 0.0 && tz < tHit) {
            float3 q = ro + rd * tz;
            if (abs(q.x) < 33.0 && q.y > 0.0) {
                float b = BarAt(q.x + 32.0);
                float above = q.y - b * kH;
                col += Ramp(b) * exp(-max(above, 0.0) * 0.11) * (0.2 + 0.2 * intensity + 0.2 * kick) * smoothstep(33.0, 30.0, abs(q.x));
            }
        }
    }
    col = lerp(col, room, 1.0 - exp(-tHit * 0.004));
    // Sparks.
    int n = (int)sparkCount;
    [loop] for (int i = 0; i < n; ++i) {
        float4 s = sparks[i];
        float3 v = float3(s.x - 32.0, s.y * kH, s.z * 1.5) - ro;
        float dz = dot(v, camFwd);
        if (dz < 0.5) continue;
        float2 ps = float2(dot(v, camRight), dot(v, camUp)) / dz * focal;
        float r = max(0.005, 0.45 / dz);
        col += lerp(hi, 1.0, 0.5) * Glow(sp - ps, r) * s.w * 2.0;
    }
    return col;
}

// =============================================================================================
// Radial
float3 RadialLook(float2 p) {
    float sway = 0.22 * sin(rotation);
    float r = length(p);
    float ang = atan2(p.x, -p.y) + sway;   // 0 straight down
    ang = ang > kPi ? ang - 2.0 * kPi : ang < -kPi ? ang + 2.0 * kPi : ang;
    float t = abs(ang) / kPi;               // 0 bottom (bass) .. 1 top (treble), mirrored
    float R0 = 0.23 * (1.0 + 0.07 * kick);
    float inner = R0 + 0.035;
    float L = 0.52;

    float3 col = float3(0.008, 0.008, 0.02);
    col += lo * 0.12 * exp(-r * 2.2) * (0.5 + intensity + 0.5 * kick);
    col *= 1.0 - 0.35 * saturate(r - 0.6);

    // The spokes.
    float bx = t * 64.0;
    int b = min((int)bx, 63);
    float h = Bar(b);
    float dAng = (frac(bx) - 0.5) / 64.0 * kPi;     // angle from the spoke's centre line
    float across = abs(r * sin(dAng));
    float halfW = r * sin(0.34 / 64.0 * kPi);
    float along = r - inner;
    float len = max(h * L, 0.004);
    float inBar = smoothstep(halfW + 0.0015, halfW - 0.0015, across) * smoothstep(-0.002, 0.002, along) * smoothstep(len + 0.002, len - 0.002, along);
    float3 barCol = Ramp(along / L) * (1.0 + 0.4 * kick + 0.35 * buildUp);
    col = lerp(col, barCol, inBar);
    // A soft glow round the bars.
    float dOut = max(along - len, 0.0) + max(-along, 0.0);
    col += Ramp(h) * exp(-(max(across - halfW, 0.0) + dOut) * 55.0) * 0.25 * (1.0 - inBar);
    // Peak dots.
    float capR = inner + Cap(b) * L + 0.014;
    float2 capP = float2(sin(dAng) * r, cos(dAng) * r) - float2(0.0, capR);
    col += lerp(hi, 1.0, 0.5) * smoothstep(0.0075, 0.004, length(capP)) * 1.5;

    // Echoes: the shape of the ring on each kick, rippling outward.
    [unroll] for (int e = 0; e < 8; ++e) {
        float age = EchoAge(e);
        if (age < 0.0 || age > 2.4) continue;
        float re = (inner + EchoAt(e, bx) * L) * (1.0 + age * 0.85);
        float w = 0.003 + 0.004 * age;
        float a = exp(-age * 1.5) * (0.5 + 0.5 * fireworks);
        col += Ramp(0.4 + 0.4 * EchoAt(e, bx)) * a * exp(-abs(r - re) / w) * 0.55;
    }

    // The core: a glowing disc in the palette, pulsing with the bass, with rings drifting out.
    if (r < R0) {
        float k = r / R0;
        float3 core = lerp(hi * 1.3, lo * 0.35, k * k);
        core *= 0.55 + 0.25 * sin(r * 80.0 - time * 3.0) * (1.0 - k) + 0.5 * kick;
        col = core;
    }
    col += hi * exp(-abs(r - R0) * 140.0) * (0.9 + 0.8 * kick);

    // Sparks: bar space wrapped onto the ring (z picks the side).
    int n = (int)sparkCount;
    [loop] for (int i = 0; i < n; ++i) {
        float4 s = sparks[i];
        float a2 = (s.z >= 0.0 ? 1.0 : -1.0) * (s.x / 64.0) * kPi - sway;
        float rr = inner + s.y * L;
        float2 sp2 = float2(sin(a2), -cos(a2)) * rr;
        col += lerp(hi, 1.0, 0.5) * Glow(p - sp2, 0.007) * s.w * 2.0;
    }
    return col + hi * flash * 0.12;
}

// =============================================================================================
float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((i.uv.x * 2.0 - 1.0) * aspect, 1.0 - i.uv.y * 2.0);
    int lk = (int)look;
    float3 col = lk == 0 ? Studio(sp) : RadialLook(sp);
    col = col / (1.0 + col * 0.18);   // gentle tone map
    col += (InterleavedGradientNoise(i.pos.xy) - 0.5) / 255.0;
    return float4(col, 1.0);
}
