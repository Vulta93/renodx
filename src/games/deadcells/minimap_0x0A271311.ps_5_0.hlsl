// 0x0A271311: minimap / map shader (Heaps sprite with a mask texture).
// Reconstructed from renodx-dev/dump/0x0A271311.ps_5_0.cso:
//   color = t0(uv) * pixelColor; color.a *= t1(uv * cb1[0].xy + cb1[0].zw).x;
//   o0 = color * (1 - displaceBias)
// Fix: clamp to 0..1 like the original 8-bit target did, so the map can't
// exceed UI white after the float16 resource upgrade.
#include "./common.hlsl"

cbuffer cb1 : register(b1) { float4 cb1[1]; }

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
SamplerState s0_s : register(s0);
SamplerState s1_s : register(s1);

void main(
    float4 position : SV_POSITION,
    float4 pixelColor : pixelColor,
    float2 calculatedUV : calculatedUV,
    float displaceBias : displaceBias,
    out float4 o0 : SV_TARGET0)
{
  float mask = t1.Sample(s1_s, calculatedUV * cb1[0].xy + cb1[0].zw).x;
  float4 color = t0.Sample(s0_s, calculatedUV) * pixelColor;
  color.a *= mask;
  o0 = saturate(color * (1.0f - displaceBias));
}
