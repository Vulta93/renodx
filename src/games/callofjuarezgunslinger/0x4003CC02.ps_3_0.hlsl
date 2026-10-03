// Final pass: gamma (brightness option) -> back buffer. Reads the composite output
// (linear, HDR when tone mapping is on) with the blood overlay blended on top.
#include "./common.hlsli"

float4 GAMMA : register(c0);  // .y gamma exponent (1/2.2 at default brightness)

sampler2D s_tex : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 tex = tex2D(s_tex, uv);

  float3 linear_color = (RENODX_TONE_MAP_TYPE > 0.f) ? CoJDecodeHDR(tex.rgb) : tex.rgb;
  // The game's gamma/brightness exponent is only meaningful on 0..1: applied to HDR values
  // (e.g. pow(x, 0.8) then sRGB decode ~ x^1.8) it explodes highlights. Apply it to the
  // colour normalised by its max channel and scale the result back linearly.
  float scale = max(1.f, max(linear_color.r, max(linear_color.g, linear_color.b)));
  // Shadow Lift: same mechanism as the game's brightness option (smaller exponent),
  // white stays white. 100% = exponent x0.7, 250% = x0.25. Vanilla mode leaves the game's exponent alone.
  float shadow_lift = (RENODX_TONE_MAP_TYPE > 0.f) ? clamp(CUSTOM_SHADOW_LIFT, 0.f, 2.5f) : 0.f;
  float gamma_exp = GAMMA.y * (1.f - 0.3f * shadow_lift);
  float3 encoded = renodx::math::SignPow(linear_color / scale, gamma_exp);
  float3 color = renodx::color::srgb::DecodeSafe(encoded) * scale;
  color = renodx::draw::RenderIntermediatePass(color);

  return float4(color, tex.a);
}
