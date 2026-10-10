// Bloom bright pass: averages 5 scene taps, decodes them and keeps the part above a depth-faded threshold.
// Only the decode changes (DecodeScene); the 8-bit output still clamps like vanilla.
#include "./common.hlsl"

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
SamplerState _texDiffuse2_s : register(s1);
SamplerState _texDepth_s : register(s2);
SamplerState _texEmission_s : register(s3);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texDiffuse2 : register(t1);
Texture2D<float4> texDepth : register(t2);
Texture2D<float4> texEmission : register(t3);

void main(
    float4 v0: SV_Position0,
    float2 v1: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float2 offset = cbShaderParams.Value2.xy;
  float3 scene_sum = texDiffuse.Sample(_texDiffuse_s, v1.xy - offset).rgb;
  scene_sum += texDiffuse.Sample(_texDiffuse_s, v1.xy + offset * float2(1.f, -1.f)).rgb;
  scene_sum += texDiffuse.Sample(_texDiffuse_s, v1.xy).rgb;
  scene_sum += texDiffuse.Sample(_texDiffuse_s, v1.xy + offset).rgb;
  scene_sum += texDiffuse.Sample(_texDiffuse_s, v1.xy + offset * float2(-1.f, 1.f)).rgb;

  // Vanilla: 0.2y / (1.04 - y) on the 5-tap average, black or inf for y >= 1.04 in a float scene buffer.
  float3 scene = DecodeScene(scene_sum * 0.2f);
  float luminance = dot(scene, float3(0.2125f, 0.7154f, 0.0721f));

  float glow_weight = texDiffuse2.Sample(_texDiffuse2_s, v1.xy).w * 4.f;
  float view_depth = 1.f / (1.f - texDepth.Sample(_texDepth_s, v1.xy).x);
  float2 depth_fades = saturate(float2(view_depth - cbShaderParams.Value1.y, view_depth - 200.f) * cbShaderParams.Value0.w);
  glow_weight -= depth_fades.y * glow_weight;

  float3 emission = texEmission.Sample(_texEmission_s, v1.xy).rgb;
  luminance += max(max(emission.z, emission.y), emission.x) * glow_weight;

  float threshold = lerp(cbShaderParams.Value0.x, cbShaderParams.Value0.z, depth_fades.x);
  o0.rgb = max(luminance - threshold, 0.f) * cbShaderParams.Value0.y * scene;
  o0.w = 1.f;
}
