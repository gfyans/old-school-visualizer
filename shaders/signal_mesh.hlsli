#ifndef SIGNAL_MESH_HLSLI
#define SIGNAL_MESH_HLSLI
#include "common.hlsli"

// Signal's skull model, drawn once per tile (instanced) into an offscreen picture that signal_ps.hlsl
// then renders in the cyber styles. Matches MeshInstances in SignalVisualizer.cpp.
static const int kMaxMeshInstances = 64;
cbuffer SignalMesh : register(b3) {
    float4 instA[kMaxMeshInstances];   // x yaw, y pitch, z scale, w eye glow
    float4 instB[kMaxMeshInstances];   // xy tile centre (screen units: y -1..1), z tile scale, w brightness (< 1: the grid behind)
};

// The same camera as signal_ps.hlsl's raymarcher: at z = 3.4 looking down -z, focal length 1.9.
static const float kCamZ = 3.4, kFocal = 1.9;

struct MeshOut {
    float4 pos  : SV_Position;
    float3 view : TEXCOORD0;   // camera-relative position
    float3 nrm  : TEXCOORD1;
    float2 bake : TEXCOORD2;   // x cavity (-1 ridge .. 1 crevice), y eye socket 0..1
    float  eyes : TEXCOORD3;
    float  dim  : TEXCOORD4;   // brightness: the grid behind the centre skull is dimmed
};

#endif
