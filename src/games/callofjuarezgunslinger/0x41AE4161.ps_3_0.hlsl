// Glow bright pass: scene alpha = glow weight. Decodes the sky marker (see common.hlsli).
#include "./common.hlsli"

float4 CONST_100 : register(c0);
float4 CONST_101 : register(c1);
sampler2D s_clr : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 scene = tex2D(s_clr, uv);
  float weight = saturate(CoJDecodeSceneAlpha(scene.a) * CONST_101.x + CONST_101.y);
  float3 color = scene.rgb * weight;
  float lum = dot(color, CONST_100.rgb);
  return float4(saturate(color * CONST_100.w + lum), weight);
}
