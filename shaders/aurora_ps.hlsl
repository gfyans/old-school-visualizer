#include "common.hlsli"

// Aurora: soft ribbons of light sweeping up the night sky over a frozen forest lake.
//
// Each ribbon is a wavy path in the sky, extruded upwards. Its path swings strongly
// towards and away from the viewer, so the near parts rise high overhead: that's what
// makes the sweeping S-shapes. A ray is followed through a stack of horizontal layers,
// picking up light near each ribbon's path (a soft glow around a brighter core), coloured
// from the low colour to the high colour with altitude. Below: a treeline, tall snowy pines
// on the right, and polished lake ice with white cracks and drifted snow mirroring it all.
//
// The sky turns slowly about the celestial pole like a long-exposure photograph: the stars draw arcs
// round it and the Milky Way wheels across. Drops (and build-ups) bring a corona: rays of light
// streaming up towards the magnetic zenith overhead. Snares send shooting stars.

cbuffer Aurora : register(b2) {   // matches AuroraConstants in AuroraVisualizer.cpp
    float animTime;    float brightness;  float sway;       float heightScale;
    float3 lowCol;     float flare;       // flare: strength of the beat's travelling flare
    float3 highCol;    float flareX;      // flareX: where the flare is along the ribbons
    float3 skyTop;     float stars;
    float3 skyHorizon; float ripple;
    float3 landCol;    float yaw;
    float pitch;       float skyRot;      float corona;     float meteorAge;   // meteorAge < 0: none
    float4 meteorStart;   // xyz where it starts (a direction), w tail length (radians)
    float4 meteorDir;     // xyz its direction of travel (perpendicular to the start), w speed (radians / s)
    float kick;        float trail;       float milky;      float auroraPad0;   // trail: star trail length (radians)
};

// The celestial pole (high in the northern sky, as at aurora latitudes) and the magnetic zenith.
static const float3 kPole = normalize(float3(-0.12, 0.87, 0.48));
static const float3 kZenith = normalize(float3(-0.04, 1.0, 0.22));

static const int kLayers = 32;
static const int kRibbons = 3;
static const float kCamHeight = 0.2;

float Hash1(float n) { return frac(sin(n) * 43758.5453); }
float Noise1(float x) {                       // smooth 1D value noise
    float i = floor(x), f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(Hash1(i), Hash1(i + 1.0), f);
}
float Fbm1(float x) { return 0.5 * Noise1(x) + 0.25 * Noise1(x * 2.1 + 3.1) + 0.125 * Noise1(x * 4.3 + 7.7); }
float Noise2(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = Hash1(dot(i, float2(1.0, 57.0))), b = Hash1(dot(i + float2(1, 0), float2(1.0, 57.0)));
    float c = Hash1(dot(i + float2(0, 1), float2(1.0, 57.0))), d = Hash1(dot(i + float2(1, 1), float2(1.0, 57.0)));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// ---------------------------------------------------------------- aurora

float Ribbon(float2 xz, int k, float y) {
    float kf = (float)k;
    float x = xz.x;
    // A long, sweeping path: big swings towards and away from the viewer (S-curves overhead).
    float centre = 9.0 + kf * 3.5
                 + sway * (sin(x * 0.13 + animTime * 0.09 + kf * 1.9) * 5.5
                         + sin(x * 0.37 - animTime * 0.07 + kf * 2.7) * 2.2)
                 + (Noise1(x * 0.12 + animTime * 0.04 + kf * 11.0) - 0.5) * 2.5;
    centre = max(centre, 2.5);
    float d = xz.y - centre;
    float w = 0.28 + 0.12 * kf;
    float glow = exp(-d * d / (w * w)) + 0.22 * exp(-d * d / (12.0 * w * w));   // soft core and wide glow
    float rays = Noise1(x * 1.6 + animTime * 0.2 + kf * 5.0);
    float shimmer = 0.55 + 0.45 * rays * rays;
    float flareBoost = 1.0 + flare * 2.0 * exp(-pow((x - flareX) / 5.0, 2.0));
    // The lower edge isn't level: it rises and dips in long waves along the ribbon, with finer folds and
    // ray-like ragged bits, so a ribbon passing overhead never ends in a straight line. It's also the
    // brightest part of the curtain, as in real aurora.
    float bottom = 1.65 + 0.95 * Noise1(x * 0.08 + animTime * 0.05 + kf * 4.0)
                 + 0.5 * Noise1(x * 0.55 - animTime * 0.12 + kf * 9.0)
                 + 0.3 * Noise1(x * 2.6 + animTime * 0.3 + kf * 2.0);
    float above = y - bottom;
    float edge = smoothstep(-0.25, 0.75, above) * (1.0 + 0.7 * exp(-max(above, 0.0) * 1.8));
    // The upper edge too: the curtain's height swells and sinks along it, with tall rays shooting up
    // here and there, and fades out softly above that.
    float tall = (2.6 + 2.8 * Noise1(x * 0.06 - animTime * 0.04 + kf * 6.0)
                + 1.1 * Noise1(x * 0.42 + animTime * 0.1 + kf * 3.0)
                + 1.6 * pow(Noise1(x * 1.9 - animTime * 0.25 + kf * 8.0), 3.0)) * heightScale;
    float top = smoothstep(tall + 0.6, tall - 1.6, above);
    return glow * shimmer * flareBoost * edge * top * (1.0 - 0.3 * kf);
}

float3 Aurora(float3 rd, float jitter) {
    float3 col = 0;
    if (rd.y <= 0.01) return col;
    float yBase = 1.4, yRange = 7.9 * heightScale;   // (each ribbon's own lower edge is higher and uneven)
    [loop] for (int i = 0; i < kLayers; ++i) {
        float hf = (i + jitter) / kLayers;                    // 0 bottom .. 1 top of the ribbons
        float y = yBase + hf * hf * yRange;                   // layers spread out higher up
        float t = (y - kCamHeight) / rd.y;
        float2 xz = rd.xz * t;
        float light = 0.0;
        [unroll] for (int k = 0; k < kRibbons; ++k) light += Ribbon(xz, k, y);
        float profile = exp(-hf * 0.9 / heightScale);         // a gentle fade upwards (each ribbon's top is its own)
        float3 c = lerp(lowCol, highCol, smoothstep(0.25, 1.0, hf));
        c += lowCol * 0.5 * exp(-hf * 6.0);                   // brighter lower part
        c += lowCol * kick * 0.6 * exp(-hf * 9.0);            // kicks light up the lower edge
        col += c * light * profile * (0.6 + 1.4 * hf) / (1.0 + t * 0.02);   // weight: upper layers are spread thinner
    }
    return col * brightness * (1.0 + 0.35 * kick) * (9.5 / kLayers);
}

// Turns v about the axis k by angle a (Rodrigues).
float3 Rotate(float3 v, float3 k, float a) {
    float c = cos(a), sn = sin(a);
    return v * c + cross(k, v) * sn + k * dot(k, v) * (1.0 - c);
}

// The stars as long-exposure trails round the pole. Stars live on rings of the polar grid (distance from
// the pole); each ring is cut into cells one trail long, so a pixel only checks two cells for a trail
// ending ahead of it. Inside the Milky Way there are more of them.
float3 StarTrails(float3 rd, float bandMask) {
    float3 A = normalize(cross(float3(0, 1, 0), kPole)), B = cross(kPole, A);
    float rho = acos(clamp(dot(rd, kPole), -1.0, 1.0));
    float phi = atan2(dot(rd, B), dot(rd, A)) - skyRot;      // angle in the stars' own frame
    const float dRho = 0.0055;
    float ring = floor(rho / dRho);
    float cells = max(1.0, floor(6.2831853 / max(trail, 0.02)));
    float cellA = 6.2831853 / cells;
    float c0 = floor(phi / cellA);
    float w = 1.3 / iResolution.y;                          // about a pixel (the view spans 1.6 radians-ish of tangent)
    float3 acc = 0;
    [unroll] for (int k = 0; k < 2; ++k) {
        float cell = c0 + k;
        float cid = cell - cells * floor(cell / cells);        // wrap round the ring
        [unroll] for (int j = 0; j < 2; ++j) {
            float3 h = Hash33(int3((int)ring, (int)cid, 61 + j));
            float chance = saturate(rho * 0.22) * (0.5 + 1.6 * bandMask) * 0.8;   // (20% fewer)
            if (h.x > chance) continue;
            float phi0 = (cell + h.y) * cellA;                 // where the star is now (the head of its trail)
            float rho0 = (ring + 0.2 + 0.6 * h.z) * dRho;
            float along = phi0 - phi;                          // 0 at the head .. trail at the tail
            float len = trail * (0.75 + 0.25 * h.y);
            if (along < 0.0 || along > len) continue;
            float onTrail = exp(-pow((rho - rho0) / w, 2.0));
            float bright = (0.25 + 0.75 * h.z * h.z) * (0.45 + 0.55 * (1.0 - along / len)) * smoothstep(0.0, 0.02, along + 0.004);
            float3 tint = lerp(float3(1.0, 0.8, 0.6), float3(0.7, 0.85, 1.0), frac(h.z * 7.3));   // warm and blue stars
            acc += tint * onTrail * bright;
        }
    }
    return acc * (0.4 + 0.3 * stars);
}

// The Milky Way: a band round the sky (fixed in the stars' frame, so it wheels with them), brighter and
// warmer at its core, broken by dark dust lanes.
float3 MilkyWay(float3 rd, out float bandMask) {
    float3 q = Rotate(rd, kPole, -skyRot);
    const float3 n = normalize(float3(0.78, 0.3, -0.55));
    float lat = dot(q, n);
    float band = exp(-pow(lat / 0.17, 2.0));
    float core = exp(-pow(lat / 0.065, 2.0));
    bandMask = band;
    float3 p = q * 7.0;
    float clouds = 0.5 * Noise2(p.xy) + 0.5 * Noise2(p.yz + 3.7);
    clouds = 0.6 * clouds + 0.4 * (0.5 * Noise2(p.zx * 2.3 + 1.1) + 0.5 * Noise2(p.xy * 2.7 + 5.3));
    float rift = smoothstep(0.42, 0.7, 0.5 * Noise2(p.xz * 3.1 + 9.0) + 0.5 * Noise2(p.yx * 5.0 + 2.0)) * smoothstep(0.03, 0.0, abs(lat - 0.01));
    float3 col = lerp(float3(0.5, 0.58, 0.85), float3(1.0, 0.86, 0.66), core);
    float light = (band * 0.1 + core * 0.15) * (0.45 + clouds) * (1.0 - 0.8 * rift);
    // A fine dust of unresolved stars.
    float2 g = (q.xy + q.z * float2(0.37, 0.61)) * 900.0;
    float3 hs = Hash33(int3(floor(g), 17));
    light += band * step(0.985, hs.x) * smoothstep(0.35, 0.0, length(frac(g) - 0.5)) * 0.25;
    return col * light * milky;
}

// The corona: rays of aurora streaming up towards the magnetic zenith, flickering along their length.
float3 Corona(float3 rd) {
    if (corona < 0.01 || rd.y < 0.02) return 0;
    float3 A = normalize(cross(float3(0, 0, 1), kZenith)), B = cross(kZenith, A);
    float d = acos(clamp(dot(rd, kZenith), -1.0, 1.0));       // angle from the zenith point
    float a = atan2(dot(rd, B), dot(rd, A));
    float r1 = Noise1(a * 28.0 + animTime * 0.5), r2 = Noise1(a * 71.0 - animTime * 0.9 + 3.0);
    float rays = pow(r1, 4.0) + 0.6 * pow(r2, 6.0);
    float flick = 0.7 + 0.3 * Noise1(d * 12.0 - animTime * 3.0 + a * 5.0);
    float fall = smoothstep(1.35, 0.35, d) * smoothstep(0.03, 0.25, d) * smoothstep(0.02, 0.3, rd.y);
    float3 c = lerp(lowCol, highCol, saturate(1.2 - d));
    return c * rays * flick * fall * corona * 1.3;
}

// A shooting star: a hair-thin streak along a great circle, fading towards its tail, with a head that
// flickers and sputters as it burns up (and sheds the odd spark just behind it).
float3 Meteor(float3 rd) {
    if (meteorAge < 0.0) return 0;
    float3 S = meteorStart.xyz, T = meteorDir.xyz, n = cross(S, T);
    float px = 1.6 / iResolution.y;                          // about one pixel, in radians
    float dist = abs(asin(clamp(dot(rd, n), -1.0, 1.0)));
    float along = atan2(dot(rd, T), dot(rd, S));
    float head = meteorDir.w * meteorAge, tail = head - meteorStart.w;
    if (along < tail - 0.01 || along > head + 0.03) return 0;
    float life = saturate(1.0 - meteorAge / 0.9);
    // Burning: a fast, ragged flicker.
    float burn = Noise1(meteorAge * 55.0) * (0.55 + 0.45 * Noise1(meteorAge * 143.0 + 7.0));
    burn = 0.35 + 1.1 * burn * burn;
    // The trail: thin, brightest just behind the head.
    float f = saturate((along - tail) / meteorStart.w) * step(along, head);
    float w = px * (0.45 + 0.35 * f);
    float3 col = float3(0.8, 0.92, 1.0) * exp(-pow(dist / w, 2.0)) * f * f * (0.7 + 0.3 * burn);
    // The head: a small glowing point, swelling and dimming as it burns, white-hot with a green-gold fringe.
    float3 H = S * cos(head) + T * sin(head);
    float dh = acos(clamp(dot(rd, H), -1.0, 1.0));
    float r = px * (0.8 + 1.4 * burn);
    col += float3(1.0, 0.97, 0.9) * exp(-pow(dh / r, 2.0)) * burn * 1.6;
    col += float3(0.6, 1.0, 0.5) * exp(-dh / (r * 3.0)) * burn * 0.25;
    // Sparks breaking off just behind the head.
    float k = floor((head - along) / (px * 6.0));
    float3 h = Hash33(int3((int)k, (int)(meteorAge * 30.0), 91));
    float spark = step(0.8, h.x) * step(0.0, head - along) * step(head - along, px * 60.0);
    col += float3(1.0, 0.85, 0.6) * spark * exp(-pow(dist / (px * 0.6), 2.0)) * (1.0 - (head - along) / (px * 60.0));
    return col * life * 2.2;
}

// ---------------------------------------------------------------- the forest

// A low, dense band of distant conifers right along the horizon.
float FarTreeline(float az) {
    float base = 0.008 + 0.010 * Fbm1(az * 9.0);
    float cellW = 0.0055;
    float cid = floor(az / cellW);
    float fa = frac(az / cellW);
    float h = (0.008 + 0.022 * Hash1(cid * 3.1)) * (0.55 + 0.45 * Fbm1(az * 3.0));
    return base + h * saturate(1.0 - abs(fa - 0.5) * 2.0 / 0.8);
}

// Tall pines on the right: tiered silhouettes, snow resting on the branches.
// Returns 1 inside a tree; snow = how much of this pixel is snow on a branch.
float NearPines(float az, float e, out float snow) {
    snow = 0.0;
    float side = smoothstep(0.12, 0.32, az);
    if (side <= 0.0) return 0.0;
    float cellW = 0.05;
    float cid0 = floor(az / cellW);
    float inside = 0.0;
    [unroll] for (int n = -1; n <= 1; ++n) {   // neighbouring trees overlap
        float cid = cid0 + n;
        float3 h3 = Hash33(int3((int)cid, 77, 3));
        if (h3.x < 0.2) continue;
        float height = (0.10 + 0.30 * h3.y) * side;
        float trunkAz = (cid + 0.3 + 0.4 * h3.z) * cellW;
        float y = e / height;                                   // 0 at the base .. 1 at the tip
        if (y < 0.0 || y > 1.0) continue;
        float tierF = frac(y * (7.0 + 3.0 * h3.z));
        float halfW = height * 0.24 * (1.0 - y) * (0.6 + 0.4 * (1.0 - tierF));   // tiers flare at the bottom
        halfW *= 0.85 + 0.3 * Noise1(e * 400.0 + cid * 13.0);  // ragged branch tips
        float dx = abs(az - trunkAz);
        if (dx < halfW || (dx < 0.0018 && y < 0.1)) {
            inside = 1.0;
            // Snow sits on top of each tier, thickest towards the outer branches.
            snow = max(snow, smoothstep(0.55, 0.0, tierF) * smoothstep(0.2, 0.9, dx / max(halfW, 1e-4)) * 0.8);
        }
    }
    return inside;
}

// Everything above the ice: sky, stars, aurora, treeline and pines.
float3 Scene(float3 rd, float jitter) {
    float3 sky = lerp(skyHorizon, skyTop, saturate(rd.y * 1.6));
    sky += lowCol * brightness * 0.08 * exp(-max(rd.y, 0.0) * 7.0);   // airglow low in the sky
    float bandMask;
    float3 milkyWay = MilkyWay(rd, bandMask);
    float above = saturate(rd.y * 8.0);
    sky += (StarTrails(rd, bandMask) + milkyWay) * above * saturate(1.05 - brightness * 0.45);   // the aurora outshines them
    sky += Aurora(rd, jitter) + Corona(rd) + Meteor(rd) * above;
    float az = atan2(rd.x, rd.z);
    float glow = dot(lowCol, float3(0.3, 0.6, 0.1)) * brightness;     // the aurora lights the snow
    if (rd.y < FarTreeline(az)) sky = landCol * 0.8;
    float snow;
    if (NearPines(az, rd.y, snow) > 0.0)
        sky = landCol * 0.5 + (float3(0.5, 0.6, 0.7) * 0.05 + lowCol * 0.12 * glow) * snow;
    return sky;
}

// ---------------------------------------------------------------- the frozen lake

// Distance to the nearest crack (cell border of a jittered grid): small on the cracks.
float CrackDistance(float2 p) {
    float2 i = floor(p), f = frac(p);
    float d1 = 9.0, d2 = 9.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float2 o = float2(x, y);
        float2 c = o + Hash33(int3(i + o, 21)).xy - f;
        float d = dot(c, c);
        if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) { d2 = d; }
    }
    return sqrt(d2) - sqrt(d1);
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    float2 sp = float2((i.uv.x * 2.0 - 1.0) * aspect, 1.0 - i.uv.y * 2.0);
    float cy = cos(yaw), syw = sin(yaw), cp = cos(pitch), spch = sin(pitch);
    float3 fwd = float3(syw * cp, spch, cy * cp);
    float3 right = float3(cy, 0.0, -syw);
    float3 up = cross(fwd, right);
    float3 rd = normalize(fwd + (sp.x * right + sp.y * up) * 0.8);
    float jitter = InterleavedGradientNoise(i.pos.xy);

    float3 col;
    if (rd.y >= 0.0) {
        col = Scene(rd, jitter);
    } else {
        float t = kCamHeight / -rd.y;
        float2 w = rd.xz * t;                                   // position on the ice
        float az = atan2(rd.x, rd.z);
        float glow = dot(lowCol, float3(0.3, 0.6, 0.1)) * brightness;

        // Drifted snow: streaky patches, more of them towards the shore on the right.
        float drift = Noise2(float2(w.x * 0.35, w.y * 1.4)) * 0.6 + Noise2(w * 1.3) * 0.4;
        float snowMask = smoothstep(0.48, 0.62, drift + 0.15 * smoothstep(0.1, 0.5, az));
        // Cracks: fine white lines in the ice, thinner with distance.
        float crackW = 0.03 + t * 0.004;
        float crack = smoothstep(crackW, 0.0, CrackDistance(w * 0.55)) * (1.0 - snowMask);
        crack += 0.5 * smoothstep(crackW * 0.7, 0.0, CrackDistance(w * 1.7 + 13.0)) * (1.0 - snowMask);

        // Polished ice mirrors the scene; cracks and gentle unevenness (more with bass) distort it.
        float2 bump = float2(Noise2(w * 2.0 + animTime * 0.05) - 0.5, Noise2(w * 2.0 + 7.0) - 0.5);
        float3 rr = normalize(float3(rd.x + bump.x * (0.01 + ripple * 0.012), -rd.y + bump.y * (0.008 + ripple * 0.01), rd.z));
        float3 reflection = Scene(rr, jitter);
        float fresnel = 0.45 + 0.55 * pow(1.0 - saturate(-rd.y * 2.5), 3.0);
        float3 ice = reflection * 0.85 * fresnel + float3(0.01, 0.03, 0.04);

        float3 snowCol = float3(0.5, 0.6, 0.7) * 0.06 + lowCol * 0.16 * glow + skyHorizon * 0.5;
        col = lerp(ice, snowCol, snowMask);
        col += (float3(0.6, 0.7, 0.8) * 0.06 + lowCol * 0.08 * glow) * saturate(crack);
        // Snowy bank along the shore under the pines.
        float bank = smoothstep(0.1, 0.35, az) * smoothstep(-0.03, -0.004, rd.y);
        col = lerp(col, snowCol * 1.3, bank);
    }

    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.5;
    col = 1.0 - exp(-col * 1.3);
    return float4(col, 1);
}
