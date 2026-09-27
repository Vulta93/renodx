// Final pass variant: gamma only (no curve / brightness / saturation). Writes to the back buffer.
#include "./common.hlsli"

float4 GAMMA : register(c0);  // .y gamma exponent

sampler2D s_tex : register(s0);

float3 VanillaGrade(float3 encoded) {
  return exp2(log2(saturate(encoded)) * GAMMA.y);
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 tex = tex2Dlod(s_tex, float4(uv, 0, 0));

  float3 color;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    COJ_FINAL_HDR(tex.rgb, VanillaGrade, color);
  } else {
    color = renodx::color::srgb::DecodeSafe(VanillaGrade(tex.rgb));
  }
  color = renodx::draw::RenderIntermediatePass(color);

  return float4(color, tex.a);
}
