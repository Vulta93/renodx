// 0x40BF5761: plain texture copy. Used to add the blurred glow layer
// (blurred by 0x991A7AE4) on top of the scene - this is the white metal/weapon
// "shine". Original: o0 = t0.Sample(s0, calculatedUV).
// Change: scale by the "Glow Strength" slider (100% = original).
#include "./common.hlsl"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 position : SV_POSITION,
    float2 calculatedUV : calculatedUV,
    out float4 o0 : SV_TARGET0)
{
  o0 = t0.Sample(s0_s, calculatedUV) * shader_injection.custom_glow_strength;
}
