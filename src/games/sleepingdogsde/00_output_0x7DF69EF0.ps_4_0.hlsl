#include "./shared.h"

cbuffer cbShaderParams : register(b0)
{

  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);

}

SamplerState _texDiffuse_s : register(s0);
Texture2D<float4> texDiffuse : register(t0);

void main(
  float4 v0 : SV_Position0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
  float4 r0 = texDiffuse.Sample(_texDiffuse_s, v1.xy);
  r0 += cbShaderParams.Value0.x;  // vanilla fade/overlay offset (gamma space)
  // Vanilla wrote this into an 8-bit UNORM back buffer, which clamps negatives to 0.
  // The old pow(abs()) hid negatives by flipping them positive; SwapChainPass keeps the
  // sign, so clamp here like the original hardware did.
  r0.rgb = max(0.f, r0.rgb);

  // Standard RenoDX output: gamma 2.2 decode (sign-safe), scale to UI nits,
  // clamp to Peak Brightness (max channel), encode scRGB.
  o0 = renodx::draw::SwapChainPass(r0);
  return;
}
