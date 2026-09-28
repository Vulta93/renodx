// Plain texture copy. Full-resolution use: scene #1 -> 8-bit target #2, read by the heat haze
// 0xD84F5620 with sRGB decoding. The vanilla 8-bit scene was stored sRGB-encoded; the float16
// scene is linear, so in HDR the full-res copy is sRGB-encoded here (otherwise the haze is ~2x
// too dark: "black smoke"). The flag is set per draw in addon.cpp (small 128x128 copies of the
// same shader stay untouched).
#include "./common.hlsli"

sampler2D s_tex : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 color = tex2D(s_tex, uv);
  if (RENODX_TONE_MAP_TYPE > 0.f && CUSTOM_COPY_FULL_RES > 0.5f) {
    color.rgb = renodx::color::srgb::Encode(saturate(color.rgb));
  }
  return color;
}
