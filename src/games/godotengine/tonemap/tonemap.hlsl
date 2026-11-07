// Godot 4 desktop Forward+ tonemap pass (servers/rendering/renderer_rd/shaders/effects/tonemap.glsl), ported to HLSL
// from Godot's MIT source (4.0.4 to 4.7.2 stable; for this pass 4.2 matches 4.3; 4.0 and 4.1 differ in Params and always
// encode to sRGB, whose clamp stands in for 4.2's explicit glow clamp) because the Devkit could not decompile the DXIL.
// Godot builds four desktop variants with USE_GLOW_FILTER_BICUBIC and USE_1D_LUT; each per-hash file sets those and
// GODOT_VERSION (major * 100 + minor) and calls GodotTonemap. The registers are the ones Godot's NIR-to-DXIL path
// assigns (set * 100000000 + binding * 100000, push constants at b1700000000), read from the original shaders.
// Before 4.6 the operator output is clamped by an sRGB encode and glow (except mix) is blended after it in sRGB.
//
// Godot's tonemap.glsl:
// Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).
// Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.
// SPDX-License-Identifier: MIT

#ifndef GODOT_VERSION
#error "GODOT_VERSION must be defined by the per-hash file"
#endif

// The GLSL `Params` push-constant block (std430: 112 bytes in 4.0-4.1, 96 in 4.2-4.5, 112 from 4.6). Scalars keep
// every member at its std430 byte offset under both HLSL cbuffer packing (D3D12) and Slang's std430 push-constant
// layout (Vulkan): an HLSL array would start each element on a new 16-byte register, a std430 float3 on a 16-byte
// boundary.
#if GODOT_VERSION < 402
// 4.0-4.1 (Vulkan only): one bool per feature instead of `flags`, read through GodotFlags().
struct Params {
  float3 bcs;
  uint use_bcs;

  uint use_glow;
  uint use_auto_exposure;
  uint use_color_correction;
  uint tonemapper;

  uint2 glow_texture_size;
  float glow_intensity;
  float glow_map_strength;

  uint glow_mode;
  float glow_level_0;
  float glow_level_1;
  float glow_level_2;
  float glow_level_3;
  float glow_level_4;
  float glow_level_5;
  float glow_level_6;

  float exposure;
  float white;
  float auto_exposure_scale;
  float luminance_multiplier;

  float2 pixel_size;
  uint use_fxaa;
  uint use_debanding;
};
#else
struct Params {
  float3 bcs;
  uint flags;

  float2 pixel_size;
  uint tonemapper;
  float output_max_value;  // `uint pad` (0) before 4.7, where SDR output_max_value is the constant 1.

  uint2 glow_texture_size;
  float glow_intensity;
  float glow_map_strength;

  uint glow_mode;
  float glow_level_0;
  float glow_level_1;
  float glow_level_2;
  float glow_level_3;
  float glow_level_4;
  float glow_level_5;
  float glow_level_6;

  float exposure;
  float white;
  float auto_exposure_scale;
  float luminance_multiplier;

#if GODOT_VERSION >= 406
  float4 tonemapper_params;
#endif
};
#endif

#ifdef __SLANG__
// Vulkan: Godot's own block at offset 0 of the push-constant range, RenoDX's data right after it (shared.h).
#define GODOT_PUSH_PARAMS Params
#if GODOT_VERSION >= 406 || GODOT_VERSION < 402
#define PUSH_CONSTANTS_OFFSET 112
#else
#define PUSH_CONSTANTS_OFFSET 96
#endif
#endif

#include "../shared.h"

#ifdef __SLANG__
#define params push_data.godot_params

// Godot's GLSL combined image samplers, at their (set, binding).
[[vk::binding(0, 0)]] Sampler2D source_color;
[[vk::binding(0, 1)]] Sampler2D source_auto_exposure;
[[vk::binding(0, 2)]] Sampler2D source_glow;
[[vk::binding(1, 2)]] Sampler2D glow_map;
#ifdef USE_1D_LUT
[[vk::binding(0, 3)]] Sampler2D source_color_correction;
#else
[[vk::binding(0, 3)]] Sampler3D source_color_correction;
#endif

#define GODOT_SAMPLE(texture, uv)                              texture.Sample(uv)
#define GODOT_SAMPLE_LEVEL(texture, uv, lod)                   texture.SampleLevel(uv, lod)
#define GODOT_SAMPLE_LEVEL_OFFSET(texture, uv, lod, offset)    texture.SampleLevel(uv, lod, offset)
#define GODOT_LOAD(texture, coord)                             texture.Load(coord)
#else
ConstantBuffer<Params> params : register(b1700000000);

Texture2D<float4> source_color : register(t0);
SamplerState source_color_sampler : register(s0);
Texture2D<float4> source_auto_exposure : register(t100000000);
Texture2D<float4> source_glow : register(t200000000);
SamplerState source_glow_sampler : register(s200000000);
Texture2D<float4> glow_map : register(t200100000);
SamplerState glow_map_sampler : register(s200100000);
#ifdef USE_1D_LUT
Texture2D<float4> source_color_correction : register(t300000000);
#else
Texture3D<float4> source_color_correction : register(t300000000);
#endif
SamplerState source_color_correction_sampler : register(s300000000);

#define GODOT_SAMPLE(texture, uv)                              texture.Sample(texture##_sampler, uv)
#define GODOT_SAMPLE_LEVEL(texture, uv, lod)                   texture.SampleLevel(texture##_sampler, uv, lod)
#define GODOT_SAMPLE_LEVEL_OFFSET(texture, uv, lod, offset)    texture.SampleLevel(texture##_sampler, uv, lod, offset)
#define GODOT_LOAD(texture, coord)                             texture.Load(coord)
#endif

#define FLAG_USE_BCS              (1u << 0)
#define FLAG_USE_GLOW             (1u << 1)
#define FLAG_USE_AUTO_EXPOSURE    (1u << 2)
#define FLAG_USE_COLOR_CORRECTION (1u << 3)
#define FLAG_USE_FXAA             (1u << 4)
#define FLAG_USE_8_BIT_DEBANDING  (1u << 5)  // FLAG_USE_DEBANDING before 4.5
#if GODOT_VERSION == 405
#define FLAG_USE_10_BIT_DEBANDING (1u << 6)
#define FLAG_CONVERT_TO_SRGB      (1u << 7)
#else
#define FLAG_CONVERT_TO_SRGB (1u << 6)
#endif

#if GODOT_VERSION < 402
// 4.0-4.1 always encode to sRGB (HDR 2D viewports came in 4.2).
uint GodotFlags() {
  uint flags = FLAG_CONVERT_TO_SRGB;
  if (params.use_bcs != 0u) flags |= FLAG_USE_BCS;
  if (params.use_glow != 0u) flags |= FLAG_USE_GLOW;
  if (params.use_auto_exposure != 0u) flags |= FLAG_USE_AUTO_EXPOSURE;
  if (params.use_color_correction != 0u) flags |= FLAG_USE_COLOR_CORRECTION;
  if (params.use_fxaa != 0u) flags |= FLAG_USE_FXAA;
  if (params.use_debanding != 0u) flags |= FLAG_USE_8_BIT_DEBANDING;
  return flags;
}
#else
uint GodotFlags() {
  return params.flags;
}
#endif

#define TONEMAPPER_LINEAR   0u
#define TONEMAPPER_REINHARD 1u
#define TONEMAPPER_FILMIC   2u
#define TONEMAPPER_ACES     3u
#define TONEMAPPER_AGX      4u

// Tone Mapper setting value after RenoDX's Vanilla/None/ACES/RenoDRT (addon.cpp).
#define TONE_MAP_TYPE_GODOT_HDR 4.f

#define GLOW_MODE_ADD       0u
#define GLOW_MODE_SCREEN    1u
#define GLOW_MODE_SOFTLIGHT 2u
#define GLOW_MODE_REPLACE   3u
#define GLOW_MODE_MIX       4u

#if GODOT_VERSION >= 407
#define GODOT_OUTPUT_MAX_VALUE params.output_max_value
#else
#define GODOT_OUTPUT_MAX_VALUE 1.f
#endif

// The parameters Godot's CPU derives from output_max_value (4.7 environment_storage.cpp): the pushed ones for the
// vanilla curve, recomputed for a "Godot HDR" curve. Before 4.6 the shader derives them from `white` itself.
struct TonemapCurve {
  float output_max_value;
  float reinhard_white_squared;
  float agx_awp_w;
};

TonemapCurve VanillaCurve() {
  TonemapCurve curve;
  curve.output_max_value = GODOT_OUTPUT_MAX_VALUE;
#if GODOT_VERSION >= 406
  curve.reinhard_white_squared = params.tonemapper_params.x;
  curve.agx_awp_w = params.tonemapper_params.w;
#else
  curve.reinhard_white_squared = params.white * params.white;
  curve.agx_awp_w = 0.f;  // AgX before 4.6 is a fixed polynomial
#endif
  return curve;
}

// Godot 4.7's HDR form; at output_max_value 1 it equals 4.6's (w^2 c + c^2) / (w^2 c + w^2).
float3 TonemapReinhard(float3 color, TonemapCurve curve) {
#if GODOT_VERSION < 404
  // Godot 4.0-4.3's own form (2c / (1 + c) at white 1, so never an identity).
  return (params.white * color + color) / (color * params.white + params.white);
#else
  return color * (1.0f + color / curve.reinhard_white_squared) / (1.0f + color / curve.output_max_value);
#endif
}

float3 TonemapFilmic(float3 color) {
  const float exposure_bias = 2.0f;
  const float A = 0.22f * exposure_bias * exposure_bias;
  const float B = 0.30f * exposure_bias;
  const float C = 0.10f;
  const float D = 0.20f;
  const float E = 0.01f;
  const float F = 0.30f;

  float3 color_tonemapped = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
#if GODOT_VERSION >= 406
  float white_tonemapped = params.tonemapper_params.x;
#else
  float white = params.white;
  float white_tonemapped = ((white * (A * white + C * B) + D * E) / (white * (A * white + B) + D * F)) - E / F;
#endif
  return color_tonemapped / white_tonemapped;
}

float3 TonemapAces(float3 color) {
  const float exposure_bias = 1.8f;
  const float A = 0.0245786f;
  const float B = 0.000090537f;
  const float C = 0.983729f;
  const float D = 0.432951f;
  const float E = 0.238081f;

  // GLSL mat3 literals are column-major and `color *= m` multiplies a row vector, so with the same literal order
  // as rows these are mul(m, color).
  const float3x3 rgb_to_rrt = float3x3(
      0.59719f * exposure_bias, 0.35458f * exposure_bias, 0.04823f * exposure_bias,
      0.07600f * exposure_bias, 0.90834f * exposure_bias, 0.01566f * exposure_bias,
      0.02840f * exposure_bias, 0.13383f * exposure_bias, 0.83777f * exposure_bias);
  const float3x3 odt_to_rgb = float3x3(
      1.60475f, -0.53108f, -0.07367f,
      -0.10208f, 1.10813f, -0.00605f,
      -0.00327f, -0.07276f, 1.07602f);

  color = mul(rgb_to_rrt, color);
  float3 color_tonemapped = (color * (color + A) - B) / (color * (C * color + D) + E);
  color_tonemapped = mul(odt_to_rgb, color_tonemapped);
#if GODOT_VERSION >= 406
  float white_tonemapped = params.tonemapper_params.x;
#else
  float white = params.white * exposure_bias;
  float white_tonemapped = (white * (white + A) - B) / (white * (C * white + D) + E);
#endif
  return color_tonemapped / white_tonemapped;
}

#if GODOT_VERSION < 406
// Before 4.6: polynomial approximation of EaryChow's AgX sigmoid (no white, no output_max_value).
float3 TonemapAgx(float3 color, TonemapCurve curve) {
  // GLSL `m * color` with column-major literals: mul(color, m) with the same literal order as rows.
  const float3x3 srgb_to_rec2020_agx_inset_matrix = float3x3(
      0.54490813676363087053, 0.14044005884001287035, 0.088827411851915368603,
      0.37377945959812267119, 0.75410959864013760045, 0.17887712465043811023,
      0.081384976686407536266, 0.10543358536857773485, 0.73224999956948382528);
  const float3x3 agx_outset_rec2020_to_srgb_matrix = float3x3(
      1.9645509602733325934, -0.29932243390911083839, -0.16436833806080403409,
      -0.85585845117807513559, 1.3264510741502356555, -0.23822464068860595117,
      -0.10886710826831608324, -0.027084020983874825605, 1.402665347143271889);
  const float min_ev = -12.4739311883324;
  const float max_ev = 4.02606881166759;

  color = max(color, 2e-10);
  color = mul(color, srgb_to_rec2020_agx_inset_matrix);
  color = clamp(log2(color), min_ev, max_ev);
  color = (color - min_ev) / (max_ev - min_ev);
  // agx_contrast_approx
  float3 x2 = color * color;
  float3 x4 = x2 * x2;
  color = 0.021 * color + 4.0111 * x2 - 25.682 * x2 * color + 70.359 * x4 - 74.778 * x4 * color + 27.069 * x4 * x2;
  color = pow(color, 2.4f);
  color = mul(color, agx_outset_rec2020_to_srgb_matrix);
  return color;
}
#else
float3 TonemapAgx(float3 color, TonemapCurve curve) {
  // GLSL `m * color` with column-major literals: mul(color, m) with the same literal order as rows.
  const float3x3 rec709_to_rec2020_agx_inset_matrix = float3x3(
      0.544814746488245, 0.140416948464053, 0.0888104196149096,
      0.373787398372697, 0.754137554567394, 0.178871756420858,
      0.0813978551390581, 0.105445496968552, 0.732317823964232);
  const float3x3 agx_outset_rec2020_to_rec709_matrix = float3x3(
      1.96488741169489, -0.299313364904742, -0.164352742528393,
      -0.855988495690215, 1.32639796461980, -0.238183969428088,
      -0.108898916004672, -0.0270845997150571, 1.40253671195648);

  // Crossover 0.18 is a constant in Godot; contrast, toe and slope do not depend on output_max_value.
  renodx::tonemap::allenwp::internal::Parameters awp;
  awp.output_max_value = curve.output_max_value;
  awp.awp_crossover_point = 0.18f;
  awp.awp_shoulder_max = awp.output_max_value - awp.awp_crossover_point;
  awp.awp_contrast = params.tonemapper_params.x;
  awp.awp_toe_a = params.tonemapper_params.y;
  awp.awp_slope = params.tonemapper_params.z;
  awp.awp_w = curve.agx_awp_w;

  color = mul(color, rec709_to_rec2020_agx_inset_matrix);
  color = renodx::tonemap::allenwp::internal::Curve(color, awp);
  color = min(awp.output_max_value, color);
  color = mul(color, agx_outset_rec2020_to_rec709_matrix);
  return color;
}
#endif

float3 ApplyTonemapping(float3 color, TonemapCurve curve) {
  if (params.tonemapper == TONEMAPPER_LINEAR) {
    return color;
  }

  color = max(0.f, color);

  if (params.tonemapper == TONEMAPPER_REINHARD) {
    return TonemapReinhard(color, curve);
  } else if (params.tonemapper == TONEMAPPER_FILMIC) {
    return TonemapFilmic(color);
  } else if (params.tonemapper == TONEMAPPER_ACES) {
    return TonemapAces(color);
  } else {
    return TonemapAgx(color, curve);
  }
}

#ifdef USE_GLOW_FILTER_BICUBIC
float W0(float a) {
  return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float W1(float a) {
  return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float W2(float a) {
  return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float W3(float a) {
  return (1.0f / 6.0f) * (a * a * a);
}

float G0(float a) {
  return W0(a) + W1(a);
}

float G1(float a) {
  return W2(a) + W3(a);
}

float H0(float a) {
  return -1.0f + W1(a) / (W0(a) + W1(a));
}

float H1(float a) {
  return 1.0f + W3(a) / (W2(a) + W3(a));
}

float4 SampleGlowLevel(float2 uv, uint lod) {
  float2 tex_size = float2(params.glow_texture_size >> lod);
  float2 pixel_size = 1.0f / tex_size;

  uv = uv * tex_size + 0.5f;

  float2 iuv = floor(uv);
  float2 fuv = frac(uv);

  float g0x = G0(fuv.x);
  float g1x = G1(fuv.x);
  float h0x = H0(fuv.x);
  float h1x = H1(fuv.x);
  float h0y = H0(fuv.y);
  float h1y = H1(fuv.y);

  float2 p0 = (float2(iuv.x + h0x, iuv.y + h0y) - 0.5f) * pixel_size;
  float2 p1 = (float2(iuv.x + h1x, iuv.y + h0y) - 0.5f) * pixel_size;
  float2 p2 = (float2(iuv.x + h0x, iuv.y + h1y) - 0.5f) * pixel_size;
  float2 p3 = (float2(iuv.x + h1x, iuv.y + h1y) - 0.5f) * pixel_size;

  return (G0(fuv.y) * (g0x * GODOT_SAMPLE_LEVEL(source_glow, p0, lod) + g1x * GODOT_SAMPLE_LEVEL(source_glow, p1, lod)))
         + (G1(fuv.y) * (g0x * GODOT_SAMPLE_LEVEL(source_glow, p2, lod) + g1x * GODOT_SAMPLE_LEVEL(source_glow, p3, lod)));
}
#else
float4 SampleGlowLevel(float2 uv, uint lod) {
  return GODOT_SAMPLE_LEVEL(source_glow, uv, lod);
}
#endif

// gather_glow() * glow_intensity, then the glow map.
float3 SampleGlow(float2 uv) {
  float glow_levels[7] = {
    params.glow_level_0, params.glow_level_1, params.glow_level_2, params.glow_level_3,
    params.glow_level_4, params.glow_level_5, params.glow_level_6,
  };
  float3 glow = 0.0f;
  [unroll]
  for (uint level = 0; level < 7; ++level) {
    if (glow_levels[level] > 0.0001) {
      glow += SampleGlowLevel(uv, level).rgb * glow_levels[level];
    }
  }
  glow = glow * params.luminance_multiplier * params.glow_intensity;
  if (params.glow_map_strength > 0.001) {
    glow = lerp(glow, GODOT_SAMPLE(glow_map, uv).rgb * glow, params.glow_map_strength);
  }
  return glow;
}

// Does not handle the mix blend mode. Before 4.6 it blends tone-mapped, sRGB-encoded glow and ignores `white`.
float3 ApplyGlow(float3 color, float3 glow, float white) {
  if (params.glow_mode == GLOW_MODE_ADD) {
    return color + glow;
  } else if (params.glow_mode == GLOW_MODE_SCREEN) {
#if GODOT_VERSION >= 406
    glow = clamp(glow, 0.0, white);
    return color + glow - (color * glow / white);
#else
    glow = saturate(glow);
    return max((color + glow) - (color * glow), 0.0);
#endif
  } else if (params.glow_mode == GLOW_MODE_SOFTLIGHT) {
    glow = saturate(glow);
#if GODOT_VERSION >= 406
    float3 soft_light_base = select(color <= 0.25f, ((16.0f * color - 12.0f) * color + 4.0f) * color, sqrt(color));
    return select(color > 1.0f, color, color + glow * (soft_light_base - color));
#else
    glow = glow * 0.5f + 0.5f;
    float3 darken = color - (1.0f - 2.0f * glow) * color * (1.0f - color);
    float3 lighten_dark = color + (2.0f * glow - 1.0f) * (4.0f * color * (4.0f * color + 1.0f) * (color - 1.0f) + 7.0f * color);
    float3 lighten = color + (2.0f * glow - 1.0f) * (sqrt(color) - color);
    return select(glow <= 0.5f, darken, select(color <= 0.25f, lighten_dark, lighten));
#endif
  } else {
    return glow;
  }
}

float3 ApplyColorCorrection(float3 color) {
#ifdef USE_1D_LUT
  color.r = GODOT_SAMPLE(source_color_correction, float2(color.r, 0.0f)).r;
  color.g = GODOT_SAMPLE(source_color_correction, float2(color.g, 0.0f)).g;
  color.b = GODOT_SAMPLE(source_color_correction, float2(color.b, 0.0f)).b;
  return color;
#else
  return GODOT_SAMPLE_LEVEL(source_color_correction, color, 0.0).rgb;
#endif
}

#if GODOT_VERSION < 405
// Godot 4.0-4.4 FXAA (reduced FXAA 3.x, 5 taps).
float3 DoFxaa(float3 color, float exposure, float2 uv_interp) {
  const float FXAA_REDUCE_MIN = (1.0 / 128.0);
  const float FXAA_REDUCE_MUL = (1.0 / 8.0);
  const float FXAA_SPAN_MAX = 8.0;
  const float scale = exposure * params.luminance_multiplier;

  float3 rgb_nw = GODOT_SAMPLE_LEVEL(source_color, uv_interp + float2(-0.5, -0.5) * params.pixel_size, 0.0).rgb * scale;
  float3 rgb_ne = GODOT_SAMPLE_LEVEL(source_color, uv_interp + float2(0.5, -0.5) * params.pixel_size, 0.0).rgb * scale;
  float3 rgb_sw = GODOT_SAMPLE_LEVEL(source_color, uv_interp + float2(-0.5, 0.5) * params.pixel_size, 0.0).rgb * scale;
  float3 rgb_se = GODOT_SAMPLE_LEVEL(source_color, uv_interp + float2(0.5, 0.5) * params.pixel_size, 0.0).rgb * scale;
  const float3 luma = float3(0.299, 0.587, 0.114);
  float luma_nw = dot(rgb_nw, luma);
  float luma_ne = dot(rgb_ne, luma);
  float luma_sw = dot(rgb_sw, luma);
  float luma_se = dot(rgb_se, luma);
  float luma_m = dot(color, luma);
  float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
  float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

  float2 dir;
  dir.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
  dir.y = ((luma_nw + luma_sw) - (luma_ne + luma_se));

  float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
  float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
  dir = min(FXAA_SPAN_MAX, max(-FXAA_SPAN_MAX, dir * rcp_dir_min)) * params.pixel_size;

  float3 rgb_a = 0.5 * exposure
                 * (GODOT_SAMPLE_LEVEL(source_color, uv_interp + dir * (1.0 / 3.0 - 0.5), 0.0).rgb
                    + GODOT_SAMPLE_LEVEL(source_color, uv_interp + dir * (2.0 / 3.0 - 0.5), 0.0).rgb)
                 * params.luminance_multiplier;
  float3 rgb_b = rgb_a * 0.5
                 + 0.25 * exposure
                       * (GODOT_SAMPLE_LEVEL(source_color, uv_interp + dir * -0.5, 0.0).rgb
                          + GODOT_SAMPLE_LEVEL(source_color, uv_interp + dir * 0.5, 0.0).rgb)
                       * params.luminance_multiplier;

  float luma_b = dot(rgb_b, luma);
  if ((luma_b < luma_min) || (luma_b > luma_max)) {
    return rgb_a;
  }
  return rgb_b;
}
#else
// FXAA 3.11 compact (Godot's port of kosua20/Rendu, MIT; NVIDIA FXAA 3.11 by Timothy Lottes, BSD-3).
float FxaaQuality(float q) {
  return (q < 5 ? 1.0 : (q > 5 ? (q < 10 ? 2.0 : (q < 11 ? 4.0 : 8.0)) : 1.5));
}

float Rgb2Luma(float3 rgb) {
  return sqrt(dot(rgb, float3(0.299, 0.587, 0.114)));
}

float FxaaLuma(float2 uv, float exposure) {
  return Rgb2Luma(GODOT_SAMPLE_LEVEL(source_color, uv, 0.0).rgb * exposure * params.luminance_multiplier);
}

// A macro, not a function: texel offsets must be compile-time constants (Slang/SPIR-V).
#define FXAA_LUMA_OFFSET(uv, exposure, offset)   Rgb2Luma(GODOT_SAMPLE_LEVEL_OFFSET(source_color, uv, 0.0, offset).rgb * (exposure) * params.luminance_multiplier)

float3 DoFxaa(float3 color, float exposure, float2 uv_interp) {
  const float EDGE_THRESHOLD_MIN = 0.0312;
  const float EDGE_THRESHOLD_MAX = 0.125;
  const int ITERATIONS = 12;
  const float SUBPIXEL_QUALITY = 0.75;

  float luma_up = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(0, 1));
  float luma_down = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(0, -1));
  float luma_left = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(-1, 0));
  float luma_right = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(1, 0));

  float luma_center = Rgb2Luma(color);

  float luma_min = min(luma_center, min(min(luma_up, luma_down), min(luma_left, luma_right)));
  float luma_max = max(luma_center, max(max(luma_up, luma_down), max(luma_left, luma_right)));

  float luma_range = luma_max - luma_min;

  if (luma_range < max(EDGE_THRESHOLD_MIN, luma_max * EDGE_THRESHOLD_MAX)) {
    return color;
  }

  float luma_down_left = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(-1, -1));
  float luma_up_right = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(1, 1));
  float luma_up_left = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(-1, 1));
  float luma_down_right = FXAA_LUMA_OFFSET(uv_interp, exposure, int2(1, -1));

  float luma_down_up = luma_down + luma_up;
  float luma_left_right = luma_left + luma_right;

  float luma_left_corners = luma_down_left + luma_up_left;
  float luma_down_corners = luma_down_left + luma_down_right;
  float luma_right_corners = luma_down_right + luma_up_right;
  float luma_up_corners = luma_up_right + luma_up_left;

  float edge_horizontal = abs(-2.0 * luma_left + luma_left_corners) + abs(-2.0 * luma_center + luma_down_up) * 2.0 + abs(-2.0 * luma_right + luma_right_corners);
  float edge_vertical = abs(-2.0 * luma_up + luma_up_corners) + abs(-2.0 * luma_center + luma_left_right) * 2.0 + abs(-2.0 * luma_down + luma_down_corners);

  bool is_horizontal = (edge_horizontal >= edge_vertical);

  float step_length = is_horizontal ? params.pixel_size.y : params.pixel_size.x;

  float luma1 = is_horizontal ? luma_down : luma_left;
  float luma2 = is_horizontal ? luma_up : luma_right;
  float gradient1 = luma1 - luma_center;
  float gradient2 = luma2 - luma_center;

  bool is1_steepest = abs(gradient1) >= abs(gradient2);

  float gradient_scaled = 0.25 * max(abs(gradient1), abs(gradient2));

  float luma_local_average = 0.0;
  if (is1_steepest) {
    step_length = -step_length;
    luma_local_average = 0.5 * (luma1 + luma_center);
  } else {
    luma_local_average = 0.5 * (luma2 + luma_center);
  }

  float2 current_uv = uv_interp;
  if (is_horizontal) {
    current_uv.y += step_length * 0.5;
  } else {
    current_uv.x += step_length * 0.5;
  }

  float2 offset = is_horizontal ? float2(params.pixel_size.x, 0.0) : float2(0.0, params.pixel_size.y);
  float2 uv1 = current_uv - offset * FxaaQuality(0);
  float2 uv2 = current_uv + offset * FxaaQuality(0);

  float luma_end1 = FxaaLuma(uv1, exposure);
  float luma_end2 = FxaaLuma(uv2, exposure);
  luma_end1 -= luma_local_average;
  luma_end2 -= luma_local_average;

  bool reached1 = abs(luma_end1) >= gradient_scaled;
  bool reached2 = abs(luma_end2) >= gradient_scaled;
  bool reached_both = reached1 && reached2;

  if (!reached1) {
    uv1 -= offset * FxaaQuality(1);
  }
  if (!reached2) {
    uv2 += offset * FxaaQuality(1);
  }

  if (!reached_both) {
    for (int i = 2; i < ITERATIONS; i++) {
      if (!reached1) {
        luma_end1 = FxaaLuma(uv1, exposure);
        luma_end1 = luma_end1 - luma_local_average;
      }
      if (!reached2) {
        luma_end2 = FxaaLuma(uv2, exposure);
        luma_end2 = luma_end2 - luma_local_average;
      }
      reached1 = abs(luma_end1) >= gradient_scaled;
      reached2 = abs(luma_end2) >= gradient_scaled;
      reached_both = reached1 && reached2;
      if (!reached1) {
        uv1 -= offset * FxaaQuality(i);
      }
      if (!reached2) {
        uv2 += offset * FxaaQuality(i);
      }
      if (reached_both) {
        break;
      }
    }
  }

  float distance1 = is_horizontal ? (uv_interp.x - uv1.x) : (uv_interp.y - uv1.y);
  float distance2 = is_horizontal ? (uv2.x - uv_interp.x) : (uv2.y - uv_interp.y);

  bool is_direction1 = distance1 < distance2;
  float distance_final = min(distance1, distance2);

  float edge_thickness = (distance1 + distance2);

  bool is_luma_center_smaller = luma_center < luma_local_average;

  bool correct_variation1 = (luma_end1 < 0.0) != is_luma_center_smaller;
  bool correct_variation2 = (luma_end2 < 0.0) != is_luma_center_smaller;

  bool correct_variation = is_direction1 ? correct_variation1 : correct_variation2;

  float pixel_offset = -distance_final / edge_thickness + 0.5;

  float final_offset = correct_variation ? pixel_offset : 0.0;

  float luma_average = (1.0 / 12.0) * (2.0 * (luma_down_up + luma_left_right) + luma_left_corners + luma_right_corners);

  float sub_pixel_offset1 = saturate(abs(luma_average - luma_center) / luma_range);
  float sub_pixel_offset2 = (-2.0 * sub_pixel_offset1 + 3.0) * sub_pixel_offset1 * sub_pixel_offset1;

  float sub_pixel_offset_final = sub_pixel_offset2 * sub_pixel_offset2 * SUBPIXEL_QUALITY;

  final_offset = max(final_offset, sub_pixel_offset_final);

  float2 final_uv = uv_interp;
  if (is_horizontal) {
    final_uv.y += final_offset * step_length;
  } else {
    final_uv.x += final_offset * step_length;
  }

  return GODOT_SAMPLE_LEVEL(source_color, final_uv, 0.0).rgb * exposure * params.luminance_multiplier;
}
#endif

// Everything Godot does after apply_tonemapping. The result is encoded the way Godot writes it: sRGB when
// FLAG_CONVERT_TO_SRGB is set (SDR viewport), linear otherwise (HDR 2D).
// 4.6+: soft-light glow, then (adjustments on) brightness, sRGB encode, contrast, saturation and the colour-correction
// LUT, else the sRGB encode alone.
// Before 4.6: clamping sRGB encode, glow (all modes but mix, tone mapped and encoded the same way), BCS in that
// encoding, then the LUT.
float3 ApplyPostTonemap(float3 color, float2 uv) {
#if GODOT_VERSION < 406
  const bool convert_to_srgb = (GodotFlags() & FLAG_CONVERT_TO_SRGB) != 0u;
  if (convert_to_srgb) {
    color = renodx::color::srgb::Encode(saturate(color));
  }
  if ((GodotFlags() & FLAG_USE_GLOW) != 0u && params.glow_mode != GLOW_MODE_MIX) {
    float3 glow = ApplyTonemapping(SampleGlow(uv), VanillaCurve());
    if (convert_to_srgb) {
      glow = renodx::color::srgb::Encode(saturate(glow));
    }
    color = ApplyGlow(color, glow, params.white);
  }
  if ((GodotFlags() & FLAG_USE_BCS) != 0u) {
    color = color * params.bcs.x;
    color = lerp(0.5f, color, params.bcs.y);
    color = lerp(dot(float3(1.0f, 1.0f, 1.0f), color) * 0.33333f, color, params.bcs.z);
  }
  if ((GodotFlags() & FLAG_USE_COLOR_CORRECTION) != 0u) {
#if GODOT_VERSION >= 405
    if (!convert_to_srgb) {
      color = renodx::color::srgb::Encode(saturate(color));
    }
#endif
    color = ApplyColorCorrection(color);
  }
  return color;
#else
  if ((GodotFlags() & FLAG_USE_GLOW) != 0u && params.glow_mode == GLOW_MODE_SOFTLIGHT) {
    color = ApplyGlow(color, ApplyTonemapping(SampleGlow(uv), VanillaCurve()), params.white);
  }

  if ((GodotFlags() & FLAG_USE_BCS) != 0u) {
    color = color * params.bcs.x;
    color = renodx::color::srgb::Encode(color);
    color = lerp(0.5f, color, params.bcs.y);
    color = lerp(dot(float3(1.0f, 1.0f, 1.0f), color) * (1.0 / 3.0), color, params.bcs.z);

    if ((GodotFlags() & FLAG_USE_COLOR_CORRECTION) != 0u) {
      color = ApplyColorCorrection(saturate(color));
    } else if ((GodotFlags() & FLAG_CONVERT_TO_SRGB) == 0u) {
      color = renodx::color::srgb::Decode(color);
    }
  } else if ((GodotFlags() & FLAG_CONVERT_TO_SRGB) != 0u) {
    color = renodx::color::srgb::Encode(color);
  }
  return color;
#endif
}

float4 GodotTonemap(float2 uv_interp, float4 frag_coord) {
  float4 color = GODOT_SAMPLE_LEVEL(source_color, uv_interp, 0.0f);
  color.rgb *= params.luminance_multiplier;

  float exposure = params.exposure;
  if ((GodotFlags() & FLAG_USE_AUTO_EXPOSURE) != 0u) {
    exposure *= 1.0 / (GODOT_LOAD(source_auto_exposure, int3(0, 0, 0)).r * params.luminance_multiplier / params.auto_exposure_scale);
  }
  color.rgb *= exposure;

  if ((GodotFlags() & FLAG_USE_FXAA) != 0u) {
    color.rgb = DoFxaa(color.rgb, exposure, uv_interp);
  }

  // Pre-tonemap glow: every mode but soft light from 4.6, only mix before.
#if GODOT_VERSION >= 406
  if ((GodotFlags() & FLAG_USE_GLOW) != 0u && params.glow_mode != GLOW_MODE_SOFTLIGHT) {
#else
  if ((GodotFlags() & FLAG_USE_GLOW) != 0u && params.glow_mode == GLOW_MODE_MIX) {
#endif
    float3 glow = SampleGlow(uv_interp);
    if (params.glow_mode == GLOW_MODE_MIX) {
      color.rgb = color.rgb * (1.0 - params.glow_intensity) + glow;
    } else {
      color.rgb = ApplyGlow(color.rgb, glow, params.white);
    }
  }

  const bool output_srgb = (GodotFlags() & FLAG_CONVERT_TO_SRGB) != 0u;
  const TonemapCurve vanilla_curve = VanillaCurve();

  // output_max_value > 1: Godot 4.7's own HDR output is on, and the addon leaves that swap chain alone.
  if (RENODX_TONE_MAP_TYPE == 0.f || GODOT_OUTPUT_MAX_VALUE > 1.f) {
    float3 vanilla = ApplyPostTonemap(ApplyTonemapping(color.rgb, vanilla_curve), uv_interp);
    // screen_space_dither(gl_FragCoord.xy, 255.0 or 1023.0)
    float3 dither = frac(dot(float2(171.0, 231.0), frag_coord.xy) / float3(103.0, 71.0, 97.0)) - 0.5;
    if ((GodotFlags() & FLAG_USE_8_BIT_DEBANDING) != 0u) {
      vanilla += dither / 255.0;
    }
#if GODOT_VERSION == 405
    else if ((GodotFlags() & FLAG_USE_10_BIT_DEBANDING) != 0u) {
      vanilla += dither / 1023.0;
    }
#endif
    // The SDR viewport target was RGBA8 UNORM before the upgrade; the HDR 2D one was already float.
    color.rgb = output_srgb ? saturate(vanilla) : vanilla;
    return color;
  }

  float3 untonemapped = color.rgb;

  // Linear, and Reinhard with white == output_max_value (identity), only clip at the UNORM write: the operator output
  // is no SDR reference there, so the vanilla chain runs on RenoDRT's neutral SDR instead (graded == neutral without
  // adjustments, i.e. a plain RenoDRT tone map of the scene). Godot 4.0-4.3's Reinhard is a real curve below white (2c/(1+c)
  // at white 1), so like Filmic/ACES it stays the reference even though it also reaches 1 at white.
  const bool is_hard_clip = params.tonemapper == TONEMAPPER_LINEAR
                            || (GODOT_VERSION >= 404 && params.tonemapper == TONEMAPPER_REINHARD
                                && vanilla_curve.reinhard_white_squared == vanilla_curve.output_max_value);
  float3 sdr_reference = is_hard_clip ? renodx::tonemap::renodrt::NeutralSDR(untonemapped)
                                      : ApplyTonemapping(untonemapped, vanilla_curve);
  float3 graded_sdr = saturate(ApplyPostTonemap(sdr_reference, uv_interp));
  if (output_srgb) {
    graded_sdr = renodx::color::srgb::Decode(graded_sdr);
  }

  // Godot HDR uses Godot 4.7's own HDR curve where Godot has one (Reinhard with white above 1, AgX; 4.6+), and
  // RenoDRT otherwise (Linear, Filmic, ACES, Reinhard white 1, everything before 4.6).
  bool use_godot_hdr_curve = false;
#if GODOT_VERSION >= 406
  use_godot_hdr_curve = RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_GODOT_HDR && !is_hard_clip
                        && (params.tonemapper == TONEMAPPER_REINHARD || params.tonemapper == TONEMAPPER_AGX);
#endif

  float3 tonemapped;
  if (use_godot_hdr_curve) {
#if GODOT_VERSION >= 406
    // output_max_value = Peak / Game Brightness, pre-compensated for gamma correction like RenoDRT's peak and at
    // least 1 like Godot's (tone_mapper.cpp); the curve parameters are recomputed the way Godot's CPU does
    // (environment_storage.cpp), and the vanilla grade is carried over from the SDR curve.
    float peak_ratio = RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
    if (RENODX_GAMMA_CORRECTION != 0.f) {
      peak_ratio = renodx::color::correct::Gamma(peak_ratio, true, RENODX_GAMMA_CORRECTION == 1.f ? 2.2f : 2.4f);
    }
    TonemapCurve hdr_curve;
    hdr_curve.output_max_value = max(1.f, peak_ratio);
    float reinhard_white = max(hdr_curve.output_max_value,
                               sqrt(vanilla_curve.reinhard_white_squared * vanilla_curve.output_max_value));
    hdr_curve.reinhard_white_squared = reinhard_white * reinhard_white / hdr_curve.output_max_value;
    // AgX high clip from the pushed SDR curve (awp_w = (high_clip - 0.18)^2 / (output_max - 0.18) * slope), so it
    // follows the curve in use whatever `white` Godot's CPU started from; scaled with output_max_value like 4.7.
    float awp_slope = params.tonemapper_params.z;
    float sdr_high_clip = 0.18f + sqrt(vanilla_curve.agx_awp_w * (vanilla_curve.output_max_value - 0.18f) / awp_slope);
    float agx_high_clip = sdr_high_clip / vanilla_curve.output_max_value * hdr_curve.output_max_value;
    hdr_curve.agx_awp_w = (agx_high_clip - 0.18f) * (agx_high_clip - 0.18f) / (hdr_curve.output_max_value - 0.18f) * awp_slope;
    tonemapped = renodx::tonemap::UpgradeToneMap(
        ApplyTonemapping(untonemapped, hdr_curve),
        sdr_reference,
        graded_sdr,
        RENODX_COLOR_GRADE_STRENGTH);
#endif
  } else {
    // The operator (Filmic/ACES/AgX/Reinhard) is part of the vanilla look, so it stays in graded_sdr and RenoDRT's
    // neutral SDR is the reference: midtones keep Godot's curve, highlights extend past it. Godot's 8-bit debanding
    // dither is left out: the target is float now.
    renodx::draw::Config config = renodx::draw::BuildConfig();
    if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_GODOT_HDR) {
      config.tone_map_type = renodx::tonemap::config::type::RENODRT;
    }
    tonemapped = renodx::draw::ToneMapPass(untonemapped, graded_sdr, config);
  }

  // The target keeps Godot's encoding so the 2D/UI canvas blends over it as in vanilla: sRGB-shaped on an SDR
  // viewport, linear on an HDR 2D one (the replaced final blit then encodes it without Godot's clamp).
  tonemapped = renodx::draw::RenderIntermediatePass(tonemapped);
  if (!output_srgb) {
    tonemapped = renodx::color::srgb::DecodeSafe(tonemapped);
  }
  color.rgb = tonemapped;
  return color;
}
