#ifndef SRC_CALLOFJUAREZGUNSLINGER_SHARED_H_
#define SRC_CALLOFJUAREZGUNSLINGER_SHARED_H_

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
  float glow_strength;
  float padding1;

  float highlight_start;
  float sun_brightness;
  float shadow_lift;
  float dof_strength;

  float sky_hdr_boost;
  float copy_full_res;
  float padding0;
  float highlight_gain;

  float sun_reach;
  float sun_falloff;
  float sun_halo;
  float sun_halo_radius;

  float sun_profile;
  float sun_uv_x;
  float sun_uv_y;
  float sun_disc_radius;
};

#ifndef __cplusplus
#if (__SHADER_TARGET_MAJOR == 3)

// DX9 (ps_3_0): the add-on pushes the injection data into pixel shader
// constants c50..c59 (constant_buffer_offset = 50 * 4 in addon.cpp).
float4 shader_injection[12] : register(c50);

#define RENODX_PEAK_WHITE_NITS                 shader_injection[0][0]
#define RENODX_DIFFUSE_WHITE_NITS              shader_injection[0][1]
#define RENODX_GRAPHICS_WHITE_NITS             shader_injection[0][2]
#define RENODX_COLOR_GRADE_STRENGTH            shader_injection[0][3]
#define RENODX_TONE_MAP_TYPE                   shader_injection[1][0]
#define RENODX_TONE_MAP_EXPOSURE               shader_injection[1][1]
#define RENODX_TONE_MAP_HIGHLIGHTS             shader_injection[1][2]
#define RENODX_TONE_MAP_SHADOWS                shader_injection[1][3]
#define RENODX_TONE_MAP_CONTRAST               shader_injection[2][0]
#define RENODX_TONE_MAP_SATURATION             shader_injection[2][1]
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION   shader_injection[2][2]
#define RENODX_TONE_MAP_BLOWOUT                shader_injection[2][3]
#define RENODX_TONE_MAP_FLARE                  shader_injection[3][0]
#define RENODX_TONE_MAP_HUE_CORRECTION         shader_injection[3][1]
#define RENODX_TONE_MAP_HUE_SHIFT              shader_injection[3][2]
#define RENODX_TONE_MAP_WORKING_COLOR_SPACE    shader_injection[3][3]
#define RENODX_TONE_MAP_CLAMP_COLOR_SPACE      shader_injection[4][0]
#define RENODX_TONE_MAP_CLAMP_PEAK             shader_injection[4][1]
#define RENODX_TONE_MAP_HUE_PROCESSOR          shader_injection[4][2]
#define RENODX_TONE_MAP_PER_CHANNEL            shader_injection[4][3]
#define RENODX_GAMMA_CORRECTION                shader_injection[5][0]
#define RENODX_INTERMEDIATE_ENCODING           shader_injection[5][2]
#define RENODX_SWAP_CHAIN_DECODING             shader_injection[6][0]
#define RENODX_SWAP_CHAIN_GAMMA_CORRECTION     shader_injection[6][1]
#define RENODX_SWAP_CHAIN_CUSTOM_COLOR_SPACE   shader_injection[6][2]
#define RENODX_SWAP_CHAIN_CLAMP_COLOR_SPACE    shader_injection[6][3]
#define RENODX_SWAP_CHAIN_ENCODING             shader_injection[7][0]
#define RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE shader_injection[7][1]
#define CUSTOM_GLOW_STRENGTH                   shader_injection[7][2]
#define CUSTOM_HIGHLIGHT_START                 shader_injection[8][0]
#define CUSTOM_SHADOW_LIFT                     shader_injection[8][2]
#define CUSTOM_DOF_STRENGTH                    shader_injection[8][3]
#define CUSTOM_SUN_BRIGHTNESS                  shader_injection[8][1]
#define CUSTOM_SKY_HDR_BOOST                   shader_injection[9][0]
#define CUSTOM_COPY_FULL_RES                   shader_injection[9][1]
#define CUSTOM_HIGHLIGHT_GAIN                  shader_injection[9][3]
#define CUSTOM_SUN_REACH                       shader_injection[10][0]
#define CUSTOM_SUN_FALLOFF                     shader_injection[10][1]
#define CUSTOM_SUN_HALO                        shader_injection[10][2]
#define CUSTOM_SUN_HALO_RADIUS                 shader_injection[10][3]
#define CUSTOM_SUN_PROFILE                     shader_injection[11][0]
#define CUSTOM_SUN_UV_X                        shader_injection[11][1]
#define CUSTOM_SUN_UV_Y                        shader_injection[11][2]
#define CUSTOM_SUN_DISC_RADIUS                 shader_injection[11][3]

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
#define RENODX_COLOR_GRADE_STRENGTH            shader_injection.color_grade_strength
#define RENODX_TONE_MAP_TYPE                   shader_injection.tone_map_type
#define RENODX_TONE_MAP_EXPOSURE               shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS             shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS                shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST               shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION             shader_injection.tone_map_saturation
#define RENODX_TONE_MAP_HIGHLIGHT_SATURATION   shader_injection.tone_map_highlight_saturation
#define RENODX_TONE_MAP_BLOWOUT                shader_injection.tone_map_blowout
#define RENODX_TONE_MAP_FLARE                  shader_injection.tone_map_flare
#define RENODX_TONE_MAP_HUE_CORRECTION         shader_injection.tone_map_hue_correction
#define RENODX_TONE_MAP_HUE_SHIFT              shader_injection.tone_map_hue_shift
#define RENODX_TONE_MAP_WORKING_COLOR_SPACE    shader_injection.tone_map_working_color_space
#define RENODX_TONE_MAP_CLAMP_COLOR_SPACE      shader_injection.tone_map_clamp_color_space
#define RENODX_TONE_MAP_CLAMP_PEAK             shader_injection.tone_map_clamp_peak
#define RENODX_TONE_MAP_HUE_PROCESSOR          shader_injection.tone_map_hue_processor
#define RENODX_TONE_MAP_PER_CHANNEL            shader_injection.tone_map_per_channel
#define RENODX_GAMMA_CORRECTION                shader_injection.gamma_correction
#define RENODX_INTERMEDIATE_ENCODING           shader_injection.intermediate_encoding
#define RENODX_SWAP_CHAIN_DECODING             shader_injection.swap_chain_decoding
#define RENODX_SWAP_CHAIN_GAMMA_CORRECTION     shader_injection.swap_chain_gamma_correction
#define RENODX_SWAP_CHAIN_CUSTOM_COLOR_SPACE   shader_injection.swap_chain_custom_color_space
#define RENODX_SWAP_CHAIN_CLAMP_COLOR_SPACE    shader_injection.swap_chain_clamp_color_space
#define RENODX_SWAP_CHAIN_ENCODING             shader_injection.swap_chain_encoding
#define RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE shader_injection.swap_chain_encoding_color_space
#define CUSTOM_GLOW_STRENGTH                   shader_injection.glow_strength
#define CUSTOM_HIGHLIGHT_START                 shader_injection.highlight_start
#define CUSTOM_SHADOW_LIFT                     shader_injection.shadow_lift
#define CUSTOM_DOF_STRENGTH                    shader_injection.dof_strength
#define CUSTOM_SUN_BRIGHTNESS                  shader_injection.sun_brightness
#define CUSTOM_SKY_HDR_BOOST                   shader_injection.sky_hdr_boost
#define CUSTOM_COPY_FULL_RES                   shader_injection.copy_full_res
#define CUSTOM_HIGHLIGHT_GAIN                  shader_injection.highlight_gain
#define CUSTOM_SUN_REACH                       shader_injection.sun_reach
#define CUSTOM_SUN_FALLOFF                     shader_injection.sun_falloff
#define CUSTOM_SUN_HALO                        shader_injection.sun_halo
#define CUSTOM_SUN_HALO_RADIUS                 shader_injection.sun_halo_radius
#define CUSTOM_SUN_PROFILE                     shader_injection.sun_profile
#define CUSTOM_SUN_UV_X                        shader_injection.sun_uv_x
#define CUSTOM_SUN_UV_Y                        shader_injection.sun_uv_y
#define CUSTOM_SUN_DISC_RADIUS                 shader_injection.sun_disc_radius

#endif

#define RENODX_RENO_DRT_TONE_MAP_METHOD renodx::tonemap::renodrt::config::tone_map_method::REINHARD

#include "../../shaders/renodx.hlsl"

#endif  // __cplusplus

#endif  // SRC_CALLOFJUAREZGUNSLINGER_SHARED_H_
