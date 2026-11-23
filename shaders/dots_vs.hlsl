#include "dots.hlsli"

// The Dot Field: a plane of dots seen from an isometric angle, bouncing to the music. One
// instance per dot, a quad each (6 vertices). Nothing is stored per dot: each works out where
// it sits from its index (the layout: grid, scatter, rows, rings, spiral) and how high it
// bounces from the audio history (the motion: ripples, terrain, equaliser, swell).

Texture2D<float> History : register(t0);   // 66 columns (64 bands, bass, level) x 512 rows at 60 Hz
SamplerState HistSampler : register(s0);   // clamp across, wrap down (a ring buffer)

struct DOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;   // -1..1 across the quad
    float3 col : COLOR0;
};

static const float kRows = 512.0, kCols = 66.0, kRate = 60.0, kMaxH = 0.32;

float Hist(float col, float lag) {
    float v = (histHead - max(lag, 0.0) * kRate + 0.5) / kRows;
    return History.SampleLevel(HistSampler, float2((col + 0.5) / kCols, v), 0);
}
float Band(float x, float lag) { return Hist(saturate(x) * 63.0, lag); }   // 0 = bass .. 1 = treble
float Bass(float lag) { return Hist(64.0, lag); }
float Level(float lag) { return Hist(65.0, lag); }

float2 Rand2(uint id, uint salt) { return float2(Pcg3d(uint3(id, salt, id ^ 0x9E3779B9u)).xy & 0xFFFFu) / 65535.0; }

// Where dot `id` sits on the plane (about -1..1) in a layout, and how far apart dots are there.
float2 LayoutPos(int layout, uint id, out float spacing) {
    uint s = (uint)side, n = s * s;
    float f = (id + 0.5) / n;
    if (layout == 1) {                       // scatter: random, like sand
        spacing = 1.6 / side;
        return Rand2(id, 7) * 2.0 - 1.0;
    }
    if (layout == 2) {                       // rows: lines of tightly packed dots
        uint rows = s / 2, per = s * 2;
        uint j = id / per, i = id % per;
        spacing = 1.0 / side;
        return float2((i + 0.5) / per, (j + 0.5) / rows) * 2.0 - 1.0;
    }
    if (layout == 3) {                       // rings: concentric circles, evenly dotted
        float R = side * 0.5;
        float x = f * R * R;
        float i = floor(sqrt(x));
        float a = (x - i * i) / (2.0 * i + 1.0);
        float r = (i + 0.5) / R * 1.15;
        spacing = 1.8 / side;
        return r * float2(cos(a * kTau), sin(a * kTau));
    }
    if (layout == 4) {                       // spiral: a sunflower head (phyllotaxis)
        float r = sqrt(f) * 1.15;
        float t = id * 2.39996323;
        spacing = 2.0 / side;
        return r * float2(cos(t), sin(t));
    }
    spacing = 2.0 / side;                    // grid
    return (float2(id % s, id / s) + 0.5) / side * 2.0 - 1.0;
}

// How high a dot at p bounces in one motion (0 .. about kMaxH).
float Height(int motion, float2 p) {
    float r = length(p);
    float across = abs(atan2(p.y, p.x)) / kPi;   // 0..1, mirrored so there's no seam
    if (motion == 1) {                           // equaliser: bass in the middle, treble at the rim
        float lag = across * 0.35;
        return kMaxH * pow(Band(r * 0.8, lag), 1.3);
    }
    // terrain: frequency across, time rolling away
    float lag = (0.5 - 0.5 * p.y) * 3.5;
    return kMaxH * pow(Band(abs(p.x) * 0.92, lag), 1.3);
}


// Everything about one dot this frame.
struct Dot {
    float3 world;
    float3 col;
    float bright;
    float rad;    // world units
    float hn;     // 0..1, how high it's bouncing
};

float2 Rotate(float2 v, float a) { float c = cos(a), s = sin(a); return float2(c * v.x - s * v.y, s * v.x + c * v.y); }

Dot ComputeDot(uint id) {
    float spA, spB;
    float2 pa = LayoutPos((int)layoutA, id, spA);
    float2 pb = LayoutPos((int)layoutB, id, spB);
    float2 rnd = Rand2(id, 3);

    // Splash: a shockwave races out from the middle and throws every dot in the air; they bounce a
    // couple of times as they land (in a new layout when the splash morphs).
    float air = 0.0, blend = morph;
    float r0 = length(lerp(pa, pb, morph));
    if (splashT >= 0.0) {
        float t = splashT - r0 / 1.4 - rnd.x * 0.05;
        float v = splashPower * (1.0 - 0.3 * saturate(r0)) * (0.75 + 0.5 * rnd.y);
        const float g = 5.0;
        float firstFlight = 2.0 * v / g;
        if (splashMorph > 0.5) blend = smoothstep(0.0, firstFlight, t);
        if (t > 0.0) {
            [unroll] for (int b = 0; b < 3; ++b) {
                float T = 2.0 * v / g;
                if (t < T) { air = v * t - 0.5 * g * t * t; break; }
                t -= T;
                v *= 0.38;
            }
        }
    }
    float2 p = lerp(pa, pb, blend);
    float spacing = lerp(spA, spB, blend);

    float h = lerp(Height((int)motionA, p), Height((int)motionB, p), motionBlend);
    float hn = saturate(h / kMaxH);
    float lift = saturate(air / 0.5);

    // The bar's scan line: a bright edge with a fading trail, nudging dots up as it passes.
    float sd = p.x - scanPos;
    float scan = (exp(-sd * sd / 0.0012) + 0.35 * exp(min(sd, 0.0) / 0.12) * step(sd, 0.0)) * scanStrength;
    h += 0.025 * scan;

    // Breach: the plane shatters into tiles that lift, tilt, spin and drift apart (each carrying its
    // piece of the terrain), then snap back together on a downbeat.
    float edgeGlow = 0.0;
    if (breach > 0.0) {
        float2 tile = floor((p + 1.0) * 3.5);
        float2 c = (tile + 0.5) / 3.5 - 1.0;
        float2 local = p - c;
        uint ts = (uint)(tile.x * 31.0 + tile.y * 7.0 + 500.0) + (uint)breachSeed * 101u;
        float2 r1 = Rand2(ts, 55), r2 = Rand2(ts, 56);
        float2 out2 = c / max(length(c), 0.2);
        p = c + Rotate(local, (r1.x - 0.5) * 1.0 * breach) + out2 * (0.12 + 0.22 * r1.y) * breach;
        h += ((0.06 + 0.22 * r2.x) + dot(local, (r2 - 0.5) * 1.1)) * breach;
        float e = max(abs(local.x), abs(local.y)) * 7.0;   // 0 at a tile's centre, 1 at its edge
        edgeGlow = smoothstep(0.82, 1.0, e) * saturate(breach * 3.0);
    }

    // Glitch: slices of the plane torn sideways.
    float slice = floor((p.y + 1.0) * 14.0);
    float2 gh = Rand2((uint)(slice + 37.0 * glitchSeed), 91);
    float torn = step(gh.x, 0.3 * glitch);
    p.x += torn * (gh.y - 0.5) * 0.35 * glitch;

    // Colour: dim at rest, through the mid colour to the peak colour as it rises.
    float3 col = lerp(baseCol, midCol, smoothstep(0.0, 0.45, hn));
    col = lerp(col, peakCol, smoothstep(0.45, 1.0, max(max(hn, lift), saturate(scan * 0.8 + edgeGlow))));
    col = lerp(col, gh.y > 0.5 ? peakCol : midCol, torn);
    float bright = 0.3 + 2.4 * hn * hn + 1.5 * lift + 1.2 * scan + torn + 2.5 * edgeGlow;
    float2 sp = Rand2(id, (uint)hatSeed);   // hi-hats: a sprinkle of dots flicker
    bright += hat * step(sp.x, 0.025) * 3.0;

    Dot d;
    d.world = float3(p.x, h + air, p.y);
    d.col = col;
    d.bright = bright * glow * dotBright * (1.0 + 0.6 * flash);
    d.rad = spacing * 0.32 * (1.0 + 0.5 * hn + 0.4 * lift);
    d.hn = hn;
    return d;
}

float2 ToScreen(float3 world) { return float2(dot(world, camRight) * zoom / aspect, dot(world, camUp) * zoom + offsetY); }

static const float2 kCorner[6] = { float2(-1, -1), float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(1, -1) };

// Data beams: the highest peaks shoot thin shafts of light straight up, fading as they rise.
// One candidate dot in every 64 can fire one.
DOut Beam(uint vid, uint k) {
    uint id = k * 64u + (Pcg3d(uint3(k, 17u, 5u)).x & 63u);
    Dot d = ComputeDot(id);
    float strength = saturate((d.hn - 0.62) / 0.38) * beams * saturate(1.0 - 3.0 * breach);
    float len = strength * (0.2 + 0.4 * Rand2(id, 9).x);
    float2 c = kCorner[vid];
    float2 base = ToScreen(d.world), top = ToScreen(d.world + float3(0, len, 0));
    float w = max(d.rad * 0.6, minRadius) * zoom / aspect;
    float up = c.y * 0.5 + 0.5;   // 0 at the dot, 1 at the tip
    DOut o;
    o.uv = float2(c.x, 0.0);
    o.pos = float4(lerp(base, top, up) + float2(c.x * w, 0.0), 0.5, 1.0);
    o.col = lerp(d.col, peakCol, 0.5) * (1.0 - up) * strength * 3.0 * glow * (1.0 + flash);
    return o;
}

DOut main(uint vid : SV_VertexID, uint id : SV_InstanceID) {
    uint n = (uint)side * (uint)side;
    if (id >= n) return Beam(vid, id - n);

    Dot d = ComputeDot(id);
    // Round dots, a little bigger as they rise. Below a pixel the quad stays a pixel and dims
    // instead, so the total light is the same at any density.
    float quad = max(d.rad, minRadius);
    float2 c = kCorner[vid];
    DOut o;
    o.uv = c;
    o.pos = float4(ToScreen(d.world) + c * quad * zoom * float2(1.0 / aspect, 1.0), 0.5, 1.0);
    o.col = d.col * d.bright * (d.rad * d.rad) / (quad * quad);
    return o;
}
