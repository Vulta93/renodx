// Split final grade (first half): per-channel curve, brightness/contrast, saturation.
// Gamma is applied afterwards by 0x4550ED56. Used e.g. on the character-select screen.
#include "./common.hlsli"

float4 CONST_103 : register(c0);  // .x contrast/scale, .y offset, .z saturation

sampler2D s_tex : register(s0);   // tonemapped scene (gamma 2.0 encoded)
sampler2D s_crv : register(s1);   // 1D per-channel curve

float3 VanillaGrade(float3 encoded) {
  float3 curve;
  curve.r = tex2D(s_crv, encoded.rr).r;
  curve.g = tex2D(s_crv, encoded.gg).g;
  curve.b = tex2D(s_crv, encoded.bb).b;
  float3 color = saturate(curve * CONST_103.x + CONST_103.y);
  float lum = saturate(dot(float3(0.2125f, 0.7154f, 0.0721f), color));
  return saturate(CONST_103.z * (color - lum) + lum);
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float4 tex = tex2Dlod(s_tex, float4(uv, 0, 0));

  float3 color;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    // Grade an SDR stand-in and transfer it to the HDR image; stay gamma 2.0 encoded
    // for the gamma pass that follows.
    float3 hdr = renodx::color::gamma::DecodeSafe(tex.rgb, 2.f);
    float3 sdr = renodx::tonemap::renodrt::NeutralSDR(hdr);
    float3 graded = renodx::color::gamma::DecodeSafe(VanillaGrade(sqrt(sdr)), 2.f);
    color = renodx::tonemap::UpgradeToneMap(hdr, sdr, graded, 1.f);
    color = renodx::color::gamma::EncodeSafe(max(0, color), 2.f);
  } else {
    color = VanillaGrade(tex.rgb);
  }

  return float4(color, tex.a);
}
