#ifndef GALAXY_HLSLI
#define GALAXY_HLSLI
#include "common.hlsli"

// Shared by the particle pass and the black-hole/output pass.
cbuffer Galaxy : register(b2) {   // matches GalaxyConstants in GalaxyVisualizer.cpp
    float3 camPos;   float focal;
    float3 camRight; float aspect;
    float3 camUp;    float starBright;    // brightness of one particle
    float3 camFwd;   float pattern;       // rotation of the spiral pattern (radians)
    float4 shapeA;                        // arms, twist, arm width, bulge size
    float4 shapeB;                        // the look being morphed to
    float thickA;    float thickB;   float shapeBlend; float gravity;
    float4 waves[4];                      // beat shockwaves: x front radius, y strength
    float3 coreCol;  float accretion;     // accretion disc brightness
    float3 armCol;   float twinkle;
    float3 knotCol;  float jet;           // relativistic jets 0..1
    float3 discCol;  float animTime;
    float accSpin;   float horizon;  float exposure;  float flash;   // horizon radius (screen height units)
    float dustAmount; float3 skyCol;
    float glow;      float kick;     float2 bhPos;     // glow: smooth galaxy light; bhPos: black hole on screen
    float2 jetTipA;  float2 jetTipB;                     // quasar beam tips on screen (screen height units, y down)
    float quasar;    float beamWidth; float2 galPad;
};

static const float kTau = 6.2831853;
// Arm width is given across the arm; tightly wound arms need a wider angular spread to match.
float ArmWidth(float4 s) { return s.z * sqrt(1.0 + s.y * s.y) * 0.3; }

static const uint kStarCount = 1u << 20;   // particles from this index on are dust (GalaxyVisualizer::kParticles)

#endif
