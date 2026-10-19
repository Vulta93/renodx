#ifndef SRC_GAMES_BINARYDOMAIN_SHARED_H_
#define SRC_GAMES_BINARYDOMAIN_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
struct ShaderInjectData {
  float peak_white_nits;
  float diffuse_white_nits;
  float graphics_white_nits;
  float color_grade_strength;

  float tone_map_type;
  float tone_map_exposure;
  float tone_map_highlights;
  float tone_map_shadows;

  float tone_map_contrast;
  float tone_map_saturation;
  float tone_map_highlight_saturation;
  float tone_map_blowout;

  float tone_map_flare;
  float tone_map_hue_correction;
  float tone_map_hue_shift;
  float tone_map_working_color_space;

  float tone_map_clamp_color_space;
  float tone_map_clamp_peak;
  float tone_map_hue_processor;
  float tone_map_per_channel;

  float gamma_correction;
  float intermediate_scaling;
  float intermediate_encoding;
  float intermediate_color_space;

  float swap_chain_decoding;
  float swap_chain_gamma_correction;
  float swap_chain_custom_color_space;
  float swap_chain_clamp_color_space;

  float swap_chain_encoding;
  float swap_chain_encoding_color_space;
  float hud_clamp;
  float padding1;
};

#ifndef __cplusplus
#if (__SHADER_TARGET_MAJOR == 3)

// ps_3_0 replaced game shaders: constants c100..c107, pushed from ShaderInjectData before each replaced draw (the game uses
// c0-c13, c32-c41, c48-c49, c57-c71 and c192-c223; c57 is its punch fade parameter, so c50-c57 must not be used).
float4 shader_injection[8] : register(c100);

#define RENODX_PEAK_WHITE_NITS                 shader_injection[0][0]
#define RENODX_DIFFUSE_WHITE_NITS              shader_injection[0][1]
#define RENODX_GRAPHICS_WHITE_NITS             shader_injection[0][2]
#define RENODX_TONE_MAP_TYPE                   shader_injection[1][0]
#define RENODX_TONE_MAP_EXPOSURE               shader_injection[1][1]
#define RENODX_TONE_MAP_HIGHLIGHTS             shader_injection[1][2]
#define RENODX_TONE_MAP_SHADOWS                shader_injection[1][3]
#define RENODX_TONE_MAP_CONTRAST               shader_injection[2][0]
#define RENODX_TONE_MAP_SATURATION             shader_injection[2][1]
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION   shader_injection[2][2]
#define RENODX_TONE_MAP_BLOWOUT                shader_injection[2][3]
#define RENODX_TONE_MAP_FLARE                  shader_injection[3][0]
#define RENODX_GAMMA_CORRECTION                shader_injection[5][0]
#define RENODX_INTERMEDIATE_ENCODING           shader_injection[5][2]
#define RENODX_SWAP_CHAIN_DECODING             shader_injection[6][0]
#define RENODX_SWAP_CHAIN_GAMMA_CORRECTION     shader_injection[6][1]
#define RENODX_SWAP_CHAIN_CUSTOM_COLOR_SPACE   shader_injection[6][2]
#define RENODX_SWAP_CHAIN_CLAMP_COLOR_SPACE    shader_injection[6][3]
#define RENODX_SWAP_CHAIN_ENCODING             shader_injection[7][0]
#define RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE shader_injection[7][1]
#define RENODX_HUD_CLAMP                       shader_injection[7][2]

#else
#if ((__SHADER_TARGET_MAJOR == 5 && __SHADER_TARGET_MINOR >= 1) || __SHADER_TARGET_MAJOR >= 6)
cbuffer shader_injection : register(b13, space50) {
#elif (__SHADER_TARGET_MAJOR < 5) || ((__SHADER_TARGET_MAJOR == 5) && (__SHADER_TARGET_MINOR < 1))
cbuffer shader_injection : register(b13) {
#endif
  ShaderInjectData shader_injection : packoffset(c0);
}

#define RENODX_PEAK_WHITE_NITS                 shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS              shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS             shader_injection.graphics_white_nits
#define RENODX_TONE_MAP_TYPE                   shader_injection.tone_map_type
#define RENODX_GAMMA_CORRECTION                shader_injection.gamma_correction
#define RENODX_INTERMEDIATE_ENCODING           shader_injection.intermediate_encoding
#define RENODX_SWAP_CHAIN_DECODING             shader_injection.swap_chain_decoding
#define RENODX_SWAP_CHAIN_GAMMA_CORRECTION     shader_injection.swap_chain_gamma_correction
#define RENODX_SWAP_CHAIN_CUSTOM_COLOR_SPACE   shader_injection.swap_chain_custom_color_space
#define RENODX_SWAP_CHAIN_CLAMP_COLOR_SPACE    shader_injection.swap_chain_clamp_color_space
#define RENODX_SWAP_CHAIN_ENCODING             shader_injection.swap_chain_encoding
#define RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE shader_injection.swap_chain_encoding_color_space
#endif  // __SHADER_TARGET_MAJOR == 3

#include "../../shaders/renodx.hlsl"

#endif  // __cplusplus

#endif  // SRC_GAMES_BINARYDOMAIN_SHARED_H_
