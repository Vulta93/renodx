#ifndef SRC_GAMES_PRINCEOFPERSIAWW_COMMON_HLSLI_
#define SRC_GAMES_PRINCEOFPERSIAWW_COMMON_HLSLI_

#include "./shared.h"

// Shared by the two addon-drawn scene passes (see addon.cpp, "Scene passes").
float4 scene_pass_params : register(c49);  // xy = 1 / render target size
float4 game_blit_c0 : register(c48);       // c0 of the game's scene blit

float2 ScenePassUV(float2 vpos) {
  return (vpos + 0.5f) * scene_pass_params.xy;
}

// The game's scene blit (ps_1_1 0x2059E26C): r0 = t0 * c0 + (t0 - c0), here without the ps_1_x / 8-bit clamp.
// Returns the linear (sRGB-decoded) HDR scene.
float3 SceneUntonemapped(sampler2D scene_texture, float2 vpos) {
  const float4 scene = tex2Dlod(scene_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  const float4 blit = scene * game_blit_c0 + (scene - game_blit_c0);
  return renodx::color::srgb::DecodeSafe(blit.rgb);
}

// The SDR scene handed to the game's own post effects (glow B / blur), which need values <= 1.0 (their ps_1_1
// shaders clamp, and their blending relies on it). Identical to vanilla for every pixel <= 1.0. Above white:
// Vanilla mode clips per channel like the original 8-bit target; otherwise the colour is scaled by its max
// channel so hue is kept (max-channel bridge, handle-sdr-tonemap-lut). The same value is the neutral_sdr
// reference of the HDR reconstruction in scene_finish_ps.
float3 SceneNeutralSDR(float3 untonemapped) {
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    return saturate(untonemapped);
  }
  const float3 color = max(0.f, untonemapped);
  return color / max(1.f, max(color.r, max(color.g, color.b)));
}

#endif  // SRC_GAMES_PRINCEOFPERSIAWW_COMMON_HLSLI_
