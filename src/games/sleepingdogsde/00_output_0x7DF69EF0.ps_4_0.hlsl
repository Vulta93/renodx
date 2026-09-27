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


// 3Dmigoto declarations
#define cmp -


void main(
  float4 v0 : SV_Position0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
  float4 r0;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyzw = texDiffuse.Sample(_texDiffuse_s, v1.xy).xyzw;
  o0.xyzw = cbShaderParams.Value0.xxxx + r0.xyzw;

  o0.rgb = pow(abs(o0.rgb), 2.2f);
  o0.rgb *= injectedData.toneMapUINits / 80.f;

  // Safety cap: nothing may exceed Peak Brightness (UI glow flashes reached 4000+ nits).
  // Scale by the brightest channel so hue is preserved.
  float peak_scrgb = injectedData.toneMapPeakNits / 80.f;
  float max_channel = max(o0.r, max(o0.g, o0.b));
  if (injectedData.toneMapType != 0.f && max_channel > peak_scrgb) {
    o0.rgb *= peak_scrgb / max_channel;
  }
  return;
}