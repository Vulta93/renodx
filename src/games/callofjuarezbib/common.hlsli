#include "./shared.h"

// Chrome Engine 4 tonemap curve (extended Reinhard with white parameter):
//   y = x * (1 + x * white) / (1 + x)
float3 CoJReinhard(float3 x, float white) {
  return x * (x * white + 1.f) / (x + 1.f);
}

// HDR stand-in for the vanilla curve output: untonemapped scene scaled so that
// in the SDR range it matches the vanilla curve (grade transfer), unbounded above.
float3 CoJUntonemapped(float3 x, float3 vanilla_y) {
  x = max(0, x);
  return renodx::tonemap::UpgradeToneMap(
      x, renodx::tonemap::renodrt::NeutralSDR(x), vanilla_y, 1.f);
}

// DOF blur chain is 8-bit (capped at white): re-add the sharp pixel's part above white.
float3 CoJBlurHDR(float3 blur, float3 sharp) {
  return saturate(blur) + max(0, sharp - saturate(sharp));
}

// Bright pass for glow (kept vanilla / SDR), gamma 2.0 encoded.
float3 CoJBrightPass(float3 y, float4 params) {
  float lum = dot(y, 1.f / 3.f);
  float threshold = saturate(lum * params.y - params.z);
  return sqrt(saturate(y * threshold / lum * params.x));
}

// Final-pass HDR path shared by 0x1F6ABA21 / 0x4550ED56: run the vanilla grade on an
// SDR stand-in (NeutralSDR), transfer it to the HDR image and tone map.
#define COJ_FINAL_HDR(encoded, GRADE_FUNC, out_color)                                         \
  {                                                                                            \
    float3 _untonemapped = renodx::color::gamma::DecodeSafe(encoded, 2.f);                      \
    float3 _neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(_untonemapped);                 \
    float3 _graded_sdr = renodx::color::srgb::DecodeSafe(GRADE_FUNC(sqrt(_neutral_sdr)));      \
    out_color = renodx::draw::ToneMapPass(_untonemapped, _graded_sdr, _neutral_sdr);           \
  }
