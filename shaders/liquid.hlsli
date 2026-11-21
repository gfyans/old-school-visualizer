#ifndef LIQUID_HLSLI
#define LIQUID_HLSLI
#include "common.hlsli"

// Liquid: a GPU fluid simulation (stable fluids: advect, vorticity, pressure projection) with ink in it.
// Velocity lives on a small grid in sim texels per second; the dye is finer. Coordinates are texture uv
// (y down). Matches LiquidConstants in LiquidVisualizer.cpp.
static const int kMaxSplats = 24;
cbuffer Liquid : register(b2) {
    float2 simTexel;   float2 dyeTexel;
    float dt;          float curlStrength;  float velKeep;   float dyeKeep;     // keep: per-frame multipliers
    float swirl;       float look;          float aspect;    float time;
    float flash;       float kick;          float splatCount; float energy;
    float3 tint;       float buildUp;       // the palette's main colour (looks use it for the backdrop)
    float4 splatPos[kMaxSplats];      // xy uv, z radius (screen heights), w dye amount
    float4 splatForce[kMaxSplats];    // xy force (sim texels / s)
    float4 splatColour[kMaxSplats];   // rgb
};

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState linearClamp : register(s0);

float4 Tap(Texture2D t, float2 uv) { return t.SampleLevel(linearClamp, uv, 0); }

// A splat's weight at uv: a soft round blob (aspect-corrected so it isn't stretched).
float SplatWeight(int i, float2 uv) {
    float2 d = (uv - splatPos[i].xy) * float2(aspect, 1.0);
    float r = splatPos[i].z;
    return exp(-dot(d, d) / (r * r));
}

#endif
