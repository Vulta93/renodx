// Final grade: per-channel curve texture, brightness/contrast, saturation, gamma.
// Writes to the back buffer (before UI).
#include "./common.hlsli"

float4 CONST_103 : register(c0);  // .x contrast/scale, .y offset, .z saturation
float4 GAMMA : register(c1);      // .y gamma exponent

sampler2D s_tex : register(s0);   // tonemapped scene (gamma 2.0 encoded)
sampler2D s_crv : register(s1);   // 1D per-channel curve

float3 VanillaGrade(float3 encoded) {
  float3 curve;
  curve.r = tex2D(s_crv, encoded.rr).r;
  curve.g = tex2D(s_crv, encoded.gg).g;
  curve.b = tex2D(s_crv, encoded.bb).b;
  float3 color = saturate(curve * CONST_103.x + CONST_103.y);
  float lum = saturate(dot(float3(0.2125f, 0.7154f, 0.0721f), color));
  color = saturate(CONST_103.z * (color - lum) + lum);
  return exp2(log2(color) * GAMMA.y);
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 tex = tex2Dlod(s_tex, float4(uv, 0, 0));

  float3 color;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    float3 untonemapped = renodx::color::gamma::DecodeSafe(tex.rgb, 2.f);
    float3 neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(untonemapped);
    float3 graded_sdr = renodx::color::srgb::DecodeSafe(VanillaGrade(sqrt(neutral_sdr)));
    color = renodx::draw::ToneMapPass(untonemapped, graded_sdr, neutral_sdr);
  } else {
    color = renodx::color::srgb::DecodeSafe(VanillaGrade(saturate(tex.rgb)));
  }
  color = renodx::draw::RenderIntermediatePass(color);

  return float4(color, tex.a);
}
