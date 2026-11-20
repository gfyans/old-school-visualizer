#include "common.hlsli"

// Aurora: soft ribbons of light sweeping up the night sky over a frozen forest lake.
//
// Each ribbon is a wavy path in the sky, extruded upwards. Its path swings strongly
// towards and away from the viewer, so the near parts rise high overhead: that's what
// makes the sweeping S-shapes. A ray is followed through a stack of horizontal layers,
// picking up light near each ribbon's path (a soft glow around a brighter core), coloured
// from the low colour to the high colour with altitude. Below: a treeline, tall snowy pines
// on the right, and polished lake ice with white cracks and drifted snow mirroring it all.

cbuffer Aurora : register(b2) {   // matches AuroraConstants in AuroraVisualizer.cpp
    float animTime;    float brightness;  float sway;       float heightScale;
    float3 lowCol;     float flare;       // flare: strength of the beat's travelling flare
    float3 highCol;    float flareX;      // flareX: where the flare is along the ribbons
    float3 skyTop;     float stars;
    float3 skyHorizon; float ripple;
    float3 landCol;    float yaw;
    float pitch;       float auroraPad0;  float auroraPad1; float auroraPad2;
};

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

float Ribbon(float2 xz, int k) {
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
    return glow * shimmer * flareBoost * (1.0 - 0.3 * kf);
}

float3 Aurora(float3 rd, float jitter) {
    float3 col = 0;
    if (rd.y <= 0.01) return col;
    float yBase = 1.8, yRange = 7.5 * heightScale;
    [loop] for (int i = 0; i < kLayers; ++i) {
        float hf = (i + jitter) / kLayers;                    // 0 bottom .. 1 top of the ribbons
        float y = yBase + hf * hf * yRange;                   // layers spread out higher up
        float t = (y - kCamHeight) / rd.y;
        float2 xz = rd.xz * t;
        float light = 0.0;
        [unroll] for (int k = 0; k < kRibbons; ++k) light += Ribbon(xz, k);
        float profile = smoothstep(0.0, 0.18, hf) * exp(-hf * 1.6 / heightScale);   // soft bottom, long fade upwards
        float3 c = lerp(lowCol, highCol, smoothstep(0.25, 1.0, hf));
        c += lowCol * 0.5 * exp(-hf * 6.0);                   // brighter lower part
        col += c * light * profile * (0.6 + 1.4 * hf) / (1.0 + t * 0.02);   // weight: upper layers are spread thinner
    }
    return col * brightness * (4.2 / kLayers);
}

float3 Stars(float3 rd) {
    float2 q = rd.xy / max(rd.z, 0.2) * 110.0;
    int3 cell = int3(floor(q), 5);
    float3 h = Hash33(cell + 3000);
    float d = length(frac(q) - (0.2 + 0.6 * h.yz));
    float tw = 0.6 + 0.4 * sin(animTime * (1.5 + 4.0 * h.y) * (0.5 + stars) + h.z * 50.0);
    float3 acc = step(0.92, h.x) * smoothstep(0.13, 0.0, d) * tw * (0.4 + 0.8 * h.y * h.y);
    float band = exp(-pow((rd.y - 0.5 + rd.x * 0.35) / 0.14, 2.0));   // a faint Milky Way
    acc += float3(0.55, 0.6, 0.75) * band * (0.03 + 0.04 * Fbm1(rd.x * 14.0 + rd.y * 9.0));
    return acc;
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
    sky += Aurora(rd, jitter) + Stars(rd) * saturate(rd.y * 8.0);
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
