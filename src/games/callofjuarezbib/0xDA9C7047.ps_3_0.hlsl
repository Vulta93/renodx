// Tonemap pre-pass: oC0 = tonemapped scene (feeds DOF blur), oC1 = glow bright-pass.
#include "./common.hlsli"

float4 CONST_100 : register(c0);  // .x bright-pass scale, .y/.z threshold, .w curve white param
float4 CONST_70 : register(c1);   // .y exposure scale

sampler2D s_clr : register(s0);
sampler2D s_avg : register(s1);

void main(float2 uv : TEXCOORD0, out float4 o0 : COLOR0, out float4 o1 : COLOR1) {
  float3 x = tex2D(s_clr, uv).rgb * CONST_70.y;
  x *= tex2Dlod(s_avg, float4(0, 0, 0, 0)).x;
  float3 y = CoJReinhard(x, CONST_100.w);

  // Bright pass for glow (kept vanilla / SDR)
  float lum = dot(y, 1.f / 3.f);
  float threshold = saturate(lum * CONST_100.y - CONST_100.z);
  float3 bright = saturate(y * threshold / lum * CONST_100.x);
  o1 = float4(sqrt(bright), 0.f);

  if (RENODX_TONE_MAP_TYPE > 0.f) {
    o0 = float4(renodx::color::gamma::EncodeSafe(CoJUntonemapped(x, y), 2.f), 0.f);
  } else {
    o0 = float4(sqrt(y), 0.f);
  }
}
