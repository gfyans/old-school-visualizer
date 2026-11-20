#include "scope_beam.hlsli"

// Oscilloscope beam: each segment between consecutive points becomes a quad (6 vertices,
// generated from SV_VertexID) wide enough to hold the beam's soft glow.
// Points are in height units: (0,0) is the screen centre, y up, +-1 = top/bottom.

StructuredBuffer<float4> Points : register(t0);   // xy position, z intensity, w = 1 beam on / 0 pen up

BeamVSOut main(uint id : SV_VertexID) {
    uint s = id / 6, c = id % 6;
    float4 a = Points[s], b = Points[s + 1];
    float2 pa = iResolution * 0.5 + float2(a.x, -a.y) * iResolution.y * 0.5;
    float2 pb = iResolution * 0.5 + float2(b.x, -b.y) * iResolution.y * 0.5;
    float2 d = pb - pa;
    float len = length(d);
    float2 dir = len > 1e-3 ? d / len : float2(1, 0);
    float2 nrm = float2(-dir.y, dir.x);
    float r = beamWidth * 4.0;   // reach of the glow

    bool atEnd = c == 1 || c == 2 || c == 4;
    float sideSign = (c == 2 || c == 4 || c == 5) ? 1.0 : -1.0;
    float2 p = (atEnd ? pb + dir * r : pa - dir * r) + nrm * r * sideSign;

    BeamVSOut o;
    o.pos = float4(p / iResolution * float2(2, -2) + float2(-1, 1), 0, 1);
    if (a.w < 0.5 || b.w < 0.5) o.pos = float4(0, 0, 0, 1);   // pen up: zero-area triangle
    o.seg = float4(pa, pb);
    o.extra = float2(0.5 * (a.z + b.z), len);
    return o;
}
