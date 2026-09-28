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

// Sun disc marker written by 0x795E3B26 into the float16 scene target (nothing else in
// the scene gets anywhere near it); the composite detects it and lifts the sun.
static const float COJ_SUN_MARKER = 64.f;

// Sky marker: the sky 0x3848A019 writes its alpha (glow weight a >= 0) as -1 - a, so the
// composite can tell sky from world (mask = saturate(-alpha)). The glow bright pass
// 0x41AE4161 decodes it back to the vanilla weight.
float CoJDecodeSceneAlpha(float a) {
  return (a < 0.f) ? max(0.f, -1.f - a) : a;
}
