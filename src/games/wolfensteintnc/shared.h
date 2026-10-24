#ifndef SRC_WOLFENSTEINTNC_SHARED_H_
#define SRC_WOLFENSTEINTNC_SHARED_H_

// RenoDX appends its push constants after the pipeline layout's own range. A layout that has none (measured for the
// tonemap 0xF3E7B60A: its shader declares the game's {int, int} block but never reads it) gets the data at byte 0;
// layouts that do have a game range get it after that range. A replacement shader defines the offset it was measured
// at before including this file; the swap chain proxy pass has its own layout and keeps the fallback of 0.
#ifndef PUSH_CONSTANTS_OFFSET
#define PUSH_CONSTANTS_OFFSET 0
#endif

// Must be 32bit aligned
// Should be 4x32
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
  float swap_chain_output_preset;
  float padding0;
  float padding1;
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
#define RENODX_GAMMA_CORRECTION              shader_injection.gamma_correction
#define RENODX_RENO_DRT_TONE_MAP_METHOD      renodx::tonemap::renodrt::config::tone_map_method::REINHARD

// The game encodes its output as sRGB (tonemap 0xF3E7B60A -> final pass 0x612E95DB). The intermediate (tonemap output,
// final pass input, swap chain proxy input) is BT.709 and follows the gamma correction setting: sRGB when off, 2.2 when on.
#define RENODX_INTERMEDIATE_ENCODING    (RENODX_GAMMA_CORRECTION + 1.f)
#define RENODX_SWAP_CHAIN_DECODING      RENODX_INTERMEDIATE_ENCODING
#define RENODX_SWAP_CHAIN_OUTPUT_PRESET shader_injection.swap_chain_output_preset

#ifndef __cplusplus
#ifdef __SLANG__
struct PushData {
  [[vk::offset(PUSH_CONSTANTS_OFFSET)]]
  ShaderInjectData shader_injection;
};

[[vk::push_constant]]
PushData gPush;
#define shader_injection gPush.shader_injection
#else
#if ((__SHADER_TARGET_MAJOR == 5 && __SHADER_TARGET_MINOR >= 1) || __SHADER_TARGET_MAJOR >= 6)
cbuffer injected_buffer : register(b13, space50) {
#elif (__SHADER_TARGET_MAJOR < 5) || ((__SHADER_TARGET_MAJOR == 5) && (__SHADER_TARGET_MINOR < 1))
cbuffer injected_buffer : register(b13) {
#endif
  ShaderInjectData shader_injection : packoffset(c0);
}
#endif

#include "../../shaders/renodx.hlsl"

#endif

#endif  // SRC_WOLFENSTEINTNC_SHARED_H_
