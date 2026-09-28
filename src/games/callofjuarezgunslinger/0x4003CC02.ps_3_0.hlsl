// Final pass: gamma (brightness option) -> back buffer. Reads the composite output
// (linear, HDR when tone mapping is on) with the blood overlay blended on top.
#include "./common.hlsli"

float4 GAMMA : register(c0);  // .y gamma exponent (1/2.2 at default brightness)

sampler2D s_tex : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 tex = tex2D(s_tex, uv);

  float3 encoded = renodx::math::SignPow(tex.rgb, GAMMA.y);
  float3 color = renodx::color::srgb::DecodeSafe(encoded);
  color = renodx::draw::RenderIntermediatePass(color);

  return float4(color, tex.a);
}
