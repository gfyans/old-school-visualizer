#ifndef DOTS_HLSLI
#define DOTS_HLSLI
#include "common.hlsli"

// Shared by the dot pass and the output pass of the Dot Field.
cbuffer Dots : register(b2) {   // matches DotsConstants in DotsVisualizer.cpp
    float3 camRight;  float zoom;        // orthographic (isometric) camera: world -> screen height units
    float3 camUp;     float aspect;
    float offsetY;    float dotBright;   float minRadius;  float waveTime;   // minRadius: about a pixel, in world units
    float layoutA;    float layoutB;     float morph;      float side;       // side: dots per side (count = side^2)
    float motionA;    float motionB;     float motionBlend; float histHead;  // histHead: newest history row (fractional)
    float splashT;    float splashMorph; float splashPower; float flash;     // splashT < 0: no splash
    float3 baseCol;   float hat;
    float3 midCol;    float hatSeed;
    float3 peakCol;   float glow;
    float3 bgCol;     float buildUp;
    float scanPos;    float scanStrength; float glitch;   float glitchSeed;   // bar sweep across the plane; glitch slices
    float breach;     float breachSeed;  float beams;    float dotsPad;      // breach: how shattered (0 = whole)
};

static const float kPi = 3.14159265;
static const float kTau = 6.2831853;

#endif
