#include "galaxy.hlsli"

// Galaxy output. The particles give sparkle, clusters and knots, but a real galaxy is mostly
// the smooth light of countless unresolved stars, so that is drawn here analytically: a glowing
// bulge and an exponential disc lit up along the same spiral arms as the particles, crossed by
// filamentary dust lanes. Everything is seen through the black hole's gravity (lensing bends
// light around it into an Einstein ring), with bloom from the particle image's mips, distant
// stars and a filmic tone curve.

Texture2D<float4> Stars : register(t0);   // accumulated particles, with mips
SamplerState Linear : register(s0);

float Hash1(float n) { return frac(sin(n) * 43758.5453); }
float Noise2(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float n = dot(i, float2(1.0, 57.0));
    return lerp(lerp(Hash1(n), Hash1(n + 1.0), f.x), lerp(Hash1(n + 57.0), Hash1(n + 58.0), f.x), f.y);
}

// Mips are box-filtered: a bright spot turns into a visible square. Four bilinear taps
// half a texel apart smooth that into a soft tent.
float3 SoftMip(float2 uv, int m) {
    float2 size;
    Stars.GetDimensions(size.x, size.y);
    float2 o = 0.5 * exp2(m) / size;
    return (Stars.SampleLevel(Linear, uv + float2(o.x, o.y), m) + Stars.SampleLevel(Linear, uv + float2(-o.x, o.y), m) +
            Stars.SampleLevel(Linear, uv + float2(o.x, -o.y), m) + Stars.SampleLevel(Linear, uv - o, m)).rgb * 0.25;
}

float3 Background(float3 rd) {
    float2 q = float2(atan2(rd.z, rd.x), asin(clamp(rd.y, -1.0, 1.0))) * 160.0;
    float3 h = Hash33(int3(floor(q), 17));
    float d = length(frac(q) - (0.2 + 0.6 * h.yz));
    float star = step(0.975, h.x) * smoothstep(0.12, 0.0, d) * (0.3 + 0.7 * h.y);
    float3 c = lerp(float3(0.7, 0.8, 1.0), float3(1.0, 0.85, 0.7), h.z) * star * 1.4;
    return c + skyCol * (0.6 + 0.4 * Noise2(rd.xy * 3.0 + rd.z));   // a faint, uneven sky glow
}

// Where a disc point sits relative to the arms of one shape: x = arm brightness,
// y = dust lane darkness (a lane trails just inside each arm), z = position along the arm.
float3 ArmField(float r, float phi, float4 s) {
    float u = (phi - pattern - s.y * log(max(r, 0.02))) * s.x / kTau;
    float off = (frac(u + 0.5) - 0.5) * kTau / s.x;      // angle from the nearest arm's centre
    float w = ArmWidth(s) * (0.5 + 0.7 * r);
    float arm = exp(-off * off / (w * w * 0.5));
    float lane = exp(-pow((off + 0.4 * w) / (0.24 * w), 2.0)) + 0.45 * exp(-pow((off - 0.75 * w) / (0.2 * w), 2.0));
    return float3(arm, lane, u);
}

// The smooth light of the galaxy along a view ray; dust = how much the dust lanes dim it.
float3 GalaxyLight(float3 rd, out float dust) {
    dust = 0.0;
    float3 light = 0;

    // Bulge: a soft, bright ellipsoid of old stars around the centre (closest approach of the ray).
    float along = dot(-camPos, rd);
    if (along > 0.0) {
        float3 c = camPos + rd * along;
        c.y *= 1.6;                                        // a little flattened
        float d = length(c);
        float bs = lerp(shapeA.w, shapeB.w, shapeBlend);
        light += coreCol * (exp(-d * d / (bs * bs * 0.10)) * 2.2 + exp(-d / (bs * 0.10)) * 1.4 + exp(-d / (bs * 0.5)) * 0.25);
    }

    // Disc: where the ray crosses the galaxy plane.
    if (camPos.y * rd.y < 0.0) {
        float t = -camPos.y / rd.y;
        float3 p = camPos + rd * t;
        float r = length(p.xz) / (1.0 - 0.07 * gravity);
        float phi = atan2(p.z, p.x);
        float3 fa = ArmField(r, phi, shapeA), fb = ArmField(r, phi, shapeB);
        float arm = lerp(fa.x, fb.x, shapeBlend);
        float lane = lerp(fa.y, fb.y, shapeBlend);
        float u = lerp(fa.z, fb.z, shapeBlend);
        float lr = log(max(r, 0.02));
        float fil = Noise2(float2(u * 7.0, lr * 16.0)) * 0.6 + Noise2(float2(u * 19.0, lr * 41.0)) * 0.4;   // ragged, filamentary
        float clumps = Noise2(float2(u * 11.0, lr * 23.0) + 5.0);

        float profile = exp(-r / 0.26) * smoothstep(1.15, 0.65, r);
        float slab = clamp(0.35 / abs(rd.y), 1.0, 5.0);   // a thin slab looks brighter seen at a slant
        float outer = smoothstep(0.12, 0.75, r);
        float3 c = lerp(coreCol, lerp(coreCol, armCol, 0.65), outer);   // warm inside, blue-violet outside
        float armLight = arm * (0.25 + 1.5 * clumps * clumps) * (0.4 + 0.8 * outer);
        float3 disc = (c * (0.45 + 1.3 * armLight) + armCol * armLight * outer * 0.9) * profile;

        // Beat shockwaves light up as they race out.
        float push = 0.0;
        [unroll] for (int w = 0; w < 4; ++w) push += waves[w].y * exp(-pow((r - waves[w].x) / 0.07, 2.0));
        disc *= 1.0 + 1.6 * push;

        light += disc * slab * 1.6;
        dust = saturate(lane * smoothstep(0.25, 0.7, fil)) * smoothstep(0.08, 0.22, r) * smoothstep(1.05, 0.6, r);
    }
    return light;
}

float SegmentDistance(float2 p, float2 a, float2 b, out float along) {
    float2 ab = b - a;
    along = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-8));
    return length(p - a - ab * along);
}

// The quasar: twin beams of light shooting out of the poles (a white-hot core in a coloured
// halo, with plasma knots racing outwards), a glare around the black hole and an anamorphic
// streak across the screen.
float3 Quasar(float2 p, float2 pb, float r) {
    if (quasar <= 0.01) return 0;
    float3 core = float3(0.85, 0.92, 1.0), halo = lerp(armCol, knotCol, 0.3);
    float3 col = 0;
    [unroll] for (int s = 0; s < 2; ++s) {
        float along;
        float d = SegmentDistance(p, bhPos, s == 0 ? jetTipA : jetTipB, along);
        float wc = beamWidth * (0.25 + 1.2 * along);              // the beam widens as it goes
        float fade = pow(1.0 - along, 0.8);
        float knots = 0.6 + 0.4 * sin(along * 38.0 - animTime * 22.0);
        col += core * exp(-d * d / (wc * wc)) * 2.5 * fade * knots;
        col += halo * exp(-d / (wc * 4.0)) * 0.7 * fade;
    }
    col += core * (exp(-r / (beamWidth * 3.0)) * 3.0 + exp(-r / 0.08) * 0.4);                   // glare
    col += lerp(core, halo, 0.5) * exp(-abs(pb.y) / 0.004) * exp(-abs(pb.x) / 0.6) * 1.2;       // streak
    float strength = quasar * quasar;
    return col * strength * (1.0 + 0.3 * kick);
}

float4 main(VSOut i) : SV_Target {
    float2 p = (i.uv - 0.5) * float2(aspect, 1.0);   // screen height units, y down
    float2 pb = p - bhPos;
    float r = length(pb);

    // Gravitational lensing: light passing near the hole is bent towards it. Inside the
    // Einstein radius we see the far side, flipped.
    float rE = horizon * 2.4;
    float2 q = bhPos + pb * (1.0 - rE * rE / max(r * r, 1e-8));
    float2 uv = q / float2(aspect, 1.0) + 0.5;
    float3 rd = normalize(camFwd + (camRight * (2.0 * q.x) - camUp * (2.0 * q.y)) / focal);

    float dust;
    float3 col = GalaxyLight(rd, dust) * glow;
    col += max(Stars.SampleLevel(Linear, uv, 0).rgb, 0.0);
    float3 bloom = Stars.SampleLevel(Linear, uv, 1).rgb * 0.30 + Stars.SampleLevel(Linear, uv, 2).rgb * 0.25;
    [unroll] for (int m = 3; m <= 7; ++m) bloom += SoftMip(uv, m) * 0.12;
    col += max(bloom, 0.0) * (0.45 + 0.3 * flash);
    col *= 1.0 - 0.85 * dust * float3(0.7, 0.85, 1.0);     // dust lanes: dark, a little brown
    col += Background(rd);

    // The horizon: black, with a thin, hot photon ring just outside.
    col *= smoothstep(horizon, horizon * 1.12, r);
    float ring = exp(-pow((r - horizon * 1.3) / (horizon * 0.12), 2.0));
    col += lerp(discCol, float3(1.0, 0.95, 0.85), 0.5) * ring * (0.4 + 0.8 * accretion);
    col += Quasar(p, pb, r);

    float2 v = i.uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.5;
    col = 1.0 - exp(-col * exposure);
    col += (InterleavedGradientNoise(i.pos.xy) - 0.5) / 255.0;   // dither the dark sky
    return float4(col, 1);
}
