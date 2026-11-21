#include "cyber.hlsli"

// Signal: 3D subjects turning in the dark, drawn in cyberpunk glitch styles (cyber.hlsli). One sample
// per cell (a glyph, a dither dot, an LED), so it's cheap. Subjects:
//   0 the skull: a model (drawn instanced into sceneNow/scenePrev by signal_mesh_*.hlsl and read back
//     here), or a distance-field skull with a hinged jaw when the exe was built without the model
//   1 a lattice "brain"   2 a block monolith (distance functions, one ray per cell)
// Layouts: one subject in the centre, or a scrolling grid of them filling the screen; changes wipe across
// cell by cell. Lit hard from one side with a cool rim light, shadowed by ambient occlusion, over data rain.

cbuffer Signal : register(b2) {   // matches SignalConstants in SignalVisualizer.cpp
    float style;     float subject;  float cellScale;  float time;
    float yaw;       float pitch;    float jaw;        float pulse;      // jaw: 0 shut .. 1 open
    float glitch;    float flicker;  float dissolve;   float flash;      // flash: drop inversion 0..1
    float eyes;      float morph;    float rain;       float hasMesh;    // morph: blend towards the next subject
    float3 palBg;    float nextSubject;
    float3 palMid;   float layoutMix;    // 0..1: wipe from the previous layout to this one
    float3 palHi;    float spin;         // slow turning phase
    float3 palAccent; float scroll;      // grid scroll, screen units
    float rowsNow;   float rowsPrev;  float beatAge;  float beatHit;   // rows 0 = one subject in the centre
};

Texture2D sceneNow : register(t0);    // the model in this layout (rgb premultiplied, a coverage; mipmapped)
Texture2D scenePrev : register(t1);   // ...and in the previous one, during a layout wipe
SamplerState sceneSampler : register(s0);

// The subject being drawn by this ray, and how it's posed (set per cell/tile before marching).
static int g_subjA = 0, g_subjB = 0;
static float g_morph = 0.0, g_yaw = 0.0, g_pitch = 0.0, g_scale = 1.0, g_eyes = 0.0;
static const float kBackRows = 4.0;   // the dim grid behind a subject in the centre (matches SignalVisualizer.cpp)
static const float kBackDim = 0.3;
static const float kTileView = 0.68;   // how much of a grid tile the subject fills (matches SignalVisualizer.cpp)

float SdSphere(float3 p, float r) { return length(p) - r; }
float SdEllipsoid(float3 p, float3 r) {   // good-enough ellipsoid distance (iq)
    float k0 = length(p / r), k1 = length(p / (r * r));
    return k0 * (k0 - 1.0) / k1;
}
float SdBox(float3 p, float3 b, float r) { float3 q = abs(p) - b + r; return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r; }
float SdCapsule(float3 p, float3 a, float3 b, float r) {
    float3 pa = p - a, ba = b - a;
    return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba))) - r;
}
float SMin(float a, float b, float k) { float h = saturate(0.5 + 0.5 * (b - a) / k); return lerp(b, a, h) - k * h * (1.0 - h); }
float SMax(float a, float b, float k) { return -SMin(-a, -b, k); }
float2x2 Rot(float a) { float c = cos(a), s = sin(a); return float2x2(c, -s, s, c); }

// --- Bone texture: smooth 3D value noise (pitting) and cell edges (hairline cracks) -----------
float Noise3(float3 p) {
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    int3 c = int3(i);
    float n000 = Hash33(c).x, n100 = Hash33(c + int3(1, 0, 0)).x, n010 = Hash33(c + int3(0, 1, 0)).x, n110 = Hash33(c + int3(1, 1, 0)).x;
    float n001 = Hash33(c + int3(0, 0, 1)).x, n101 = Hash33(c + int3(1, 0, 1)).x, n011 = Hash33(c + int3(0, 1, 1)).x, n111 = Hash33(c + int3(1, 1, 1)).x;
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y), lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}
float CrackEdge(float3 p) {   // distance to the nearest cell border of a jittered 3D grid (small on a crack)
    float3 i = floor(p), f = frac(p);
    float d1 = 9.0, d2 = 9.0;
    [loop] for (int z = -1; z <= 1; ++z)
    [loop] for (int y = -1; y <= 1; ++y)
    [loop] for (int x = -1; x <= 1; ++x) {
        float3 o = float3(x, y, z);
        float3 r = o + Hash33(int3(i + o) + 911) - f;
        float d = dot(r, r);
        if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
    }
    return sqrt(d2) - sqrt(d1);
}

// A gaunt human skull, facing +z (y up, about 1 unit = half its height). Material: 0 bone, 1 teeth,
// 2 inside an eye socket. Built from the parts that make a skull read as one: a long cranium with
// hollow temples, a heavy brow over deep orbits, cheekbones with thin arches back to the ears, an
// inverted-pear nasal cavity, a narrow upper jaw with long uneven teeth, and a hinged jaw.
float Teeth(float3 p, float3 centre, float radius, float halfArc, float height, int salt) {
    float3 t = p - centre;
    float ang = atan2(t.x, t.z);
    const float pitch = 0.135;
    float idx = floor((ang + 4.0) / pitch);
    float3 h = Hash33(int3((int)idx, salt, 5));
    float local = (ang + 4.0) - (idx + 0.5) * pitch;
    float len = height * (0.8 + 0.4 * h.x) * (abs(ang) > 0.6 ? 0.85 : 1.0);   // uneven; molars shorter
    float tooth = SdBox(float3(local * radius, t.y - (salt == 1 ? -len * 0.4 : len * 0.4), length(t.xz) - radius),
                        float3(0.026 + 0.006 * h.y, len, 0.03), 0.014);
    return max(tooth, abs(ang) - halfArc);
}

float Skull(float3 p, out float mat) {
    mat = 0.0;
    float3 m = float3(abs(p.x), p.yz);   // mirrored for the paired parts
    // Cranium: long front to back, flattened and hollowed at the temples.
    float d = SdEllipsoid(p - float3(0, 0.28, -0.14), float3(0.7, 0.72, 0.9));
    d = SMax(d, -SdEllipsoid(m - float3(0.78, 0.0, 0.18), float3(0.2, 0.32, 0.42)), 0.12);
    // Brow ridge over the orbits.
    d = SMin(d, SdCapsule(m, float3(0.05, 0.09, 0.66), float3(0.42, 0.1, 0.56), 0.065), 0.07);
    // Facial skeleton: narrower than the cranium.
    d = SMin(d, SdEllipsoid(p - float3(0, -0.16, 0.42), float3(0.42, 0.38, 0.34)), 0.14);
    // Cheekbones and the thin arches running back to the ears, with hollows beneath them.
    d = SMin(d, SdEllipsoid(m - float3(0.4, -0.12, 0.47), float3(0.12, 0.1, 0.13)), 0.06);
    d = SMin(d, SdCapsule(m, float3(0.42, -0.12, 0.42), float3(0.6, -0.06, 0.02), 0.045), 0.03);
    d = SMax(d, -SdEllipsoid(m - float3(0.5, -0.3, 0.2), float3(0.12, 0.16, 0.26)), 0.05);
    // Deep orbits, tilted slightly outward, with a sharp rim.
    float3 o = m - float3(0.23, -0.01, 0.62);
    o.xz = mul(Rot(-0.25), o.xz);
    float eye = SdEllipsoid(o, float3(0.155, 0.145, 0.3));
    eye = min(eye, SdSphere(m - float3(0.2, 0.0, 0.42), 0.13));   // the socket runs deep into the skull
    d = SMax(d, -eye, 0.02);
    // Nasal cavity: an inverted pear, pointed at the top.
    float3 nq = p - float3(0, -0.22, 0.72);
    nq.x *= 1.0 + 4.0 * max(nq.y, 0.0);
    float nose = SdEllipsoid(nq, float3(0.07, 0.12, 0.22));
    d = SMax(d, -nose, 0.015);
    // Upper jaw (maxilla): a narrow block the teeth hang from.
    d = SMin(d, SdBox(p - float3(0, -0.42, 0.38), float3(0.22, 0.06, 0.18), 0.05), 0.08);
    float upper = Teeth(p, float3(0, -0.5, 0.1), 0.4, 0.95, 0.075, 0);

    // The jaw, hinged just under the ears.
    const float3 hinge = float3(0, -0.36, -0.12);
    float3 j = p - hinge;
    j.yz = mul(Rot(-jaw * 0.42 * step(rowsNow + rowsPrev, 0.5)), j.yz);   // the jaw only works up close
    j += hinge;
    float3 jm = float3(abs(j.x), j.yz);
    float3 ja = j - float3(0, -0.74, 0.1);
    float bodyR = length(ja.xz);
    float jawBone = length(float2(bodyR - 0.4, ja.y * 0.8)) - 0.065;   // a U-shaped bar
    jawBone = max(jawBone, -ja.z - 0.12);                               // open at the back
    jawBone = SMin(jawBone, SdEllipsoid(j - float3(0, -0.8, 0.5), float3(0.12, 0.07, 0.06)), 0.06);   // chin
    jawBone = SMin(jawBone, SdCapsule(jm, float3(0.39, -0.76, -0.02), float3(0.45, -0.38, -0.14), 0.05), 0.05);   // branches up to the hinge
    float lower = Teeth(j, float3(0, -0.66, 0.1), 0.38, 0.9, 0.065, 1);

    // Bone texture near the surface only (it's the expensive part): pitting and hairline cracks.
    if (d < 0.06) {
        d += 0.012 * (Noise3(p * 18.0) - 0.5) + 0.005 * (Noise3(p * 47.0) - 0.5);
        d += 0.008 * smoothstep(0.035, 0.0, CrackEdge(p * 3.2)) * step(0.1, p.y);   // cracks on the cranium
    }
    if (jawBone < 0.06) jawBone += 0.01 * (Noise3(j * 18.0) - 0.5);

    float teeth = min(upper, lower);
    float total = min(min(d, jawBone), teeth);
    if (teeth <= total + 1e-4) mat = 1.0;
    if (eye < 0.03 && d < 0.05) mat = 2.0;
    return total;
}

// A sphere hollowed into a gyroid lattice: a cyber brain.
float Brain(float3 p, out float mat) {
    mat = 0.0;
    float s = SdSphere(p, 1.0);
    float3 q = p * 7.0 + time * 0.3;
    float g = abs(dot(sin(q), cos(q.yzx))) / 7.0 - 0.025;
    float core = SdSphere(p, 0.35 + 0.1 * pulse);
    if (core < g) mat = 2.0;
    return min(max(s, g), core);
}

// Stacked blocks folded like a small Menger sponge.
float Monolith(float3 p, out float mat) {
    mat = 0.0;
    // A tall block, carved like a Menger sponge (the construction Fractal flights' Menger halls use).
    float3 q = p / float3(0.6, 1.1, 0.6);
    float d = SdBox(q, float3(1.0, 1.0, 1.0), 0.02);
    float s = 1.0;
    [unroll] for (int i = 0; i < 3; ++i) {
        float3 a = frac(q * s * 0.5 + 0.5) * 2.0 - 1.0;
        s *= 3.0;
        float3 r = abs(1.0 - 3.0 * abs(a));
        float c = (min(max(r.x, r.y), min(max(r.y, r.z), max(r.z, r.x))) - 1.0) / s;
        d = max(d, c);
    }
    return d * 0.6;   // back to world scale (conservative)
}

float Subject(int which, float3 p, out float mat) {
    float d;
    [branch] if (which == 0) d = Skull(p, mat);
    else if (which == 1) d = Brain(p, mat);
    else d = Monolith(p, mat);
    return d;
}

float Map(float3 p, out float mat) {
    p.xz = mul(Rot(g_yaw), p.xz);
    p.yz = mul(Rot(g_pitch), p.yz);
    p /= g_scale;
    float matA, matB = 0.0;
    float d = Subject(g_subjA, p, matA);
    mat = matA;
    [branch] if (g_morph > 0.001) {
        float b = Subject(g_subjB, p, matB);
        mat = g_morph < 0.5 ? matA : matB;
        d = lerp(d, b, smoothstep(0.0, 1.0, g_morph));
    }
    return d * g_scale;
}

float3 Normal(float3 p) {
    // Tetrahedral taps in a loop (not unrolled), so Map's code isn't copied four times.
    float3 n = 0;
    [loop] for (int i = 0; i < 4; ++i) {
        float3 e = 0.5773 * (2.0 * float3((((i + 3) >> 1) & 1), ((i >> 1) & 1), (i & 1)) - 1.0);
        float m;
        n += e * Map(p + e * 0.002, m);
    }
    return normalize(n);
}

// Data rain: dim falling streaks per cell column, behind the subjects.
float3 Rain(float2 cell) {
    float3 hc = Hash33(int3((int)cell.x, 3, 1));
    float fall = frac(cell.y * 0.02 - time * (0.15 + 0.35 * hc.x) + hc.y);
    return float3(0.1, 0.35, 0.9) * rain * step(0.82, hc.z) * pow(fall, 6.0) * 0.6;
}

// The scene at one ray: lit subject, or data rain behind it. Returns colour for the styles to use.
static bool g_hit = false;   // whether the last Render hit the subject
float3 Render(float2 sp, float2 cell) {
    float3 ro = float3(0, 0.0, 3.4), rd = normalize(float3(sp, -1.9));
    // Bounding sphere: most rays miss, and skip the march.
    float b = dot(ro, rd), c = dot(ro, ro) - 2.6;
    float h = b * b - c;
    float3 col = 0;
    bool hit = false;
    float mat = 0.0, t = 0.0;
    if (h > 0.0) {
        t = max(-b - sqrt(h), 0.0);
        [loop] for (int i = 0; i < 64; ++i) {
            float d = Map(ro + rd * t, mat);
            if (d < 0.002) { hit = true; break; }
            t += d * 0.9;
            if (t > 6.0) break;
        }
    }
    g_hit = hit;
    if (hit) {
        float3 p = ro + rd * t, n = Normal(p);
        // Ambient occlusion: how closed-in this spot is (sockets, nose, between the teeth go black).
        float ao = 0.0, w = 1.0, dummy;
        [loop] for (int k = 1; k <= 4; ++k) {
            float hk = 0.03 * k * k;
            ao += w * (hk - Map(p + n * hk, dummy));
            w *= 0.6;
        }
        ao = saturate(1.0 - ao * 5.0);
        // A hard key light from the upper right, a cool rim from behind on the left.
        float key = saturate(dot(n, normalize(float3(0.75, 0.6, 0.5))));
        float rim = pow(saturate(1.0 + dot(n, rd)), 2.5) * saturate(dot(n, normalize(float3(-0.9, 0.1, -0.3))) + 0.5);
        float bone = (0.03 + 1.1 * pow(key, 1.8)) * ao * ao;
        col = float3(bone, bone * 0.95, bone * 0.88);                // old, yellowed bone
        col += float3(0.15, 0.45, 1.0) * rim * 0.8 * ao;             // cool rim: picks the accent colour
        if (mat > 0.5 && mat < 1.5) col *= 1.1;
        if (mat > 1.5) {
            // Inside the socket: black, with a faint ember deep inside that flares on snares.
            float depth = saturate(1.0 + dot(n, rd) * -1.0);
            col *= 0.08;
            col += float3(1.0, 0.12, 0.08) * g_eyes * 0.9 * smoothstep(0.4, 0.95, depth) * ao;
        }
        } else {
        col = Rain(cell);
    }
    return col;
}

// Grid layouts: square tiles, `rows` to the screen height, scrolling upward, alternate rows staggered.
// Poses this tile's subject (each turns in a wave across the grid; kicks ripple out from the centre)
// and returns the screen position to render it from. SignalVisualizer.cpp poses the model's tiles alike.
float2 TileView(float2 sp, float rows) {
    g_yaw = yaw; g_pitch = pitch; g_scale = 1.0 + 0.03 * pulse; g_eyes = eyes;
    if (rows < 0.5) return sp;
    float T = 2.0 / rows;
    float row = floor((sp.y - scroll) / T);
    float shift = frac(row * 0.5) * T;
    float col = floor((sp.x - shift) / T + 0.5);
    float2 centre = float2(col * T + shift, (row + 0.5) * T + scroll);
    float dist = length(centre) / T;
    g_yaw = yaw + 0.45 * sin(spin * 2.0 - 0.6 * (col + row));
    g_pitch = pitch + 0.08 * sin(spin * 1.7 - 0.5 * row);
    float ripple = beatHit * exp(-pow(dist - beatAge * 9.0, 2.0) * 0.8) * exp(-beatAge * 3.0);
    g_scale = 1.0 + 0.12 * ripple;
    g_eyes = eyes * (0.4 + 0.6 * Hash33(int3(col, row, 7)).x);
    return (sp - centre) / (T * 0.5) * kTileView;
}

// The model's picture at a pixel, averaged over the cell (a mip level) so thin parts don't shimmer.
float3 ModelScene(int layout, float2 px, float lod, float2 cell) {
    float2 uv = px / iResolution;
    float4 s = layout == 0 ? sceneNow.SampleLevel(sceneSampler, uv, lod) : scenePrev.SampleLevel(sceneSampler, uv, lod);
    return s.rgb + Rain(cell) * (1.0 - s.a);
}

// What one cell sees. During a layout change each cell shows the old layout or the new one; while the
// model changes to or from another subject, cells cut between them (the model can't melt like the
// distance fields do).
float3 Scene(float2 sp, float2 px, float2 cell, float lod) {
    int layout = Hash33(int3(cell, 23)).x < layoutMix ? 0 : 1;
    int a = (int)subject, b = (int)nextSubject;
    float m = morph;
    bool model = hasMesh > 0.5;
    if (model && m > 0.001 && (a == 0 || b == 0)) {
        if (Hash33(int3(cell, 29)).x < smoothstep(0.0, 1.0, m)) a = b;
        m = 0.0;
    }
    float3 col;
    [branch] if (model && a == 0) {
        col = ModelScene(layout, px, lod, cell);
    } else {
        g_subjA = a; g_subjB = b; g_morph = m;
        float rows = layout == 0 ? rowsNow : rowsPrev;
        col = Render(TileView(sp, rows), cell);
        if (!g_hit && rows < 0.5) {   // around a subject in the centre: the dim grid behind it
            float3 back = Render(TileView(sp, kBackRows), cell);
            if (g_hit) col = back * kBackDim + col * (1.0 - kBackDim);
        }
    }
    return col;
}

float4 main(VSOut i) : SV_Target {
    int st = (int)style;
    float2 px = i.uv * iResolution;
    float2 size = CyberCellSize(st, iResolution.y) * cellScale;
    float2 centre, local;
    CyberCell(px, size, centre, local);
    float2 cell = floor(px / size);
    float tear = CyberTear(centre.y, time, glitch);
    float2 c = centre + float2(tear, 0.0);
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((c.x / iResolution.x * 2.0 - 1.0) * aspect, 1.0 - c.y / iResolution.y * 2.0);

    CyberPalette pal;
    pal.bg = palBg; pal.mid = palMid; pal.hi = palHi; pal.accent = palAccent;
    // Drops flash the palette inside out for a moment.
    if (flash > 0.5) { float3 tmp = pal.bg; pal.bg = pal.hi * 0.9; pal.hi = tmp; }

    // One sample, or three side by side for the scan style's RGB fringes (a loop, so the scene's code
    // is compiled once).
    float3 samples[3] = { float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0) };
    const int count = st == kStyleScan ? 3 : 1;
    const float offPx = count == 3 ? 3.0 + 10.0 * glitch : 0.0;
    const float lod = max(log2(max(size.x, size.y)) - 0.5, 0.0);
    [loop] for (int k = 0; k < count; ++k) {
        float2 o = float2((k - 1) * offPx, 0.0);
        samples[k] = Scene(sp + o / iResolution.y * 2.0, c + o, cell, lod);
    }
    float3 src = count == 3 ? samples[1] : samples[0];
    // Build-ups dissolve the picture cell by cell.
    if (Hash33(int3(cell, 11)).x < dissolve * 0.7) src *= 0.15;

    float3 col;
    if (st == kStyleAscii) col = CyberAscii(src, local, cell, flicker, time, pal);
    else if (st == kStyleDither) col = CyberDither(src, int2(cell), pal);
    else if (st == kStyleLed) col = CyberLed(src * 1.3, local) + pal.bg * 0.6;
    else col = CyberScan(samples[2], src, samples[0], 0, local, cell, time, glitch) + pal.bg;
    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.6;
    return float4(col, 1);
}
