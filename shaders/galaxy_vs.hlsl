#include "galaxy.hlsli"

// One galaxy particle per vertex, drawn as a point. Nothing is stored: each particle works
// out where it is from its index. Most are disc stars gathered along logarithmic spiral
// arms that turn as a pattern (so they never wind up); the rest are field stars, a bulge,
// the black hole's accretion disc and its jets.

struct POut {
    float4 pos : SV_Position;
    float3 col : COLOR0;
};

float3 Rand(uint id, uint salt) {
    return float3(Pcg3d(uint3(id, salt, id ^ 0x9E3779B9u)) & 0xFFFFu) / 65535.0;
}
float Gauss(float2 u) { return sqrt(-2.0 * log(max(u.x, 1e-5))) * cos(kTau * u.y); }

// A disc star for one galaxy shape. armness: 1 in the middle of an arm, 0 between arms.
// About a third of the arm stars belong to clusters (star-forming regions): clumps strung
// along the arms that make them look knotty rather than smooth.
float3 DiscStar(uint id, float4 s, float thick, out float armness, out float radius, out float cluster) {
    float3 r0 = Rand(id, 11), r1 = Rand(id, 12), r2 = Rand(id, 13);
    float a = 0.05 + 0.95 * saturate(-log(1.0 - r0.x * 0.95) / 3.0);   // exponential disc
    a += Gauss(r2.xy) * 0.015;
    float theta;
    cluster = 0.0;
    float arms = s.x;
    if (r0.y < 0.32) {                                  // field stars: everywhere, slowly turning
        theta = r1.x * kTau + pattern * 0.8;
        armness = 0.0;
    } else {
        float k = floor(r0.z * arms);
        float aw = ArmWidth(s);
        float spread = Gauss(r1.xy) * aw * (0.5 + 0.7 * a);
        if (r0.y > 0.5) {                               // a cluster member
            float c = floor(r1.z * 2600.0);
            float3 hc = Rand((uint)c, 77);
            a = 0.12 + 0.85 * hc.x;
            k = floor(hc.y * arms);
            spread = (hc.z - 0.5) * aw * 1.2;
            float2 g = float2(Gauss(r2.xz), Gauss(r2.zy));
            float size = 0.004 + 0.014 * frac(hc.x * 37.0);
            a += g.x * size;
            spread += g.y * size * 1.3 / a;
            cluster = 0.4 + 2.6 * frac(hc.z * 53.0) * frac(hc.y * 91.0);   // brightness varies a lot
        }
        theta = s.y * log(max(a, 0.02)) + k * kTau / arms + spread + pattern;
        armness = exp(-spread * spread / (aw * aw * 0.5));
    }
    theta += 0.04 * sin(animTime * 0.3 + r2.z * 40.0);  // a gentle wander so it never looks frozen
    float y = Gauss(r2.zx) * thick * (0.35 + 0.9 * exp(-a * 3.0)) * (1.0 - 0.6 * cluster);
    radius = a;
    return float3(cos(theta) * a, y, sin(theta) * a);
}

// Dust: thin, dark lanes hugging the inner edge of each arm (drawn subtractively).
float3 Dust(uint id, float4 s, float thick, out float radius) {
    float3 r0 = Rand(id, 61), r1 = Rand(id, 62);
    float a = 0.1 + 0.8 * pow(r0.x, 0.8);
    float k = floor(r0.y * s.x);
    float aw = ArmWidth(s);
    float spread = -aw * 0.45 + Gauss(r1.xy) * aw * 0.22;   // trailing just inside the arm
    float theta = s.y * log(a) + k * kTau / s.x + spread + pattern + 0.04 * sin(animTime * 0.3 + r1.z * 40.0);
    radius = a;
    return float3(cos(theta) * a, Gauss(r1.zx) * thick * 0.25, sin(theta) * a);
}

float3 Bulge(uint id, float size) {
    float3 r0 = Rand(id, 21), r1 = Rand(id, 22);
    float r = size * saturate(-log(1.0 - r0.x * 0.97) * 0.3);
    float z = r0.y * 2.0 - 1.0, phi = r0.z * kTau + pattern * 1.4;
    float s = sqrt(1.0 - z * z);
    return float3(s * cos(phi), z * 0.6, s * sin(phi)) * r;
}

POut main(uint id : SV_VertexID) {
    POut o;
    float3 kind = Rand(id, 1);
    bool dust = id >= kStarCount;
    float3 p;
    float3 col;
    float bright = 1.0;
    bool galactic = true;   // feels the beat shockwaves

    if (dust) {
        // ---- dust lanes (this pass subtracts light)
        float ra, rb;
        p = lerp(Dust(id, shapeA, thickA, ra), Dust(id, shapeB, thickB, rb), shapeBlend);
        float rad = lerp(ra, rb, shapeBlend);
        col = float3(0.75, 0.85, 1.0) * dustAmount / max(starBright, 1e-6);
        bright = smoothstep(0.08, 0.25, rad) * smoothstep(0.95, 0.6, rad);
    } else if (kind.x < 0.10) {
        // ---- bulge: old, warm stars
        p = lerp(Bulge(id, shapeA.w), Bulge(id, shapeB.w), shapeBlend);
        col = coreCol;
        bright = 0.25;
    } else if (kind.x < 0.155) {
        // ---- accretion disc around the black hole: hot and fast, faster with gravity
        float3 r = Rand(id, 31);
        float rs = 0.018;
        float a = rs * (1.7 + 4.5 * r.x * r.x);
        float theta = r.y * kTau + accSpin * pow(0.04 / a, 1.5);
        p = float3(cos(theta) * a, Gauss(r.zy) * 0.0015, sin(theta) * a);
        float heat = saturate(1.0 - (a / rs - 1.7) / 4.5);
        col = lerp(discCol, float3(1.0, 0.95, 0.9), heat * heat);
        bright = (0.04 + 0.25 * heat) * accretion;
        galactic = false;
    } else if (kind.x < 0.195) {
        // ---- jets: plasma streaming out of both poles (only during drops)
        float3 r = Rand(id, 41);
        float t = frac(r.x + animTime * 0.9);
        float len = (0.4 + 2.6 * jet) * t;
        float wid = 0.003 + 0.035 * t;
        float ang = r.y * kTau + t * 14.0 - animTime * 3.0;
        float sgn = r.z < 0.5 ? 1.0 : -1.0;
        p = float3(cos(ang) * wid * r.z * 2.0, sgn * len, sin(ang) * wid * r.z * 2.0);
        col = lerp(float3(0.9, 0.95, 1.0), armCol, t);
        float knots = 0.35 + 0.65 * pow(0.5 + 0.5 * sin(t * 30.0 - animTime * 12.0), 3.0);   // plasma blobs
        bright = 6.0 * jet * jet * (1.0 - t) * knots;
        galactic = false;
    } else {
        // ---- disc stars, morphing between the two shapes
        float armA, armB, radA, radB, clA, clB;
        float3 pa = DiscStar(id, shapeA, thickA, armA, radA, clA);
        float3 pb = DiscStar(id, shapeB, thickB, armB, radB, clB);
        float cl = lerp(clA, clB, shapeBlend);
        p = lerp(pa, pb, shapeBlend);
        float armness = lerp(armA, armB, shapeBlend);
        float rad = lerp(radA, radB, shapeBlend);
        float3 r = Rand(id, 51);
        col = lerp(coreCol, armCol, smoothstep(0.06, 0.45, rad));
        col = lerp(lerp(col, coreCol, 0.35) * float3(1.0, 0.92, 0.85), col, armness);   // between the arms: older, warmer
        bright = 0.25 + 0.3 * armness;
        if (cl > 0.0) {                                                // star-forming clusters: young, hot, bright
            col = lerp(lerp(armCol, float3(0.85, 0.9, 1.0), 0.3), knotCol, step(0.55, frac(cl * 7.3)));
            bright = 1.6 * cl * smoothstep(0.1, 0.4, rad);
        }
        if (r.y < 0.02) {                                              // a few bright, twinkling stars
            bright *= 4.0 * (1.0 + twinkle * 0.8 * sin(animTime * (8.0 + 20.0 * r.z) + r.z * 60.0));
            col = lerp(col, float3(1, 1, 1), 0.5);
        }
    }

    if (galactic && !dust) {
        // Gravity pulls everything in a little.
        p.xz *= 1.0 - 0.07 * gravity;
        // Beat shockwaves race outwards, flinging the arms out and lighting them up.
        float rr = length(p.xz);
        float push = 0.0;
        [unroll] for (int w = 0; w < 4; ++w) {
            float d = (rr - waves[w].x) / 0.07;
            push += waves[w].y * exp(-d * d);
        }
        p.xz *= 1.0 + 0.14 * push;
        p.y *= 1.0 + 1.2 * push;
        bright *= 1.0 + 3.0 * push;
    }

    float3 v = p - camPos;
    float z = dot(v, camFwd);
    o.pos = float4(dot(v, camRight) * focal / aspect, dot(v, camUp) * focal, 0.5 * z, z);
    o.col = col * bright * starBright * (1.0 + flash);
    return o;
}
