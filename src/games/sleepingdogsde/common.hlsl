#include "./shared.h"

// The scene buffer stores y = 1.04x / (x + 0.2); vanilla's 8-bit target caps y at 1 (x = 5). In the float16 upgrade the
// additive glows and particles stack above it, where the vanilla decode turns y >= 1.04 into black or inf.
float3 DecodeScene(float3 encoded) {
  float3 vanilla_range = saturate(encoded);
  return 0.2f * vanilla_range / (1.04f - vanilla_range);
}
