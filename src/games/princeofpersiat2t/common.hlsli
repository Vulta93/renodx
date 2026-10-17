#ifndef SRC_GAMES_PRINCEOFPERSIAT2T_COMMON_HLSLI_
#define SRC_GAMES_PRINCEOFPERSIAT2T_COMMON_HLSLI_

#include "./shared.h"

// Shared by the two addon-drawn scene passes (see addon.cpp, "Scene passes").
float4 scene_pass_params : register(c49);  // xy = 1 / render target size
float4 game_blit_c0 : register(c48);       // c0 of the game's offset blit (pass 1 only)

float2 ScenePassUV(float2 vpos) {
  return (vpos + 0.5f) * scene_pass_params.xy;
}

// The HDR scene before the game's offset blit (ps_1_1 0x10B45210: rgb = t0 + bx2(c0) = t0 + (2 * c0 - 1)). The offset
// is not part of this signal: it is a per-frame grade applied to the SDR scene in scene_sdr_ps, so it ends up in
// graded_sdr and fades to black stay black. Returns the linear (sRGB-decoded) scene.
float3 SceneUntonemapped(sampler2D scene_texture, float2 vpos) {
  const float4 scene = tex2Dlod(scene_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  return renodx::color::srgb::DecodeSafe(scene.rgb);
}

// The SDR scene handed to the game's own glow and post effects, which need values <= 1.0 (their ps_1_1 shaders
// clamp, and their blending relies on it). Identical to vanilla for every pixel <= 1.0. Above white: Vanilla mode
// clips per channel like the original 8-bit target; otherwise the colour is scaled by its max channel so hue is kept
// (max-channel bridge, handle-sdr-tonemap-lut). The same value is the neutral_sdr reference of the HDR reconstruction
// in scene_finish_ps.
float3 SceneNeutralSDR(float3 untonemapped) {
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    return saturate(untonemapped);
  }
  const float3 color = max(0.f, untonemapped);
  return color / max(1.f, max(color.r, max(color.g, color.b)));
}

#endif  // SRC_GAMES_PRINCEOFPERSIAT2T_COMMON_HLSLI_
