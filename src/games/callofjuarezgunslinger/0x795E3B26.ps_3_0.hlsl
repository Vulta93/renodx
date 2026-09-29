// Sun disc candidate (drawn right after the sky 0x3848A019): colour * falloff^2.
// The sky panorama only contains a soft glow; the crisp disc must be a separate draw.
// HDR: rgb stays vanilla (the composite finds the sun as sky pixels above white). The blend is
// additive in rgb AND alpha; the sky stores alpha as -1 - a, so the sprite's alpha is negated to
// add to the glow weight like in vanilla (and keep the sky marker intact).
#include "./common.hlsli"

float4 CONST_101 : register(c0);

float4 main(float falloff : TEXCOORD0) : COLOR {
  float4 color = falloff * falloff * CONST_101;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    color.a = -color.a;
  }
  return color;
}
