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
