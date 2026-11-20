#include "common.hlsli"

// Generates the nebula's 64^3 tileable noise volume once at startup.
//   r: gradient-noise fBm (soft clouds)
//   g: inverted Worley / cellular noise (filaments and bubbles)
//   b: second gradient-noise fBm, different seed (colour variation)
//   a: fine Worley (sparkle / small detail)
// Every octave's period divides the texture size, so it tiles seamlessly.

RWTexture3D<unorm float4> Output : register(u0);
static const int kSize = 64;

// Cells are at most one period below zero here, so a positive bias makes the modulus unsigned.
int3 Wrap(int3 c, int period) { return int3(uint3(c + period) % uint(period)); }

float3 Gradient(int3 c, int period, uint seed) {
    uint3 h = Pcg3d(uint3(Wrap(c, period)) + seed * 7919u);
    float3 g = float3(h & 0xFFFFu) / 32767.5 - 1.0;
    return g / max(length(g), 1e-4);
}

float Perlin(float3 p, int period, uint seed) {
    int3 i = (int3)floor(p);
    float3 f = p - floor(p);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n000 = dot(Gradient(i + int3(0, 0, 0), period, seed), f - float3(0, 0, 0));
    float n100 = dot(Gradient(i + int3(1, 0, 0), period, seed), f - float3(1, 0, 0));
    float n010 = dot(Gradient(i + int3(0, 1, 0), period, seed), f - float3(0, 1, 0));
    float n110 = dot(Gradient(i + int3(1, 1, 0), period, seed), f - float3(1, 1, 0));
    float n001 = dot(Gradient(i + int3(0, 0, 1), period, seed), f - float3(0, 0, 1));
    float n101 = dot(Gradient(i + int3(1, 0, 1), period, seed), f - float3(1, 0, 1));
    float n011 = dot(Gradient(i + int3(0, 1, 1), period, seed), f - float3(0, 1, 1));
    float n111 = dot(Gradient(i + int3(1, 1, 1), period, seed), f - float3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, u.x), lerp(n010, n110, u.x), u.y),
                lerp(lerp(n001, n101, u.x), lerp(n011, n111, u.x), u.y), u.z);
}

float Fbm(float3 p01, int basePeriod, uint seed) {
    float sum = 0, amp = 0.5, norm = 0;
    [unroll] for (int o = 0; o < 4; ++o) {
        int period = basePeriod << o;
        sum += amp * Perlin(p01 * period, period, seed + o);
        norm += amp;
        amp *= 0.5;
    }
    return saturate(sum / norm * 0.9 + 0.5);
}

float Worley(float3 p01, int period, uint seed) {
    float3 p = p01 * period;
    int3 i = (int3)floor(p);
    float3 f = p - floor(p);
    float best = 9.0;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        int3 o = int3(x, y, z);
        float3 jitter = float3(Pcg3d(uint3(Wrap(i + o, period)) + seed * 104729u) & 0xFFFFu) / 65535.0;
        float3 r = float3(o) + jitter - f;
        best = min(best, dot(r, r));
    }
    return sqrt(best);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    float3 p = (float3(id) + 0.5) / kSize;
    float clouds = Fbm(p, 4, 1u);
    float cells = 0.65 * (1.0 - Worley(p, 4, 2u)) + 0.35 * (1.0 - Worley(p, 8, 3u));
    float tint = Fbm(p, 2, 11u);
    float fine = 1.0 - Worley(p, 16, 5u);
    Output[id] = float4(clouds, saturate(cells * 1.1), tint, saturate(fine));
}
