#include "signal_mesh.hlsli"

// Lit like signal_ps.hlsl's subjects: a hard key light, a cool rim from behind, dark crevices (the
// cavity term baked into the model stands in for ambient occlusion), ridges caught bright, and an ember
// deep in the eye sockets that flares on snares. Alpha marks coverage, for the data rain behind.
float4 main(MeshOut i) : SV_Target {
    float3 n = normalize(i.nrm), rd = normalize(i.view);
    if (dot(n, rd) > 0.0) n = -n;   // the inside of a thin part
    float cav = i.bake.x;
    float ao = saturate(1.0 - 0.95 * max(cav, 0.0));
    ao *= ao;
    float ridge = max(-cav, 0.0);
    float key = saturate(dot(n, normalize(float3(0.75, 0.6, 0.5))));
    float rim = pow(saturate(1.0 + dot(n, rd)), 2.5) * saturate(dot(n, normalize(float3(-0.9, 0.1, -0.3))) + 0.5);
    float bone = (0.03 + 1.1 * pow(key, 1.8)) * ao * (1.0 + 0.6 * ridge);
    float3 col = float3(bone, bone * 0.95, bone * 0.88);
    col += float3(0.15, 0.45, 1.0) * rim * 0.8 * ao;
    float socket = i.bake.y;
    col = lerp(col, col * 0.08 + float3(1.0, 0.12, 0.08) * (0.12 + 1.1 * i.eyes) * socket, saturate(socket * 1.5));
    return float4(col, 1.0);
}
