// Recording: copies the finished frame into the recording-size target (scaled, bilinear).
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

Texture2D Source : register(t0);
SamplerState Linear : register(s0);

float4 main(VSOut i) : SV_Target {
    return float4(Source.SampleLevel(Linear, i.uv, 0).rgb, 1.0);
}
