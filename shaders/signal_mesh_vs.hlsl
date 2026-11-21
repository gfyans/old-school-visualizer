#include "signal_mesh.hlsli"

// Object -> world turns the opposite way to signal_ps.hlsl's Map (which turns world into object space),
// so the model and the distance-field subjects move alike.
float3 Turn(float3 p, float yaw, float pitch) {
    float c = cos(pitch), s = sin(pitch);
    p.yz = float2(c * p.y + s * p.z, -s * p.y + c * p.z);
    c = cos(yaw); s = sin(yaw);
    p.xz = float2(c * p.x + s * p.z, -s * p.x + c * p.z);
    return p;
}

MeshOut main(float4 pos : POSITION, float4 nrm : NORMAL, uint id : SV_InstanceID) {
    float4 a = instA[id], b = instB[id];
    float3 world = Turn(pos.xyz * a.z, a.x, a.y);
    float3 v = world - float3(0, 0, kCamZ);
    float w = -v.z;
    float aspect = iResolution.x / iResolution.y;
    MeshOut o;
    // Perspective onto the screen, then into this instance's tile.
    float2 screen = v.xy * kFocal * b.z + b.xy * w;
    const float n = 0.5, f = 12.0;
    o.pos = float4(screen.x / aspect, screen.y, w * f / (f - n) - n * f / (f - n), w);
    o.view = v;
    o.nrm = Turn(nrm.xyz, a.x, a.y);
    o.bake = float2(pos.w, nrm.w);
    o.eyes = a.w;
    return o;
}
