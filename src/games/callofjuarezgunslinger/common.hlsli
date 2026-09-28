#include "./shared.h"

// DOF blur chain is 8-bit (capped at white): re-add the sharp pixel's part above white.
float3 CoJBlurHDR(float3 blur, float3 sharp) {
  return saturate(blur) + max(0, sharp - saturate(sharp));
}
