// Sun disc candidate (drawn right after the sky 0x3848A019): colour * falloff^2.
// The sky panorama only contains a soft glow; the crisp disc must be a separate draw.
// HDR: rgb multiplied by a marker so the composite can lift the sun to "Sun Brightness".
#include "./common.hlsli"

float4 CONST_101 : register(c0);

float4 main(float falloff : TEXCOORD0) : COLOR {
  float4 color = falloff * falloff * CONST_101;
  if (RENODX_TONE_MAP_TYPE > 0.f && CUSTOM_SUN_BRIGHTNESS > 0.f) {
    color.rgb *= COJ_SUN_MARKER;
  }
  return color;
}
