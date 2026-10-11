// Output pass: adds Value0.x (a constant black-level trim, measured -0.025) to the gamma-encoded image and writes the swap
// chain. The intermediate holds the scene scaled by Game / UI nits, so the trim is applied in vanilla's space: invert the
// intermediate pass back to the tone mapper's output gamma, add the trim, redo the pass. Added to the intermediate
// directly, a higher UI Brightness darkened the scene. UI pixels in the same target get the same treatment (the two
// cannot be separated here; the difference is ~0.007 in gamma at UI 400 / Game 203).
#include "./shared.h"

cbuffer cbShaderParams : register(b0) {
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
    float4 v0: SV_Position0,
    float2 v1: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float4 color = texDiffuse.Sample(_texDiffuse_s, v1.xy);
  // Inverse of the tone mapper's RenderIntermediatePass(srgb::DecodeSafe(output_gamma)).
  float3 output_gamma = renodx::color::srgb::EncodeSafe(renodx::draw::InvertIntermediatePass(color.rgb));
  o0.rgb = renodx::draw::RenderIntermediatePass(renodx::color::srgb::DecodeSafe(output_gamma + cbShaderParams.Value0.x));
  o0.w = color.a + cbShaderParams.Value0.x;
}
