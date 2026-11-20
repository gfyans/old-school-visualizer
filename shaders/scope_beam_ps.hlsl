#include "scope_beam.hlsli"

// Oscilloscope beam: Gaussian falloff from the segment. Like a real electron beam, it is
// brighter where it moves slowly (short segments = long dwell). Output goes additively into
// the phosphor buffer: R = slow afterglow channel, G = fast "white-hot" channel.

float4 main(BeamVSOut i) : SV_Target {
    float2 p = i.pos.xy;
    float2 a = i.seg.xy, b = i.seg.zw;
    float2 ab = b - a;
    float along = dot(p - a, ab);
    float len2 = max(dot(ab, ab), 1e-4);
    float h = saturate(along / len2);
    float d = length(p - a - ab * h);
    float core = exp(-d * d / (2.0 * beamWidth * beamWidth));
    // Half weight in the round caps: neighbouring segments' caps overlap, so joints stay even.
    float cap = (along < 0.0 || along > len2) ? 0.5 : 1.0;
    float dwell = min(beamEnergy * 6.0 / (i.extra.y + beamWidth * 1.5), 3.0);
    float intensity = core * cap * dwell * i.extra.x;
    return float4(intensity, intensity, 0, 0);
}
