#ifndef SCOPE_BEAM_HLSLI
#define SCOPE_BEAM_HLSLI
#include "common.hlsli"

// Shared by the oscilloscope beam vertex and pixel shaders.
cbuffer Beam : register(b2) {   // matches BeamConstants in ScopeVisualizer.cpp
    float beamWidth;    // beam sigma, pixels
    float beamEnergy;   // overall brightness
    float beamPad0;
    float beamPad1;
};

struct BeamVSOut {
    float4 pos : SV_Position;
    nointerpolation float4 seg : TEXCOORD0;     // segment endpoints, pixels
    nointerpolation float2 extra : TEXCOORD1;   // x intensity, y segment length (pixels)
};

#endif
