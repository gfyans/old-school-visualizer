#include "common.hlsli"

// Detonation: a vast procedural city at night around a central spire. Kicks send rings of light out from
// the spire across the rooftops; build-ups charge an orb on the spire and spiral the clouds; a drop erupts a
// white dome that tears at the buildings on its rim and punches a ring through the clouds, then inverts to
// a black sphere with a burning rim, and finally collapses, leaving a crater the city slowly rebuilds.
//
// The city is a grid of box buildings (1 unit cells) traced exactly with a 2D DDA, so it's crisp and
// cheap. Everything else is analytic: the dome is a sphere, the clouds a noise layer on a plane.
// Looks: 0 Night (neon and haze), 1 Ink (manga linework and screentone on paper), 2 Dusk (hazy, painted).

cbuffer Detonation : register(b2) {   // matches DetonationConstants in DetonationVisualizer.cpp
    float3 camPos;   float focal;
    float3 camRight; float time;
    float3 camUp;    float look;
    float3 camFwd;   float charge;      // build-up energy 0..1
    float domeR;     float domeWhite;   float domeOn;    float flash;      // domeWhite: 1 white-hot .. 0 black void
    float ruinR;     float regrow;      float swirl;     float cloudRing;  // crater radius; 0..1 rebuilt; cloud spin phase
    float4 ringAge;                     // seconds since the last four kicks (large = none)
    float4 ringHit;                     // their strengths
    float shockR;    float lights;      float flicker;   float spire;      // lights: share of windows lit; spire 0..1 standing
    float3 glowCol;  float intensity;   // window / city light colour
};

static const float kTop = 16.0;        // tallest building (traversal slab)
static const float kCloud = 28.0;      // cloud deck height
static const float3 kOrb = float3(0, 15.2, 0);
static const float kRingSpeed = 22.0;  // units per second

// ---------------------------------------------------------------------------------------------
float Hash1(int2 c, int salt) { return Hash33(int3(c, salt)).x; }

float Noise2(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    int2 c = int2(i);
    float a = Hash1(c, 41), b = Hash1(c + int2(1, 0), 41), d = Hash1(c + int2(0, 1), 41), e = Hash1(c + int2(1, 1), 41);
    return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
}
float Fbm(float2 p) {
    float s = 0.0, a = 0.5;
    [unroll] for (int i = 0; i < 5; ++i) { s += a * Noise2(p); p = p * 2.03 + 17.1; a *= 0.5; }
    return s;
}

// Beat rings: brightness of the rings passing radius r (from the spire), and the gentle lift they give.
float Rings(float r) {
    float s = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        float age = ringAge[i];
        float d = r - age * kRingSpeed;
        s += ringHit[i] * exp(-d * d * 0.35) * exp(-age * 0.9);
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// The building in one cell.
struct Building { bool exists; float3 lo, hi; float ruin; float tear; };

int2 Mod7(int2 c) { return int2(uint2(c + 70000) % 7u); }   // 70000 is a multiple of 7

bool IsRoad(int2 c) {
    int2 m = Mod7(c);
    return m.x == 3 || m.y == 3;   // avenues every 7 blocks, clear of the spire
}

Building GetBuilding(int2 c) {
    Building b;
    float3 h = Hash33(int3(c, 5));
    float2 centre = float2(c) + 0.5;
    float r = length(centre);
    b.exists = !IsRoad(c) && h.z > 0.06 && r > 2.2;   // a few empty lots; a plaza round the spire
    float inset = 0.1 + 0.12 * h.y;
    float height = 0.35 + 1.6 * h.x * h.x;
    if (h.y > 0.93) height += 2.0 + 3.0 * h.x;                 // the odd tower
    height *= 1.0 + 3.2 * exp(-r * r / 260.0);                  // downtown
    height += Rings(r) * 0.12;                                   // kicks lift the roofs a little as they pass
    // The crater: rubble inside it, rebuilt from the rim inwards.
    b.ruin = r < ruinR * (1.0 - regrow) ? 1.0 : 0.0;
    if (b.ruin > 0.5) { height = 0.06 + 0.18 * h.x; inset += 0.05 * h.z; }
    // Torn at the dome's rim while it's white-hot: shorn short and leaning out.
    b.tear = domeOn * domeWhite * saturate(1.0 - (r - domeR) / 5.0) * step(domeR, r);
    height *= 1.0 - 0.6 * b.tear * (0.5 + 0.5 * h.z);
    b.lo = float3(c.x + inset, 0.0, c.y + inset);
    b.hi = float3(c.x + 1.0 - inset, min(height, kTop), c.y + 1.0 - inset);
    return b;
}

bool HitBox(float3 ro, float3 inv, float3 lo, float3 hi, out float t, out float3 n) {
    float3 t0 = (lo - ro) * inv, t1 = (hi - ro) * inv;
    float3 tn = min(t0, t1), tf = max(t0, t1);
    float a = max(max(tn.x, tn.y), tn.z), b = min(min(tf.x, tf.y), tf.z);
    t = a;
    n = (a == tn.x) ? float3(-sign(inv.x), 0, 0) : (a == tn.y) ? float3(0, -sign(inv.y), 0) : float3(0, 0, -sign(inv.z));
    return b >= max(a, 0.0);
}

struct Hit {
    float t; int kind;       // 0 sky, 1 building, 2 ground, 3 spire, 4 orb, 5 dome
    float3 n; int2 cell;
    float3 lo, hi; float ruin; float tear;
};

Hit TraceCity(float3 ro, float3 rd, float tFar) {
    Hit h;
    h.t = tFar; h.kind = 0; h.n = float3(0, 1, 0); h.cell = int2(0, 0); h.lo = 0; h.hi = 0; h.ruin = 0; h.tear = 0;
    float3 inv = 1.0 / (abs(rd) < 1e-6 ? 1e-6 * (rd >= 0 ? 1 : -1) : rd);
    // The ground.
    float tg = rd.y < 0.0 ? -ro.y / rd.y : 1e9;
    if (tg < h.t) { h.t = tg; h.kind = 2; }
    // Enter the slab of buildings.
    float t0 = 0.0;
    bool above = ro.y > kTop && rd.y >= 0.0;   // looking up from above every roof: no buildings
    if (ro.y > kTop && !above) t0 = (kTop - ro.y) / rd.y;
    float3 p = ro + rd * t0;
    int2 cell = int2(floor(p.xz));
    int2 stp = int2(rd.x >= 0 ? 1 : -1, rd.z >= 0 ? 1 : -1);
    float2 tDelta = abs(inv.xz);
    float2 tNext = (float2(cell) + (rd.xz >= 0 ? 1.0 : 0.0) - p.xz) * inv.xz + t0;
    float tSlabExit = rd.y > 0.0 ? (kTop - ro.y) / rd.y : tg;
    [loop] for (int i = 0; i < (above ? 0 : 220); ++i) {
        Building b = GetBuilding(cell);
        if (b.exists) {
            float t; float3 n;
            if (HitBox(ro, inv, b.lo, b.hi, t, n) && t < h.t) {
                h.t = t; h.kind = 1; h.n = n; h.cell = cell; h.lo = b.lo; h.hi = b.hi; h.ruin = b.ruin; h.tear = b.tear;
                break;   // cells are visited front to back
            }
        }
        float tExit = min(tNext.x, tNext.y);
        if (tExit > min(h.t, tSlabExit)) break;
        if (tNext.x < tNext.y) { tNext.x += tDelta.x; cell.x += stp.x; }
        else { tNext.y += tDelta.y; cell.y += stp.y; }
    }
    // The spire and its orb.
    if (spire > 0.01) {
        float t; float3 n;
        float top = 14.0 * spire;
        if (HitBox(ro, inv, float3(-0.32, 0, -0.32), float3(0.32, top, 0.32), t, n) && t < h.t) {
            h.t = t; h.kind = 3; h.n = n; h.lo = float3(-0.32, 0, -0.32); h.hi = float3(0.32, top, 0.32);
        }
        float orbR = 0.35 + 0.9 * charge;
        float3 oc = ro - kOrb * float3(1, spire, 1);
        float bq = dot(oc, rd), cq = dot(oc, oc) - orbR * orbR, dq = bq * bq - cq;
        if (dq > 0.0) {
            float to = -bq - sqrt(dq);
            if (to > 0.0 && to < h.t) { h.t = to; h.kind = 4; h.n = normalize(oc + rd * to); }
        }
    }
    return h;
}

// Distance from a face point to the nearest edge of that face (for ink lines), plus face coordinates.
float EdgeDistance(float3 p, float3 n, float3 lo, float3 hi, out float2 uv, out float2 size) {
    if (abs(n.y) > 0.5) { uv = p.xz - lo.xz; size = hi.xz - lo.xz; }
    else if (abs(n.x) > 0.5) { uv = float2(p.z - lo.z, p.y); size = float2(hi.z - lo.z, hi.y); }
    else { uv = float2(p.x - lo.x, p.y); size = float2(hi.x - lo.x, hi.y); }
    float2 e = min(uv, size - uv);
    return min(e.x, e.y);
}

// Windows on a wall: x lit amount, y window shape mask.
float2 Windows(float2 uv, float2 size, int2 cell, float3 n) {
    const float2 grid = float2(0.11, 0.15);
    float2 g = uv / grid;
    int2 w = int2(floor(g));
    float2 f = frac(g);
    float shape = step(0.22, f.x) * step(f.x, 0.78) * step(0.28, f.y) * step(f.y, 0.78);
    shape *= step(0.08, uv.y) * step(uv.y, size.y - 0.06) * step(0.05, uv.x) * step(uv.x, size.x - 0.05);
    float3 hw = Hash33(int3(w.x + cell.x * 37 + (int)(n.x * 3.0 + n.z * 7.0) * 911, w.y + cell.y * 53, 9));
    float lit = step(hw.x, lights);
    // Build-ups make them stutter.
    lit *= 1.0 - flicker * step(0.5, frac(hw.y * 13.0 + floor(time * (8.0 + 10.0 * hw.z)) * 0.37));
    return float2(lit * (0.6 + 0.4 * hw.z), shape);
}

// ---------------------------------------------------------------------------------------------
// The dome: a sphere on the ground at the spire.
float HitDome(float3 ro, float3 rd, out float3 n) {
    n = float3(0, 1, 0);
    if (domeOn < 0.01 || domeR < 0.05) return 1e9;
    float b = dot(ro, rd), c = dot(ro, ro) - domeR * domeR, d = b * b - c;
    if (d < 0.0) return 1e9;
    float t = -b - sqrt(d);
    if (t < 0.0) return 1e9;   // inside it: it's the whole world (handled by the caller's tint)
    n = normalize(ro + rd * t);
    return t;
}

// The dome's glow around the edge: how close the ray passes to its surface.
float DomeHalo(float3 ro, float3 rd, float tMax) {
    if (domeOn < 0.01) return 0.0;
    float tc = clamp(-dot(ro, rd), 0.0, tMax);
    float d = length(ro + rd * tc) - domeR;
    return exp(-max(d, 0.0) * 0.12) * saturate(domeR * 0.2);
}

// Light the blast throws on a surface: from the dome (white-hot) or its burning rim on the ground (void).
float3 BlastLight(float3 p, float3 n, float3 hot) {
    float r = length(p.xz);
    float3 toC = normalize(float3(-p.x, domeR * 0.35 - p.y, -p.z));
    float ds = max(length(p) - domeR, 0.3);
    float white = domeOn * domeWhite * 2.0 / (1.0 + 0.015 * ds * ds) * (0.25 + 0.75 * saturate(dot(n, toC)));
    float rimBand = r - domeR * 1.04;
    float rim = domeOn * (1.0 - domeWhite) * 2.2 * exp(-rimBand * rimBand * 0.08) * (0.4 + 0.6 * saturate(n.y + dot(n, toC)));
    return float3(1.0, 0.95, 0.88) * white + hot * rim;
}

// ---------------------------------------------------------------------------------------------
// Clouds: a noise deck spiralling round the spire, faster as energy builds, blown open by the blast.
float4 Clouds(float3 ro, float3 rd, float3 hot, float3 cool) {
    if (rd.y <= 0.0) return 0;
    float t = (kCloud - ro.y) / rd.y;
    float3 p = ro + rd * t;
    float r = length(p.xz);
    float ang = swirl * 14.0 / (r + 14.0);
    float c = cos(ang), s = sin(ang);
    float2 q = float2(c * p.x - s * p.z, s * p.x + c * p.z);
    float dens = Fbm(q * 0.035 + float2(time * 0.01, 0.0));
    dens = saturate((dens - 0.42 + 0.12 * charge) * 2.4);
    // A hole round the spire as it charges, and the blast's ring pushing the deck outward.
    dens *= smoothstep(1.5, 6.0 + 4.0 * charge, r);
    float ringD = r - cloudRing;
    dens *= cloudRing > 0.5 ? smoothstep(-2.0, 2.0, ringD) : 1.0;
    float ring = cloudRing > 0.5 ? exp(-ringD * ringD * 0.02) * saturate(1.0 - cloudRing / 220.0) : 0.0;
    dens = saturate(dens + ring * 0.8);
    // Lit from below: the city's glow, the orb, the dome, and lightning flickers while charging.
    float3 lightCol = glowCol * (0.12 + 0.25 * intensity) * exp(-r * 0.004);
    lightCol += cool * charge * 2.0 * exp(-r * 0.05);
    lightCol += float3(1.0, 0.95, 0.88) * domeOn * domeWhite * 4.0 * exp(-max(r - domeR, 0.0) * 0.03);
    lightCol += hot * domeOn * (1.0 - domeWhite) * exp(-abs(r - domeR) * 0.08);
    float bolt = step(0.93, Hash1(int2(floor(time * 9.0), 3), 77)) * charge;
    lightCol += cool * bolt * 2.0 * exp(-r * 0.02);
    lightCol += hot * ring * 2.5;
    float3 col = lightCol * (0.4 + 0.6 * dens) + flash * 0.8;
    float fade = exp(-t * 0.006);
    return float4(col, dens * fade);
}

// ---------------------------------------------------------------------------------------------
struct LookPalette { float3 skyTop, skyHorizon, fog, hot, cool, ground; };

LookPalette GetLook(int lk) {
    LookPalette L;
    if (lk == 2) {   // Dusk: hazy, painted
        L.skyTop = float3(0.16, 0.12, 0.22); L.skyHorizon = float3(0.95, 0.48, 0.22); L.fog = float3(0.7, 0.42, 0.28);
        L.hot = float3(1.0, 0.55, 0.2); L.cool = float3(0.6, 0.8, 1.0); L.ground = float3(0.09, 0.06, 0.06);
    } else {         // Night (Ink works from Night's lighting)
        L.skyTop = float3(0.012, 0.012, 0.04); L.skyHorizon = float3(0.3, 0.08, 0.13); L.fog = float3(0.17, 0.06, 0.12);
        L.hot = float3(1.0, 0.25, 0.08); L.cool = float3(0.35, 0.75, 1.0); L.ground = float3(0.015, 0.015, 0.025);
    }
    return L;
}

float3 Sky(float3 rd, LookPalette L) {
    float h = saturate(rd.y);
    float3 col = lerp(L.skyHorizon, L.skyTop, pow(h, 0.45));
    float3 hs = Hash33(int3(floor(rd.xy * 300.0 + rd.z * 97.0), 13));   // faint stars
    col += step(0.997, hs.x) * 0.3 * h * float3(0.8, 0.9, 1.0);
    return col;
}

// Inks a picture: paper, black lines and screentone dots (darkness in, ink 0..1 out).
float Screentone(float tone, float2 px) {
    const float cell = 5.0;
    float2 g = px / cell;
    g.x += 0.5 * (floor(g.y) % 2.0);   // staggered rows, like printed tone
    float2 f = frac(g) - 0.5;
    float radius = sqrt(saturate(tone)) * 0.62;
    return step(length(f), radius);
}

float4 main(VSOut i) : SV_Target {
    int lk = (int)look;
    LookPalette L = GetLook(lk);
    float2 px = i.uv * iResolution;
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((i.uv.x * 2.0 - 1.0) * aspect, 1.0 - i.uv.y * 2.0);
    float3 ro = camPos;
    float3 rd = normalize(camFwd * focal + camRight * sp.x + camUp * sp.y);
    const float tFar = 260.0;

    Hit h = TraceCity(ro, rd, tFar);
    float3 dn;
    float td = HitDome(ro, rd, dn);
    bool dome = td < h.t;
    if (dome) { h.t = td; h.kind = 5; h.n = dn; }
    float3 p = ro + rd * h.t;
    float r = length(p.xz);
    float pixelSize = 2.0 / (iResolution.y * focal);   // world size of a pixel at distance 1

    float3 col = 0;
    float edge = 9.0;       // ink: distance to a drawn line, in pixels
    float inkFill = -1.0;   // ink: forced paper (0) or ink (1), else from the tone
    float inkTone = -1.0;   // ink: the surface's own screentone darkness (else from the light)
    float footprint = h.t * pixelSize;   // world size of this pixel
    if (h.kind == 0) {
        col = Sky(rd, L);
    } else if (h.kind == 5) {
        // The dome: white-hot with bands rolling up it, then a black void with a burning rim.
        float bands = 0.75 + 0.25 * sin(p.y * 1.4 - time * 5.0 + 3.0 * Fbm(float2(atan2(p.z, p.x) * 6.0, p.y * 0.3)));
        float facing = saturate(-dot(h.n, rd));
        float3 white = lerp(L.hot * 2.0, float3(3.0, 2.9, 2.7), pow(facing, 0.6)) * bands;
        float rimGlow = pow(1.0 - facing, 3.0);
        float3 voidCol = L.hot * rimGlow * 3.0 + L.hot * 0.02;
        col = lerp(voidCol, white, domeWhite);
        inkFill = domeWhite > 0.5 ? 0.0 : (rimGlow > 0.55 ? 0.0 : 1.0);
        edge = abs(facing - 0.08) / max(fwidth(facing), 1e-4);
    } else {
        // City surfaces.
        float3 n = h.n;
        float3 albedo = 0.08;
        float3 emit = 0;
        if (h.kind == 1 || h.kind == 3) {
            float2 uv, size;
            float ed = EdgeDistance(p, n, h.lo, h.hi, uv, size);
            // Lines fade out where buildings get too small to draw them (they'd turn to noise).
            edge = ed / footprint + 9.0 * saturate(footprint / 0.06 - 1.0);
            float3 hc = Hash33(int3(h.cell, 17));
            albedo = (0.08 + 0.08 * hc.x) * float3(0.9, 0.95, 1.1);
            emit += L.skyHorizon * 0.25 * exp(-ed / max(0.015, footprint * 1.5)) * (1.0 - h.ruin);   // corners catch the sky
            float sunSide = dot(n, normalize(float3(-0.4, 0.7, 0.3)));
            inkTone = n.y > 0.5 ? 0.04 : sunSide > 0.0 ? 0.14 : 0.42;
            if (abs(n.y) < 0.5 && h.ruin < 0.5) {
                float2 w = Windows(uv, size, h.cell, n);
                // Far away, windows blend to their average so they don't shimmer.
                float lod = saturate(footprint / 0.035 - 0.5);
                float glow = lerp(w.x * w.y, lights * 0.22, lod);
                emit += glowCol * glow * (1.4 + 0.8 * intensity) * (1.0 - h.tear);
                albedo *= 1.0 - 0.5 * w.y * (1.0 - lod);
                float near = saturate(1.5 - footprint / 0.012);
                if (w.y > 0.5 && w.x < 0.1) inkTone = lerp(inkTone, 0.85, near);   // ink: unlit windows filled in
                inkTone = lerp(inkTone, inkTone + 0.12, lod);
            }
            if (n.y > 0.5) {   // roofs catch the kick rings and a little sky; a few blink for aircraft
                emit += L.cool * Rings(r) * 0.9;
                albedo *= 1.3;
                float beacon = step(0.9, hc.y) * step(length(uv - size * 0.5), max(0.05, footprint * 1.5));
                emit += L.hot * 3.0 * beacon * step(0.5, frac(time * 0.5 + hc.z));
            }
            if (h.ruin > 0.5) {   // the crater: scorched rubble, embers dying out as it's rebuilt
                albedo = 0.03;
                inkTone = 0.75;
                float ember = step(0.86, Hash33(int3(floor(p * 9.0))).x);
                emit += L.hot * ember * 1.2 * (1.0 - regrow) * (0.6 + 0.4 * sin(time * 3.0 + hc.x * 20.0));
            }
            emit += L.hot * h.tear * 2.0 * exp(-p.y * 1.5);   // burning where the rim tears at them
            if (h.kind == 3) {   // the spire: bands of light climbing it, faster and brighter as it charges
                albedo = 0.12;
                emit += L.cool * (0.35 + 1.5 * charge) * step(0.5, frac(p.y * 1.5 - time * (1.0 + 4.0 * charge)));
                inkTone = 0.6;
            }
        } else if (h.kind == 2) {
            // Streets: dark asphalt, traffic streaming along the avenues, kick rings sweeping out.
            int2 c = int2(floor(p.xz));
            float2 f = frac(p.xz);
            albedo = L.ground;
            int2 m = Mod7(c);
            if (r > ruinR * (1.0 - regrow)) {
                if (m.x == 3) {
                    float lane = f.x < 0.5 ? 1.0 : -1.0;
                    float flow = frac(p.z * 0.6 - time * 0.8 * lane + Hash1(int2(c.x, 0), 3));
                    emit += (lane > 0 ? float3(1.0, 0.85, 0.6) : float3(1.0, 0.1, 0.05)) * step(0.9, flow) * step(abs(abs(f.x - 0.5) - 0.2), 0.04) * 1.2;
                }
                if (m.y == 3) {
                    float lane = f.y < 0.5 ? 1.0 : -1.0;
                    float flow = frac(p.x * 0.6 - time * 0.8 * lane + Hash1(int2(0, c.y), 4));
                    emit += (lane > 0 ? float3(1.0, 0.85, 0.6) : float3(1.0, 0.1, 0.05)) * step(0.9, flow) * step(abs(abs(f.y - 0.5) - 0.2), 0.04) * 1.2;
                }
                emit += glowCol * 0.05;   // street-level glow
            } else {
                albedo = 0.02;
                emit += L.hot * step(0.9, Hash33(int3(floor(p * 7.0))).x) * (1.0 - regrow) * 0.8;
            }
            emit += L.cool * Rings(r) * 1.2;
            float2 e = min(f, 1.0 - f);
            edge = 9.0;
            inkTone = (m.x == 3 || m.y == 3) ? 0.18 : 0.5;
            if (r < ruinR * (1.0 - regrow)) inkTone = 0.8;
        } else if (h.kind == 4) {
            emit = lerp(L.cool, float3(1, 1, 1), 0.5) * (1.5 + 4.0 * charge);
            inkFill = 0.0;
        }
        // Lighting: dim moonlight and sky, the orb, and the blast.
        float3 moonDir = normalize(float3(-0.4, 0.7, 0.3));
        float3 light = (lk == 2 ? float3(1.0, 0.6, 0.35) * 0.9 : float3(0.25, 0.3, 0.5) * 0.35) * saturate(dot(n, moonDir));
        light += (lk == 2 ? L.skyTop * 1.5 : float3(0.05, 0.045, 0.09)) * (0.6 + 0.4 * n.y);
        light += L.skyHorizon * 0.5 * (1.0 - abs(n.y));
        // The city's own glow bouncing off the walls, strongest low down.
        light += glowCol * 0.12 * (0.5 + 0.5 * intensity) * (1.0 - 0.5 * n.y) * exp(-p.y * 0.12);
        float3 toOrb = kOrb - p;
        float dOrb = length(toOrb);
        light += L.cool * charge * spire * 3.0 * saturate(dot(n, toOrb / dOrb)) / (1.0 + 0.01 * dOrb * dOrb);
        light += BlastLight(p, n, L.hot);
        light += flash * 2.0;
        col = albedo * light + emit;
    }

    // Haze: distance fog, glowing near the blast; the dome's halo; the clouds above.
    float3 fogCol = L.fog * (0.6 + 0.6 * intensity) + L.cool * charge * 0.08 + glowCol * 0.05 * (0.5 + intensity);
    fogCol += float3(1.0, 0.9, 0.8) * domeOn * domeWhite * 0.3 + L.hot * domeOn * (1.0 - domeWhite) * 0.08;
    float fogDensity = lk == 2 ? 0.016 : 0.011;
    float fog = 1.0 - exp(-h.t * fogDensity);
    if (h.kind == 0) fog = 0.0;
    if (h.kind == 5) fog *= lerp(0.25, 1.0, domeWhite);   // the void stays black through the haze
    col = lerp(col, fogCol, fog);
    float halo = DomeHalo(ro, rd, h.t);
    col += lerp(L.hot * 0.8, float3(1.1, 1.0, 0.95), domeWhite) * halo * domeOn * (h.kind == 5 ? 0.3 : 1.0);
    // The shockwave out in front of the dome: a bright ring of dust at street level.
    if (shockR > 0.5) {
        float2 g = p.xz;
        float sd = length(g) - shockR;
        col += float3(1.0, 0.85, 0.7) * exp(-sd * sd * 0.05) * saturate(1.0 - shockR / 200.0) * 1.5 * step(p.y, 3.0) * (h.kind == 5 ? 0.0 : 1.0);
    }
    // The light column rising from the orb while charging.
    {
        float3 w = ro - kOrb;
        float2 a = rd.xz, b = w.xz;
        float tc = max(-dot(a, b) / max(dot(a, a), 1e-5), 0.0);
        float y = ro.y + rd.y * tc;
        float d = length(b + a * tc);
        if (tc < h.t && y > kOrb.y && y < kCloud)
            col += L.cool * charge * charge * 1.5 * exp(-d * d * 4.0) * spire;
    }
    float4 cl = Clouds(ro, rd, L.hot, L.cool);
    float tCloud = rd.y > 0.0 ? (kCloud - ro.y) / rd.y : 1e9;
    if (tCloud < h.t) col = lerp(col, cl.rgb, cl.a);

    // Looks.
    if (lk == 1) {
        // Ink: tone from the night's light (inverted: dark areas get dots), lines where faces meet.
        float lum = dot(col, float3(0.3, 0.55, 0.15));
        float tone = 1.0 - saturate(1.0 - exp(-lum * 3.0));
        if (inkTone >= 0.0) {
            // Surfaces keep their own tone, washed out by the blast's light and by distance.
            float3 bl = BlastLight(p, h.n, L.hot) + flash * 2.0;
            tone = inkTone * saturate(1.0 - dot(bl, 0.4)) * lerp(1.0, 0.35, fog);
        }
        if (h.kind == 0) tone = 0.08 + 0.25 * saturate(1.0 - rd.y * 2.0);   // pale sky
        if (tCloud < h.t) tone = lerp(tone, 0.1 + 0.3 * cl.a, cl.a);   // clouds stay light: contours do the work
        float ink = Screentone(tone * 0.9, px);
        if (inkFill >= 0.0) ink = inkFill;
        ink = max(ink, step(edge, 1.1));
        if (tCloud < h.t && cl.a > 0.05) {   // cloud contours
            float iso = frac(cl.a * 5.0);
            ink = max(ink, step(abs(iso - 0.5), 0.5 * fwidth(cl.a * 5.0) * 1.2) * 1.0);
        }
        // Speed lines round a white-hot dome.
        if (domeOn * domeWhite > 0.3 && h.kind == 0) {
            float a = atan2(dot(rd, camUp), dot(rd, camRight));
            float lines = step(0.92, Hash1(int2(floor(a * 90.0), 0), 61));
            ink = max(ink, lines * step(0.0, rd.y) * domeWhite);
        }
        float3 paper = float3(0.94, 0.92, 0.86);
        float3 inkCol = float3(0.05, 0.05, 0.07);
        col = lerp(paper, inkCol, ink);
        col += flash * 0.25;
    } else if (lk == 2) {
        // Dusk: a painted feel: soft posterised light and canvas grain.
        float grain = Noise2(px * 0.7) * 0.06 - 0.03;
        col = col / (1.0 + col * 0.35);
        col = floor(col * 14.0 + 0.5) / 14.0 * 0.6 + col * 0.4;
        col += grain;
    } else {
        col *= 1.5;
        col = col / (1.0 + col * 0.3);   // gentle tone map
    }

    // Inside the dome: the world is white.
    if (domeOn > 0.01 && length(ro) < domeR) col = lerp(col, float3(1.4, 1.35, 1.3), domeWhite);
    col += flash * 0.35;
    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.5;
    return float4(max(col, 0.0), 1);
}
