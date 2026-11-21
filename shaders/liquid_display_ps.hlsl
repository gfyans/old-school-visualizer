#include "liquid.hlsli"

// Liquid: shows the dye (t0, mipmapped) in one of three looks.
//   0 Ink    - pigment billowing in backlit water: the dye absorbs the light, so it reads as coloured ink
//   1 Neon   - the dye glows on black, with bloom
//   2 Chrome - the dye's density is the surface of liquid metal, tinted by its colour, lit by a studio
SamplerState mipSampler : register(s1);

float3 Dye(float2 uv, float lod) { return tex0.SampleLevel(mipSampler, uv, lod).rgb; }
float Amount(float3 d) { return max(d.r, max(d.g, d.b)); }

float3 InkLook(float2 uv) {
    float2 v = (uv - 0.5) * float2(aspect, 1.0);
    // A warm backlight, brightest in the middle, swelling a little with the kicks.
    float3 bg = float3(0.82, 0.78, 0.72) * (1.0 - 0.45 * dot(v, v)) * (0.85 + 0.12 * kick + 0.25 * flash);
    bg = lerp(bg, bg * (0.7 + 0.3 * tint), 0.35);
    float3 d = Dye(uv, 0);
    float a = Amount(d);
    float3 pigment = d / max(a, 1e-3);
    // The ink absorbs the colours it isn't, and goes dark where it's thick.
    float3 col = bg * exp(-a * 3.4 * (1.15 - pigment)) * exp(-a * 0.5);
    // Light catching the soft edges of the plumes.
    float edge = saturate(Amount(Dye(uv, 3.0)) - a);
    col += bg * edge * 0.25;
    return col;
}

float3 NeonLook(float2 uv) {
    float3 d = Dye(uv, 0);
    float3 bloom = Dye(uv, 2.0) * 0.5 + Dye(uv, 4.0) * 0.35;
    float3 col = d * (0.9 + 0.3 * kick) + bloom * (0.6 + 0.4 * energy);
    col += tint * 0.015;
    col = 1.0 - exp(-col * 1.2);
    return col + flash * 0.12;
}

float3 ChromeLook(float2 uv) {
    // The whole screen is a sheet of liquid metal; the ink's density raises it into ridges and swells
    // (read from a blurred mip, so the surface is smooth rather than grainy).
    float2 t = dyeTexel * 4.0;
    float hL = Amount(Dye(uv - float2(t.x, 0), 2.0)), hR = Amount(Dye(uv + float2(t.x, 0), 2.0));
    float hU = Amount(Dye(uv - float2(0, t.y), 2.0)), hD = Amount(Dye(uv + float2(0, t.y), 2.0));
    float3 d = Dye(uv, 1.5);
    float h = Amount(d);
    float3 n = normalize(float3(-(hR - hL) * 5.0, (hD - hU) * 5.0, 1.0));   // screen space, y up
    float3 r = reflect(float3(0, 0, -1), n);
    // What the metal reflects: a dim studio seen at a slight angle (so the flat sheet shades across the
    // screen), a big soft key light and a hard strip light.
    float2 e = r.xy + (uv - 0.5) * float2(0.7, -0.5);
    float3 env = lerp(float3(0.05, 0.05, 0.06), float3(0.32, 0.32, 0.36), saturate(0.55 + e.y * 0.9));
    env += 0.06 * tint;
    env += 1.4 * pow(saturate(1.0 - length(e - float2(-0.35, 0.45)) * 1.6), 3.0);
    env += 0.6 * smoothstep(0.12, 0.0, abs(e.x - 0.42 - 0.1 * e.y)) * saturate(1.0 - abs(e.y) * 1.5) * (1.0 + 0.6 * kick);
    float3 pigment = d / max(h, 1e-3);
    float3 metal = lerp(0.9.xxx, pigment, saturate(h * 1.5) * 0.65);   // tinted where the ink is
    float fres = pow(1.0 - n.z, 1.5);
    float3 col = env * metal * (0.8 + 2.0 * fres);
    col = 1.0 - exp(-col * 1.3);
    return col + flash * 0.1;
}

float4 main(VSOut i) : SV_Target {
    int lk = (int)look;
    float3 col = lk == 0 ? InkLook(i.uv) : lk == 1 ? NeonLook(i.uv) : ChromeLook(i.uv);
    // Dither in proportion to brightness: the host's target keeps only ~6 bits of mantissa, which would
    // otherwise band the smooth gradients into steps.
    float noise = InterleavedGradientNoise(i.pos.xy + frac(time * 7.0) * 64.0) - 0.5;
    col *= 1.0 + noise * 0.035;
    col += noise / 255.0;
    return float4(col, 1.0);
}
