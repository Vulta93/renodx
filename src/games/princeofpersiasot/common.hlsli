#ifndef SRC_GAMES_PRINCEOFPERSIASOT_COMMON_HLSLI_
#define SRC_GAMES_PRINCEOFPERSIASOT_COMMON_HLSLI_

#include "./shared.h"

// Shared by the three addon-drawn passes (see addon.cpp, "Scene passes").
float4 scene_pass_params : register(c49);  // xy = 1 / render target size

// Addon-only tone mapper value (renodx::draw uses 0-3); see the ToneMapType setting in addon.cpp.
static const float TONE_MAP_TYPE_ROLLOFF = 4.f;

float2 ScenePassUV(float2 vpos) {
  return (vpos + 0.5f) * scene_pass_params.xy;
}

// SDR version of the frame (linear, intermediate units) handed to the game's soft-glow chain, which needs values
// <= 1.0: its ps_1_1 shaders clamp at white and its composite blends that clamped blur over the image (before the
// bridge, a test ramp showed every highlight cut to ~42% of its above-white part). Identical for every pixel <= 1.0; above white the colour is scaled by its max channel so hue is
// kept (max-channel bridge, handle-sdr-tonemap-lut). The same value is the neutral reference of the reconstruction
// in scene_upgrade_ps.
float3 BridgeSDR(float3 hdr_linear) {
  const float3 color = max(0.f, hdr_linear);
  return color / max(1.f, max(color.r, max(color.g, color.b)));
}

#endif  // SRC_GAMES_PRINCEOFPERSIASOT_COMMON_HLSLI_
