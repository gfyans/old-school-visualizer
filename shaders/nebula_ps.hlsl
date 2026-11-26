#include "common.hlsli"

// Volumetric nebula: a ray per pixel marches through animated, domain-warped
// noise, accumulating emissive gas front to back. A drifting light core
// illuminates nearby gas, and each gas sample looks a short way towards it to see how
// much gas is in the way (light scattering): clouds get bright rims facing the core,
// dark undersides, and beams of light through the gaps. Events add more light sources:
//   pulsar     a spinning star whose lighthouse beams sweep through the gas in time with the
//              music, leaving a fading wake of lit gas behind each sweep
//   supernova  during a build-up a star ahead brightens and tightens; on the drop it blows:
//              a flash, then a ragged shockwave shell (visible even in empty space) that lights
//              the gas and shoves it outward as it passes, leaving a small remnant star
// Stars show through wherever the gas is thin. All audio response is computed on the CPU
// (NebulaVisualizer.cpp) and arrives here as smooth parameters.
//
// Compiled twice: as the raymarch, and with NEB_VISIBILITY as a one-pixel pass that measures
// how much gas lies between the camera and the pulsar / supernova (for their lens flares).

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
    float fineRange;    float snStar;  float snPush;  float pulsarStar;   // fineRange: see Gas; snStar: progenitor / remnant brightness
    float3 pulsarAxis;  float pulsarWake;     // spin axis (beams sweep the plane across it); wake brightness
    float3 snStarCol;   float snStarSize;     // progenitor / remnant colour, and its halo's angular size
};

Texture3D<float4> Noise : register(t0);
SamplerState Wrap : register(s0);

static const int kMaxSteps = 56;
static const float kMaxDist = 34.0;
static const float3 kShockBlue = float3(0.70, 0.86, 1.0);   // the shockwave's leading edge

float3 Palette(float t) {
    t = saturate(t);
    float3 lo = lerp(colDeep, colMid, smoothstep(0.0, 0.55, t));
    return lerp(lo, colHot, smoothstep(0.5, 1.0, t));
}

// The supernova's blast: behind its shockwave the gas has been shoved outward by up to snPush
// units (so the noise at p is what used to be nearer the centre), it piles up just behind the
// front, and the cavity inside thins out. Returns where to sample the gas; scale multiplies its density.
float3 Blast(float3 p, out float scale) {
    scale = 1.0;
    float3 moved = p;
    [branch] if (snPush > 0.0) {
        float3 sv = p - snPos;
        float ds = length(sv);
        float edge = ds - snShellRadius;                      // < 0: the shock has passed this point
        float push = snPush * smoothstep(1.2, -1.5, edge);
        float k = snPush / 3.0;
        scale = (1.0 + 0.6 * k * exp(-edge * edge * 1.5)) * lerp(1.0, 0.35, smoothstep(-1.0, -6.0, edge) * k);
        moved = p - sv / max(ds, 1e-3) * min(push, ds * 0.85);
    }
    return moved;
}

// World space wraps every 64 units (all noise periods divide 64), so the
// camera position can wrap without visible seams.
void Gas(float3 p, out float density, out float dust, out float shade, out float glint) {
    float blastScale;
    p = Blast(p, blastScale);
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
    density = saturate(saturate((d - threshold) * gain) * blastScale);
    // Dark dust lanes: absorb light but don't glow.
    dust = saturate((w.y - 0.58) * 6.0) * saturate((b.y - 0.35) * 2.0) * min(blastScale, 1.0);
    shade = saturate((a.z - 0.5) * 2.2 + 0.5);
    glint = b.w;
}

// A cheaper density (first warp level and the two main octaves) for the light-scattering
// look-ahead: close enough to say whether gas lies between a point and the light.
float GasLite(float3 p) {
    float blastScale;
    p = Blast(p, blastScale);
    float4 w = Noise.SampleLevel(Wrap, p / 64.0 + warpOffset, 0);
    float3 q = p + (float3(w.x, w.z, w.y) - 0.5) * warp;
    float4 a = Noise.SampleLevel(Wrap, q / 32.0, 0);
    float4 b = Noise.SampleLevel(Wrap, q / 8.0, 0);
    return saturate(saturate((lerp(a.x, a.y, filament) + (b.x - 0.5) * detail - threshold) * gain) * blastScale);
}

// Gas fades out very close to the lens (up close the noise is magnified into big soft blobs)
// and towards the far end of the march.
float GasFade(float t) { return smoothstep(1.0, 5.5, t) * smoothstep(kMaxDist, kMaxDist * 0.5, t); }

#if NEB_VISIBILITY

// How much light gets from a point to the camera through the gas (same steps as the raymarch).
float Transmittance(float3 target) {
    float3 rel = target - camPos;
    float dist = length(rel);
    float3 rd = rel / max(dist, 1e-4);
    float t = 0.25, transmittance = 1.0;
    [loop] for (int s = 0; s < kMaxSteps && t < min(dist, kMaxDist); ++s) {
        float stepLen = 0.2 + t * 0.03;
        float fade = GasFade(t);
        [branch] if (fade * 2.0 > 0.003) {
            float density, dust, shade, glint;
            Gas(camPos + rd * t, density, dust, shade, glint);
            transmittance *= exp(-(density * absorb + dust * 1.5) * fade * stepLen);
            if (transmittance < 0.01) break;
        }
        t += stepLen;
    }
    return transmittance;
}

float4 main(VSOut i) : SV_Target {
    return float4(Transmittance(pulsarPos), Transmittance(snPos), 0, 1);
}

#else

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

// A star as a point in the volume: how bright this ray sees it, and the distance along the ray
// where it sits (it's added there, so gas in front hides it and gas behind doesn't).
// size: angular radius of the soft halo; the core is about a pixel across.
float3 PointStar(float3 rd, float3 pos, float3 colour, float brightness, float size, out float along) {
    float3 rel = pos - camPos;
    along = dot(rel, rd);
    if (brightness <= 0.0 || along <= 0.0) { along = -1.0; return 0; }
    float ang = sqrt(max(dot(rel, rel) - along * along, 0.0)) / along;   // angle off the ray
    float pixel = 2.0 * tanHalfFov / iResolution.y;
    float core = 1.0 / (1.0 + (ang * ang) / (pixel * pixel * 1.5));
    float halo = exp(-ang / size) * 0.25 + exp(-ang / (size * 4.0)) * 0.05;
    return colour * brightness * (core * 4.0 + halo);
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

    // The pulsar and the supernova's star, as points: added when the march reaches them.
    float pulsarAt, starAt;
    float3 pulsarLight = PointStar(rd, pulsarPos, lerp(pulsarCol, 1.0, 0.5), pulsarStar, 0.012, pulsarAt);
    float3 starLight = PointStar(rd, snPos, snStarCol, snStar, snStarSize, starAt);
    float3 wakeSide = cross(pulsarAxis, pulsarDir);   // the way the beams are turning

    [loop] for (int s = 0; s < kMaxSteps; ++s) {
        float stepLen = 0.2 + t * 0.03;   // finer steps up close
        float3 p = camPos + rd * t;

        // Point stars between this step and the next (bright ones also anchor the reprojection).
        [branch] if (pulsarAt >= 0.0 && pulsarAt < t + stepLen) {
            col += transmittance * pulsarLight;
            float wgt = saturate(max(pulsarLight.r, max(pulsarLight.g, pulsarLight.b))) * transmittance;
            depthSum += wgt * pulsarAt; depthWeight += wgt;
            pulsarAt = -1.0;
        }
        [branch] if (starAt >= 0.0 && starAt < t + stepLen) {
            col += transmittance * starLight;
            float wgt = saturate(max(starLight.r, max(starLight.g, starLight.b))) * transmittance;
            depthSum += wgt * starAt; depthWeight += wgt;
            starAt = -1.0;
        }

        float3 toLight = lightPos - p;
        float dl2 = dot(toLight, toLight);
        float dl = sqrt(dl2);
        float glow = lightStrength / (1.0 + dl2 * 0.12);
        float shell = shellStrength * exp(-abs(dl - shellRadius) * 1.4);

        // Pulsar: two narrow beams from a spinning star, widening slightly with distance, and
        // behind them a wake: the plane they sweep stays lit for a moment after they pass.
        float beam = 0, pulsarCore = 0, wake = 0;
        [branch] if (pulsarStrength > 0.0) {
            float3 pv = p - pulsarPos;
            float along = dot(pv, pulsarDir);
            float pv2 = dot(pv, pv);
            float perp2 = pv2 - along * along;
            float bw = pulsarWidth * (1.0 + abs(along) * 0.08);
            beam = pulsarStrength * exp(-perp2 / (bw * bw)) * exp(-abs(along) * 0.04);
            pulsarCore = pulsarStrength * 1.5 / (1.0 + pv2 * 0.8);   // lights a pocket of gas around it, so it shows even deep in cloud
            float h = dot(pv, pulsarAxis);                          // height off the swept plane
            float r = sqrt(max(pv2 - h * h, 1e-6));
            float phi = atan2(dot(pv, wakeSide), along);            // angle ahead of the + beam
            float behind = frac(-phi / 3.14159265) * 3.14159265;    // since either beam passed
            float thick = 0.3 + r * 0.06;
            wake = pulsarWake * exp(-behind * 1.6) * exp(-h * h / (thick * thick)) * exp(-r * 0.06) * saturate(r * 2.0);
        }

        // Supernova: a blinding flash, then a shockwave shell that lights the gas as it passes.
        float snGlow = 0, snShell = 0, ds = 1e4;
        [branch] if (snFlash + snShellStrength > 0.0) {
            ds = length(p - snPos);
            snGlow = snFlash * 2.5 / (1.0 + ds * ds * 0.15);
            snShell = snShellStrength * exp(-abs(ds - snShellRadius) * 1.1);
        }

        // Skip the noise reads where the near/far fade leaves nothing to see: density and dust
        // are each at most 1, so no sample there can pass the threshold below (~8% faster).
        float fade = GasFade(t);
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
            float heat = density * 0.75 + (shade - 0.5) * 0.45 + light * 0.18 + shell * 0.3 + snShell * 0.6 + beam * 0.3 + wake * 0.3;
            float3 c = Palette(heat);
            float3 emission = c * (ambient + light + shell * 1.6) * (0.12 + density)
                            + colHot * (pow(saturate(glint), 10.0) * sparkle * (0.5 + glow) * density);
            // Rims: the thin, lit edges of clouds facing the core glow hot.
            emission += colHot * glow * lit * saturate(1.0 - density * 2.5) * density * 0.6;
            // Event light falling on the gas.
            emission += pulsarCol * (beam * 1.3 + pulsarCore + wake * 0.9) * (0.05 + density);
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
        float3 scattered = lightColor * glow * glow * 0.012 * scatter
                         + pulsarCol * (beam * 0.025 + pulsarCore * 0.02 + wake * 0.006)
                         + snCol * snGlow * 0.012;
        // The shockwave itself: a thin, ragged shell of hot gas, visible even where there's no
        // cloud to light. Blue-white at the leading edge, the look's hot colour behind it.
        [branch] if (snShellStrength > 0.0) {
            float edge = ds - (snShellRadius + (shade - 0.5) * 1.6 + (glint - 0.5) * 0.8);
            float width = 0.35 + snShellRadius * 0.04;
            float front = exp(-edge * edge / (width * width));
            scattered += lerp(colHot * 1.4, kShockBlue, smoothstep(-width, width, edge)) * front * snShellStrength * 0.07;
        }
        col += transmittance * stepLen * scattered;

        t += stepLen;
        if (t > kMaxDist) break;
    }
    // Point stars beyond the end of the march.
    if (pulsarAt >= 0.0) col += transmittance * pulsarLight;
    if (starAt >= 0.0) col += transmittance * starLight;
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

#endif
