// Galaxy particles: each is a single additive point.
struct POut {
    float4 pos : SV_Position;
    float3 col : COLOR0;
};

float4 main(POut i) : SV_Target { return float4(i.col, 1.0); }
