#include "./shared.h"

// The composite output target (#2) stays 8-bit (see addon.cpp), so HDR is carried to the
// final pass in RGB only (the pass does not write alpha): per channel
//   encode e = sqrt(x / (1 + x)),  decode t = e^2, x = t / (1 - t).
// Invertible, 0 -> 0, 1 -> 0.707, sqrt keeps shadow precision above vanilla's linear 8-bit.
float3 CoJEncodeHDR(float3 color) {
  color = max(0, color);
  return sqrt(color / (1.f + color));
}

float3 CoJDecodeHDR(float3 encoded) {
  float3 t = min(encoded * encoded, 0.999f);
  return t / (1.f - t);
}
