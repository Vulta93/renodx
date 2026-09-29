// Plain texture copy. Full-resolution use: scene #1 -> 8-bit target #2, read by the heat haze
// 0xD84F5620 with sRGB decoding. The vanilla 8-bit scene was stored sRGB-encoded; the float16
// scene is linear, so in HDR the full-res copy is sRGB-encoded here (otherwise the haze is ~2x
// too dark: "black smoke"). The flag is set per draw in addon.cpp (small 128x128 copies of the
// same shader stay untouched).
// Copy into the float16 scene target #1 (after the composite; the damage "tear" 0x03AF484E reads it
// back as its refraction source with sRGB decoding, which a float16 texture cannot do): write
// the decoded (linear) values, i.e. what the 8-bit chain would have returned when read back.
#include "./common.hlsli"

sampler2D s_tex : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 color = tex2D(s_tex, uv);
  if (CUSTOM_COPY_FULL_RES > 1.5f) {
    color.rgb = renodx::color::srgb::DecodeSafe(color.rgb);
  } else if (RENODX_TONE_MAP_TYPE > 0.f && CUSTOM_COPY_FULL_RES > 0.5f) {
    color.rgb = renodx::color::srgb::Encode(saturate(color.rgb));
  }
  return color;
}
