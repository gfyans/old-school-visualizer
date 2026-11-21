#include "common.hlsli"

// Fractal flights: flying forward through an infinite fractal. Each world is a distance
// estimator for a space-filling fractal; a winding tunnel is carved along the camera's path
// so the flight never crashes, and its walls show the fractal's cut cross-section.
// Shading: orbit-trap colouring, glowing veins, a headlight, near-miss glow and fog.

cbuffer Fractal : register(b2) {   // matches FractalConstants in FractalVisualizer.cpp
    float3 camPos;   float focal;
    float3 camRight; float breath;    // bass: the fractal breathes
    float3 camUp;    float spin;      // world rotation around the flight path (mids)
    float3 camFwd;   float glowAmt;
    float3 albedo;   float emit;      // vein brightness
    float3 emitCol;  float sparkle;   // treble glints
    float3 fogCol;   float flash;
    float4 pulses[4];                 // rings of light: x world z, y strength
    float world;     float animTime;  float hyper;  float tunnel;   // tunnel radius
    float chase;     float3 fracPad;  // beat-locked light bands running down the walls
};

static const int kSteps = 110;
static const float kMaxDist = 22.0;

float2 Path(float z) {   // the flight path (matches Path() in FractalVisualizer.cpp)
    return float2(sin(z * 0.11) * 1.1 + sin(z * 0.037) * 1.9, sin(z * 0.07) * 0.7 + cos(z * 0.029) * 0.9);
}

float2x2 Rot(float a) { float c = cos(a), s = sin(a); return float2x2(c, -s, s, c); }

// ---------------------------------------------------------------- worlds
// Each returns a distance estimate and an orbit trap (roughly 0..1, small in the "veins").

float Menger(float3 p, out float trap) {
    float d = -1e5, s = 1.0;
    trap = 1.0;
    float holes = 1.0 + 0.12 * breath;
    [unroll] for (int i = 0; i < 5; ++i) {
        float3 a = frac(p * s * 0.5) * 2.0 - 1.0;
        s *= 3.0;
        float3 r = abs(1.0 - 3.0 * abs(a));
        float da = max(r.x, r.y), db = max(r.y, r.z), dc = max(r.z, r.x);
        float c = (min(da, min(db, dc)) - holes) / s;
        if (c > d) { d = c; trap = min(trap, min(da, min(db, dc)) * 0.5 + 0.08 * i); }
    }
    return d;
}

float Mandelbox(float3 p, out float trap) {
    const float k = 2.2;                       // world scale
    float3 q = p * k;
    q = q - 8.0 * floor(q / 8.0 + 0.5);        // tile
    float S = -1.85 - 0.2 * breath;
    float4 z = float4(q, 1.0);
    trap = 1e5;
    [loop] for (int i = 0; i < 10; ++i) {
        z.xyz = clamp(z.xyz, -1.0, 1.0) * 2.0 - z.xyz;
        float r2 = dot(z.xyz, z.xyz);
        trap = min(trap, r2);
        float m = clamp(1.0 / r2, 1.0, 4.0);
        z *= m;
        z.xyz = z.xyz * S + q;
        z.w = z.w * abs(S) + 1.0;
    }
    trap = saturate(sqrt(trap) * 0.6);
    return (length(z.xyz) - abs(S - 1.0)) / abs(z.w) / k;
}

float Apollonian(float3 p, out float trap) {
    float s = 1.18 + 0.1 * breath;
    float scale = 1.0;
    trap = 1e5;
    [loop] for (int i = 0; i < 8; ++i) {
        p = -1.0 + 2.0 * frac(0.5 * p + 0.5);
        float r2 = dot(p, p);
        trap = min(trap, r2);
        float k = s / r2;
        p *= k;
        scale *= k;
    }
    trap = saturate(trap * 1.2);
    return 0.25 * abs(p.y) / scale;
}

float Crystal(float3 p, out float trap) {
    p = p - 2.6 * floor(p / 2.6 + 0.5);        // tile
    float sc = 2.0 + 0.08 * breath;
    float2x2 ra = Rot(0.3 + 0.2 * sin(spin * 0.7)), rb = Rot(0.5 + 0.15 * cos(spin * 0.5));
    trap = 1e5;
    [loop] for (int i = 0; i < 8; ++i) {
        p = abs(p);
        if (p.x < p.y) p.xy = p.yx;
        if (p.x < p.z) p.xz = p.zx;
        if (p.y < p.z) p.yz = p.zy;
        p.xy = mul(ra, p.xy);
        p.yz = mul(rb, p.yz);
        p = p * sc - float3(1.0, 0.55, 0.3) * (sc - 1.0);
        trap = min(trap, dot(p, p));
    }
    trap = saturate(sqrt(trap) * 0.5);
    return (max(max(abs(p.x), abs(p.y)), abs(p.z)) - 1.0) * pow(abs(sc), -8.0);
}

// Vaulted halls: a Menger-style kaleidoscopic fractal whose folds turn slowly.
float Cathedral(float3 p, out float trap) {
    p = p - 3.0 * floor(p / 3.0 + 0.5);        // tile
    const float sc = 3.0;
    float2x2 ra = Rot(0.12 + 0.1 * sin(spin * 0.6)), rb = Rot(0.08 * cos(spin * 0.4));
    float holes = 1.0 + 0.06 * breath;
    trap = 1e5;
    [loop] for (int i = 0; i < 6; ++i) {
        p = abs(p);
        if (p.x < p.y) p.xy = p.yx;
        if (p.x < p.z) p.xz = p.zx;
        if (p.y < p.z) p.yz = p.zy;
        p.xy = mul(ra, p.xy);
        p.xz = mul(rb, p.xz);
        p.x = sc * p.x - holes * (sc - 1.0);
        p.y = sc * p.y - holes * (sc - 1.0);
        p.z = sc * p.z;
        if (p.z > 0.5 * (sc - 1.0)) p.z -= sc - 1.0;
        trap = min(trap, dot(p, p));
    }
    trap = saturate(sqrt(trap) * 0.25);
    return (max(max(abs(p.x), abs(p.y)), abs(p.z)) - 1.0) * pow(sc, -6.0);
}

float Map(float3 p, out float trap) {
    float2 c = Path(p.z);
    float3 q = p;
    q.xy = mul(Rot(spin), p.xy - c) + c;       // the world turns around the flight path
    float d;
    int w = (int)world;
    if (w == 0)      d = Menger(q, trap);
    else if (w == 1) d = Mandelbox(q, trap);
    else if (w == 2) d = Apollonian(q, trap);
    else if (w == 3) d = Crystal(q, trap);
    else             d = Cathedral(q, trap);
    float tube = tunnel - length(p.xy - c);    // carve the flight tunnel
    return max(d, tube);
}

float3 Normal(float3 p, float eps) {
    float t;
    const float2 k = float2(1, -1);
    return normalize(k.xyy * Map(p + k.xyy * eps, t) + k.yyx * Map(p + k.yyx * eps, t) +
                     k.yxy * Map(p + k.yxy * eps, t) + k.xxx * Map(p + k.xxx * eps, t));
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((i.uv.x * 2.0 - 1.0) * aspect, 1.0 - i.uv.y * 2.0);
    float3 rd = normalize(camFwd * focal + camRight * sp.x + camUp * sp.y);
    float3 ro = camPos;
    float pixel = 2.0 / (iResolution.y * focal);   // angular size of a pixel

    float t = 0.01, trap = 1.0, glow = 0.0;
    int steps = 0;
    bool hit = false;
    float jitter = InterleavedGradientNoise(i.pos.xy);
    t += jitter * 0.01;
    [loop] for (; steps < kSteps; ++steps) {
        float3 p = ro + rd * t;
        float d = Map(p, trap);
        float eps = max(pixel * t * 0.8, 1e-4);
        glow += exp(-d * 40.0) * 0.02 / (1.0 + t * 0.3);
        if (d < eps) { hit = true; break; }
        t += d * 0.85;
        if (t > kMaxDist) break;
    }

    float3 col = fogCol;
    if (hit) {
        float3 p = ro + rd * t;
        float3 n = Normal(p, max(pixel * t, 2e-4));
        float ao = 1.0 - (float)steps / kSteps;
        ao *= ao;
        // Headlight from the camera, plus a soft fill from the vein colour.
        float diff = saturate(dot(n, -rd));
        float spec = pow(saturate(dot(reflect(rd, n), -rd)), 24.0);
        float atten = 1.0 / (1.0 + t * t * 0.08);
        float3 base = lerp(albedo, albedo * emitCol * 2.0, 0.35 * (1.0 - trap));
        col = base * (0.15 + diff * 1.2 * atten) * ao + spec * atten * 0.4;
        // Veins glow where the orbit trap is small, pulsing with the bass.
        float vein = pow(saturate(1.0 - trap), 6.0);
        col += emitCol * vein * emit * (0.5 + 0.5 * ao);
        // Rings of light the flight passes through.
        float ring = 0.0;
        [unroll] for (int k = 0; k < 4; ++k) ring += pulses[k].y * exp(-pow((p.z - pulses[k].x) / 0.25, 2.0));
        col += emitCol * ring * (0.4 + 1.5 * vein) * 1.5;
        // Bands of light chasing down the tunnel, one per beat (aTempo.y = beat phase).
        float band = pow(0.5 + 0.5 * cos(6.2831853 * (p.z * 0.5 + aTempo.y)), 10.0);
        col += emitCol * band * chase * (0.25 + 1.2 * vein) * atten * 2.0;
        // Treble glints.
        float3 cell = floor(p * 70.0);
        col += emitCol * step(0.997, Hash33(int3(cell)).x) * sparkle * 4.0 * atten;
        col = lerp(fogCol, col, exp(-t * 0.13));
    }
    col += emitCol * glow * glowAmt;

    // Hyperspeed: the colour streams towards the centre of the view.
    float2 v = i.uv - 0.5;
    col += emitCol * hyper * 0.25 * pow(saturate(length(v) * 1.8), 3.0);
    col += flash;
    col *= 1.0 - dot(v, v) * 0.7;
    col = 1.0 - exp(-col * 1.3);
    return float4(col, 1);
}
