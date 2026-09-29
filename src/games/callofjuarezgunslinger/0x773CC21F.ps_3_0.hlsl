// Damage "tear" HUD effect, step 3: a faint grey glow, screen-blended (ONE, INVSRCCOLOR) along
// the tear's edges over the composite target (#2). That target holds the HDR-encoded image
// (see common.hlsli) and the blend is fixed-function, so the vanilla source value cannot be
// blended correctly (the sqrt-shaped encoding turned it into a big white wedge). In HDR the
// glow is dropped; in Vanilla mode this is the original shader.
#include "./common.hlsli"

float4 CONST_110 : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  if (RENODX_TONE_MAP_TYPE > 0.f) return float4(0.f, 0.f, 0.f, 0.f);
  float r = uv.y * uv.x;
  r = r * r;
  r = r * r;
  return float4(r * CONST_110.x, r * CONST_110.x, r * CONST_110.x, 0.f);
}
