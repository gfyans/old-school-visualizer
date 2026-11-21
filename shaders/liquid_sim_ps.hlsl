#include "liquid.hlsli"

// Liquid's simulation passes, one per build (CMakeLists.txt compiles this file once for each PASS_*):
//   CURL        t0 velocity                -> curl
//   VORTICITY   t0 velocity, t1 curl       -> velocity  (vorticity confinement keeps eddies alive; the swirl
//                                                         of a build-up winds it all round the centre)
//   SPLAT_VEL   t0 velocity                -> velocity  (the music's pushes)
//   SPLAT_DYE   t0 dye                     -> dye       (the music's ink)
//   DIVERGENCE  t0 velocity                -> divergence
//   PRESSURE    t0 pressure, t1 divergence -> pressure  (one Jacobi step; run many times)
//   GRADIENT    t0 pressure, t1 velocity   -> velocity  (subtract the pressure gradient: incompressible)
//   ADVECT      t0 quantity, t1 velocity   -> quantity  (carried along the flow, slowly fading)

float4 main(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float2 tx = 1.0 / iResolution;   // texel of the target being written (the sim grid, or the dye)
    float2 dx = float2(tx.x, 0), dy = float2(0, tx.y);

#if defined(PASS_CURL)
    float vL = Tap(tex0, uv - dx).y, vR = Tap(tex0, uv + dx).y;
    float uU = Tap(tex0, uv - dy).x, uD = Tap(tex0, uv + dy).x;
    return float4(0.5 * ((vR - vL) - (uD - uU)), 0, 0, 1);

#elif defined(PASS_VORTICITY)
    float cL = abs(Tap(tex1, uv - dx).x), cR = abs(Tap(tex1, uv + dx).x);
    float cU = abs(Tap(tex1, uv - dy).x), cD = abs(Tap(tex1, uv + dy).x);
    float c = Tap(tex1, uv).x;
    float2 n = 0.5 * float2(cR - cL, cD - cU);
    n /= length(n) + 1e-4;
    float2 v = Tap(tex0, uv).xy;
    v += curlStrength * c * float2(n.y, -n.x) * dt;
    // The build-up's whirlpool: a turning push round the centre, strongest in a broad ring.
    float2 q = (uv - 0.5) * float2(aspect, 1.0);
    float r = length(q);
    v += swirl * float2(-q.y, q.x) / max(r, 0.05) * exp(-r * 2.2) * dt * 300.0;   // (sim texels are square)
    return float4(v, 0, 1);

#elif defined(PASS_SPLAT_VEL)
    float2 v = Tap(tex0, uv).xy;
    int n = (int)splatCount;
    [loop] for (int k = 0; k < n; ++k) v += splatForce[k].xy * SplatWeight(k, uv);
    return float4(v, 0, 1);

#elif defined(PASS_SPLAT_DYE)
    float3 d = Tap(tex0, uv).rgb;
    int n = (int)splatCount;
    [loop] for (int k = 0; k < n; ++k) d += splatColour[k].rgb * (splatPos[k].w * SplatWeight(k, uv));
    return float4(d, 1);

#elif defined(PASS_DIVERGENCE)
    float L = Tap(tex0, uv - dx).x, R = Tap(tex0, uv + dx).x;
    float U = Tap(tex0, uv - dy).y, D = Tap(tex0, uv + dy).y;
    // Walls: the flow can't leave the screen (mirror the velocity at the edges).
    float2 c = Tap(tex0, uv).xy;
    if (uv.x - tx.x < 0.0) L = -c.x;
    if (uv.x + tx.x > 1.0) R = -c.x;
    if (uv.y - tx.y < 0.0) U = -c.y;
    if (uv.y + tx.y > 1.0) D = -c.y;
    return float4(0.5 * (R - L + D - U), 0, 0, 1);

#elif defined(PASS_PRESSURE)
    float L = Tap(tex0, uv - dx).x, R = Tap(tex0, uv + dx).x;
    float U = Tap(tex0, uv - dy).x, D = Tap(tex0, uv + dy).x;
    float div = Tap(tex1, uv).x;
    return float4((L + R + U + D - div) * 0.25, 0, 0, 1);

#elif defined(PASS_GRADIENT)
    float L = Tap(tex0, uv - dx).x, R = Tap(tex0, uv + dx).x;
    float U = Tap(tex0, uv - dy).x, D = Tap(tex0, uv + dy).x;
    float2 v = Tap(tex1, uv).xy - 0.5 * float2(R - L, D - U);
    return float4(v, 0, 1);

#elif defined(PASS_ADVECT_VEL)
    float2 v = Tap(tex1, uv).xy;
    float2 from = uv - dt * v * simTexel;
    return float4(Tap(tex0, from).xy * velKeep, 0, 1);

#elif defined(PASS_ADVECT_DYE)
    float2 v = Tap(tex1, uv).xy;
    float2 from = uv - dt * v * simTexel;
    return float4(Tap(tex0, from).rgb * dyeKeep, 1);

#else
    return 0;
#endif
}
