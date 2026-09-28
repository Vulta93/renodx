#include "./shared.h"

// The composite output target (#2) stays 8-bit (see addon.cpp), so HDR is carried to the
// final pass in RGB only (the pass does not write alpha): per channel
//   encode e = sqrt(x / (K + x)),  decode t = e^2, x = K t / (1 - t).
// K = 4: ~2% steps around 3x white, finer than vanilla's linear 8-bit in the shadows.
static const float COJ_ENCODE_K = 4.f;

float3 CoJEncodeHDR(float3 color) {
  color = max(0, color);
  return sqrt(color / (COJ_ENCODE_K + color));
}

float3 CoJDecodeHDR(float3 encoded) {
  float3 t = min(encoded * encoded, 0.999f);
  return COJ_ENCODE_K * t / (1.f - t);
}
