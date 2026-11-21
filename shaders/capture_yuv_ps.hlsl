// Recording: converts the recording-size RGB frame to NV12 planes on the GPU (BT.709, limited
// range), so the CPU only copies 1.5 bytes a pixel and the video encoder takes it directly.
//   PLANE 0: luma, one value per pixel (R8 target, full size)
//   PLANE 1: chroma, Cb and Cr per 2x2 block (R8G8 target, half size; bilinear at the block's
//            centre averages its four pixels)
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

Texture2D Source : register(t0);
SamplerState Linear : register(s0);

#ifndef PLANE
#define PLANE 0
#endif

float4 main(VSOut i) : SV_Target {
    float3 c = Source.SampleLevel(Linear, i.uv, 0).rgb;
#if PLANE == 0
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    return float4((16.0 + 219.0 * y) / 255.0, 0, 0, 1);
#else
    float cb = dot(c, float3(-0.1146, -0.3854, 0.5));
    float cr = dot(c, float3(0.5, -0.4542, -0.0458));
    return float4((128.0 + 224.0 * cb) / 255.0, (128.0 + 224.0 * cr) / 255.0, 0, 1);
#endif
}
