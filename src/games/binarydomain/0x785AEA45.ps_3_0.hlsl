#include "./shared.h"

// Binary Domain final pass (ps_3_0 0x785AEA45): pow(x * c192.x + c192.y, c192.z) of the HUD buffer, the game's brightness /
// gamma setting (identity at the default), then onto the back buffer.
// The HUD buffer is float16 now. The HUD is drawn into it with additive blends (the pulse effect stacks about twelve
// glow sprites), and the original 8-bit buffer capped that sum at white. The addon keeps a copy of the scene from just
// before the HUD (s14, see addon.cpp) and this pass applies the same cap: the HUD can bring a pixel up to white, but not
// above the scene that was already there. HDR scene pixels above white are untouched.

float4 c_ps_user : register(c192);
sampler2D hud_buffer_texture : register(s0);
sampler2D scene_before_hud_texture : register(s14);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float3 color = tex2D(hud_buffer_texture, uv).rgb;
  [branch]
  if (RENODX_HUD_CLAMP != 0.f) {
    const float3 scene_before_hud = tex2D(scene_before_hud_texture, uv).rgb;
    color = min(color, max(scene_before_hud, 1.f));
  }
  return float4(pow(max(color * c_ps_user.x + c_ps_user.y, 0.f), c_ps_user.z), 1.f);
}
