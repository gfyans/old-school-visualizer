#include "common.hlsli"

// Volumetric nebula: a ray per pixel marches through animated, domain-warped
// noise, accumulating emissive gas front to back. A drifting light core
// illuminates nearby gas, and each gas sample looks a short way towards it to see how
// much gas is in the way (light scattering): clouds get bright rims facing the core,
// dark undersides, and beams of light through the gaps. Events add more light sources:
// a pulsar whose lighthouse beams sweep through the gas in time with the music, and on drops a supernova whose shockwave lights the gas as it passes. Stars show
// through wherever the gas is thin. All audio response is computed on the CPU
// (NebulaVisualizer.cpp) and arrives here as smooth parameters.

cbuffer Nebula : register(b2) {   // matches NebulaConstants in NebulaVisualizer.cpp
    float3 camPos;      float tanHalfFov;
    float3 camRight;    float exposure;
    float3 camUp;       float starBoost;
    float3 camFwd;      float sparkle;
    float3 lightPos;    float lightStrength;
    float3 lightColor;  float shellRadius;
    float3 colDeep;     float shellStrength;
    float3 colMid;      float absorb;
    float3 colHot;      float ambient;
    float threshold;    float gain;  float warp;  float filament;
    float3 warpOffset;  float detail;
    float2 jitter;      float frameIndex;  float scatter;   // TAA: sub-pixel jitter (pixels), frame counter; god-ray strength
    float3 pulsarPos;   float pulsarStrength;
    float3 pulsarDir;   float pulsarWidth;    // one beam points along +dir, the other along -dir
    float3 pulsarCol;   float snFlash;
    float3 snPos;       float snShellRadius;
    float3 snCol;       float snShellStrength;
    float fineRange;    float3 nebulaPad;     // the most the finest octave can move the density field (see Gas)
};

Texture3D<float4> Noise : register(t0);
SamplerState Wrap : register(s0);

static const int kMaxSteps = 56;
static const float kMaxDist = 34.0;

float3 Palette(float t) {
    t = saturate(t);
    float3 lo = lerp(colDeep, colMid, smoothstep(0.0, 0.55, t));
    return lerp(lo, colHot, smoothstep(0.5, 1.0, t));
}

// World space wraps every 64 units (all noise periods divide 64), so the
// camera position can wrap without visible seams.
void Gas(float3 p, out float density, out float dust, out float shade, out float glint) {
    // Two levels of domain warping: broad swirls, then finer turbulence.
    float4 w = Noise.SampleLevel(Wrap, p / 64.0 + warpOffset, 0);
    float3 q = p + (float3(w.x, w.z, w.y) - 0.5) * warp;
    float4 w2 = Noise.SampleLevel(Wrap, q / 16.0 - warpOffset * 2.0, 0);
    q += (float3(w2.z, w2.x, w2.y) - 0.5) * (warp * 0.4);
    float4 a = Noise.SampleLevel(Wrap, q / 32.0, 0);
    float4 b = Noise.SampleLevel(Wrap, q / 8.0, 0);
    float base = lerp(a.x, a.y, filament);
    // Detail octaves plus fine cellular erosion give billowy, crisp edges. The finest octave
    // moves d by at most fineRange: where the gas is solid (or empty) either way, the density
    // doesn't depend on it, so its fetch is skipped (same image, ~6% faster).
    float coarse = base + (b.x - 0.5) * detail - (1.0 - b.w) * 0.09;
    float fineTerm = 0.0;
    [branch] if ((coarse - fineRange - threshold) * gain < 1.0 && (coarse + fineRange - threshold) * gain > 0.0)
        fineTerm = (Noise.SampleLevel(Wrap, q / 4.0, 0).x - 0.5) * detail * 0.5;
    float d = base + (b.x - 0.5) * detail + fineTerm - (1.0 - b.w) * 0.09;
    density = saturate((d - threshold) * gain);
    // Dark dust lanes: absorb light but don't glow.
    dust = saturate((w.y - 0.58) * 6.0) * saturate((b.y - 0.35) * 2.0);
    shade = saturate((a.z - 0.5) * 2.2 + 0.5);
    glint = b.w;
}

// A cheaper density (first warp level and the two main octaves) for the light-scattering
// look-ahead: close enough to say whether gas lies between a point and the light.
float GasLite(float3 p) {
    float4 w = Noise.SampleLevel(Wrap, p / 64.0 + warpOffset, 0);
    float3 q = p + (float3(w.x, w.z, w.y) - 0.5) * warp;
    float4 a = Noise.SampleLevel(Wrap, q / 32.0, 0);
    float4 b = Noise.SampleLevel(Wrap, q / 8.0, 0);
    return saturate((lerp(a.x, a.y, filament) + (b.x - 0.5) * detail - threshold) * gain);
}

float3 Stars(float3 rd) {
    float3 acc = 0;
    [unroll] for (int layer = 0; layer < 2; ++layer) {
        float scale = layer == 0 ? 45.0 : 80.0;
        float3 p = rd * scale + 1000.0;
        int3 cell = (int3)floor(p);
        float3 f = p - floor(p);
        float3 h = Hash33(cell + layer * 777);
        if (h.x > 0.84) {
            float3 starPos = 0.25 + 0.5 * Hash33(cell + 4242);
            float dist = length(f - starPos);
            float size = 0.10 + 0.12 * h.y;
            float b = saturate(1.0 - dist / size);
            b *= b;
            float twinkle = 0.7 + 0.3 * sin(iTime * (1.5 + 4.0 * h.z) + h.y * 60.0);
            float3 tint = lerp(float3(0.65, 0.78, 1.0), float3(1.0, 0.85, 0.65), h.z);
            acc += tint * b * twinkle * (0.6 + 1.4 * h.y * h.y);
        }
    }
    return acc * starBoost;
}

float4 main(VSOut i) : SV_Target {
    float aspect = iResolution.x / iResolution.y;
    // Sub-pixel jitter changes every frame; the TAA resolve averages it into extra detail.
    float2 uv = (i.pos.xy + jitter) / iResolution;
    float2 sp = float2((uv.x * 2.0 - 1.0) * aspect, 1.0 - uv.y * 2.0);
    float3 rd = normalize(camFwd + (sp.x * camRight + sp.y * camUp) * tanHalfFov);

    // Per-pixel start offset hides step banding. It changes every frame so the TAA resolve
    // averages the grain away (rather than freezing it into a fixed pattern).
    float t = 0.15 + 0.2 * InterleavedGradientNoise(i.pos.xy + frameIndex * 5.588238);
    float transmittance = 1.0;
    float3 col = 0;
    float depthSum = 0.0, depthWeight = 0.0;   // opacity-weighted distance, for reprojection

    [loop] for (int s = 0; s < kMaxSteps; ++s) {
        float stepLen = 0.2 + t * 0.03;   // finer steps up close
        float3 p = camPos + rd * t;

        float3 toLight = lightPos - p;
        float dl2 = dot(toLight, toLight);
        float dl = sqrt(dl2);
        float glow = lightStrength / (1.0 + dl2 * 0.12);
        float shell = shellStrength * exp(-abs(dl - shellRadius) * 1.4);

        // Pulsar: two narrow beams from a spinning star, widening slightly with distance.
        float3 pv = p - pulsarPos;
        float along = dot(pv, pulsarDir);
        float perp2 = dot(pv, pv) - along * along;
        float bw = pulsarWidth * (1.0 + abs(along) * 0.08);
        float beam = pulsarStrength * exp(-perp2 / (bw * bw)) * exp(-abs(along) * 0.04);
        float pulsarCore = pulsarStrength * 2.0 / (1.0 + dot(pv, pv) * 6.0);

        // Supernova: a blinding flash, then a shockwave shell that lights the gas as it passes.
        float3 sv = p - snPos;
        float ds = length(sv);
        float snGlow = snFlash * 2.5 / (1.0 + ds * ds * 0.15);
        float snShell = snShellStrength * exp(-abs(ds - snShellRadius) * 1.1);

        // Fade gas very close to the lens: up close the noise is magnified into big soft blobs.
        // Density and dust are each at most 1, so where the fade is this small no sample can pass
        // the threshold below: skip the noise reads (the first few steps of every ray, ~8% faster).
        float fade = smoothstep(1.0, 5.5, t) * smoothstep(kMaxDist, kMaxDist * 0.5, t);
        float density = 0, dust = 0, shade = 0, glint = 0;
        [branch] if (fade * 2.0 > 0.003) {
            Gas(p, density, dust, shade, glint);
            density *= fade;
            dust *= fade;
        }

        [branch] if (density + dust > 0.003) {
            // Light scattering: how much gas lies between this point and the core?
            float lit = 1.0;
            [branch] if (glow > 0.04) {
                float occ = GasLite(p + toLight / dl * 0.9);
                lit = exp(-occ * absorb * 1.6);
            }
            float light = glow * (0.15 + 0.85 * lit);
            // Thin gas sits in the deep colour; dense or lit cores climb to the hot colour.
            float heat = density * 0.75 + (shade - 0.5) * 0.45 + light * 0.18 + shell * 0.3 + snShell * 0.6 + beam * 0.3;
            float3 c = Palette(heat);
            float3 emission = c * (ambient + light + shell * 1.6) * (0.12 + density)
                            + colHot * (pow(saturate(glint), 10.0) * sparkle * (0.5 + glow) * density);
            // Rims: the thin, lit edges of clouds facing the core glow hot.
            emission += colHot * glow * lit * saturate(1.0 - density * 2.5) * density * 0.6;
            // Event light falling on the gas.
            emission += pulsarCol * (beam * 1.3 + pulsarCore) * (0.05 + density);
            emission += snCol * (snGlow + snShell * 1.6) * (0.05 + density);
            emission *= exp(-t * 0.035);   // distant gas recedes into darkness: depth
            float alphaGas = 1.0 - exp(-density * stepLen * absorb);
            float alphaAll = 1.0 - exp(-(density * absorb + dust * 1.5) * stepLen);
            col += transmittance * alphaGas * emission;
            depthSum += transmittance * alphaAll * t;
            depthWeight += transmittance * alphaAll;
            transmittance *= 1.0 - alphaAll;
            if (transmittance < 0.02) break;
        }
        // Light scattered by thin dust between clouds: the core's halo and beams through the
        // gaps (gas in front has already cut the transmittance, which makes the rays).
        col += transmittance * stepLen * (lightColor * glow * glow * 0.012 * scatter
                                        + pulsarCol * (beam * 0.025 + pulsarCore * 0.02)
                                        + snCol * snGlow * 0.012);

        t += stepLen;
        if (t > kMaxDist) break;
    }
    col += transmittance * Stars(rd);

    // Hue-preserving tonemap on the brightest channel: bright gas stays coloured instead of
    // bleaching to white; only the very hottest cores drift towards white.
    float peak = max(col.r, max(col.g, col.b));
    float mapped = 1.0 - exp(-peak * exposure);
    col *= mapped / max(peak, 1e-4);
    col = lerp(col, mapped.xxx, smoothstep(0.8, 1.0, mapped) * 0.2);
    col = pow(saturate(col), 1.0 / 2.2);        // linear -> display
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = saturate(lerp(luma.xxx, col, 1.35));  // vibrance: push colours away from grey

    // Alpha carries where the gas is (opacity-weighted distance); mostly empty pixels count as
    // infinitely far, so they reproject by direction alone (stars, far haze).
    float depth = depthWeight > 0.05 ? depthSum / depthWeight : 1000.0;
    return float4(col, depth);   // bloom and vignette are applied after the TAA resolve
}
