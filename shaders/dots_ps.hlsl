// Dot Field: one round, soft-edged dot, added to what's there.
struct DOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float3 col : COLOR0;
};

float4 main(DOut i) : SV_Target {
    float d = length(i.uv);
    float a = smoothstep(1.0, 0.55, d);
    return float4(i.col * a, 1.0);
}
