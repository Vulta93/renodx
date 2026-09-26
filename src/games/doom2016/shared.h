#ifndef SRC_DOOM2016_SHARED_H_
#define SRC_DOOM2016_SHARED_H_

// Must be 32bit aligned. Keep small: Vulkan push constants are limited to 256 bytes total.
struct ShaderInjectData {
  float peak_white_nits;
  float diffuse_white_nits;
  float graphics_white_nits;
  float tone_map_type;

  float tone_map_exposure;
  float tone_map_highlights;
  float tone_map_shadows;
  float tone_map_contrast;

  float tone_map_saturation;
  float tone_map_highlight_saturation;
  float tone_map_blowout;
  float tone_map_flare;

  float gamma_correction;
  float scene_grade_strength;
  float tone_map_hue_correction;
  float padding0;
};

#define RENODX_PEAK_WHITE_NITS               shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS            shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS           shader_injection.graphics_white_nits
#define RENODX_TONE_MAP_TYPE                 shader_injection.tone_map_type
#define RENODX_TONE_MAP_EXPOSURE             shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS           shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS              shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST             shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION           shader_injection.tone_map_saturation
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION shader_injection.tone_map_highlight_saturation
#define RENODX_TONE_MAP_BLOWOUT              shader_injection.tone_map_blowout
#define RENODX_TONE_MAP_FLARE                shader_injection.tone_map_flare
#define RENODX_TONE_MAP_HUE_CORRECTION       shader_injection.tone_map_hue_correction
#define RENODX_GAMMA_CORRECTION              shader_injection.gamma_correction
#define RENODX_COLOR_GRADE_STRENGTH          shader_injection.scene_grade_strength
#define RENODX_RENO_DRT_TONE_MAP_METHOD      renodx::tonemap::renodrt::config::tone_map_method::REINHARD
// Intermediate (scene + UI in the game's buffers) and swap chain decoding follow the
// gamma-correction setting: sRGB when off, 2.2 when on.
#define RENODX_INTERMEDIATE_ENCODING         (RENODX_GAMMA_CORRECTION + 1.f)
#define RENODX_SWAP_CHAIN_DECODING           RENODX_INTERMEDIATE_ENCODING
#define RENODX_SWAP_CHAIN_ENCODING           ENCODING_SCRGB
// Wide gamut: the scene is stored in the game's r11g11b10 buffer, which can't hold negative values.
// Colours outside BT.709 are negative in BT.709 but positive in BT.2020, so the intermediate
// (tonemap -> final pass -> swap chain proxy) is kept in BT.2020.
#define RENODX_INTERMEDIATE_COLOR_SPACE      renodx::color::convert::COLOR_SPACE_BT2020
#define RENODX_TONE_MAP_CLAMP_PEAK           renodx::color::convert::COLOR_SPACE_BT2020
#define RENODX_SWAP_CHAIN_CLAMP_COLOR_SPACE  renodx::color::convert::COLOR_SPACE_BT2020

#ifndef __cplusplus
// Vulkan: RenoDX appends its data as push constants. DOOM's post-process pipelines
// have no push constants of their own, so the injection starts at offset 0.
struct PushData {
  ShaderInjectData shader_injection;
};
[[vk::push_constant]]
PushData gPush;
#define shader_injection gPush.shader_injection

#include "../../shaders/renodx.hlsl"
#endif

#endif  // SRC_DOOM2016_SHARED_H_
