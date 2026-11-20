#ifndef COMMON_HLSLI
#define COMMON_HLSLI

// Matches FrameConstants in src/render/Renderer.h
cbuffer Frame : register(b0) {
    float iTime;
    float iDeltaTime;
    float2 iResolution;   // size of the target being drawn (may be below window size)
};

// Matches AudioConstants in src/render/Renderer.h. All values 0..1.
cbuffer Audio : register(b1) {
    float4 aBands;        // x bass, y low-mid, z high-mid, w treble
    float aLevel;         // loudness now
    float aIntensity;     // slow average loudness (song section)
    float aBeat;          // 1 on beat, decays
    float aBeatCount;     // increments every beat
    float4 aSpectrum[16]; // 64 log-spaced bands, 30 Hz - 16 kHz
};

float Spectrum(int band) {
    float4 v = aSpectrum[band >> 2];
    int c = band & 3;
    return c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w;
}

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;   // 0..1, origin top-left
};

// Integer hash, 3 in / 3 out (Jarzynski & Olano, "Hash Functions for GPU Rendering").
uint3 Pcg3d(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float3 Hash33(int3 p) {
    return float3(Pcg3d(uint3(p)) & 0xFFFFu) / 65535.0;
}

// Cheap, stable per-pixel noise in [0,1) (Jimenez 2014); good for dithering.
float InterleavedGradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

#endif
