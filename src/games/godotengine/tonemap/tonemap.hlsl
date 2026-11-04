// Godot 4.6 desktop Forward+ tonemap pass (servers/rendering/renderer_rd/shaders/effects/tonemap.glsl, 4.6-stable),
// ported to HLSL from Godot's MIT source because the Devkit could not decompile the DXIL. Godot builds four desktop
// variants from it with USE_GLOW_FILTER_BICUBIC and USE_1D_LUT; each per-hash file sets those and calls
// GodotTonemap. The registers are the ones Godot's NIR-to-DXIL path assigns (set * 100000000 + binding * 100000,
// push constants at b1700000000), read from the original shaders' resource bindings.
//
// Godot's tonemap.glsl:
// Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).
// Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.
// SPDX-License-Identifier: MIT

#include "../shared.h"

// std430 layout of the GLSL `Params` push-constant block (112 bytes). The float glow_levels[7] array is split so
// that HLSL packing keeps every member at its std430 byte offset (an HLSL array would start each element on a new
// 16-byte register).
struct Params {
  float3 bcs;
  uint flags;

  float2 pixel_size;
  uint tonemapper;
  uint pad;

  uint2 glow_texture_size;
  float glow_intensity;
  float glow_map_strength;

  uint glow_mode;
  float3 glow_levels_0_2;
  float4 glow_levels_3_6;

  float exposure;
  float white;
  float auto_exposure_scale;
  float luminance_multiplier;

  float4 tonemapper_params;
};

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

#define FLAG_USE_BCS              (1u << 0)
#define FLAG_USE_GLOW             (1u << 1)
#define FLAG_USE_AUTO_EXPOSURE    (1u << 2)
#define FLAG_USE_COLOR_CORRECTION (1u << 3)
#define FLAG_USE_FXAA             (1u << 4)
#define FLAG_USE_8_BIT_DEBANDING  (1u << 5)
#define FLAG_CONVERT_TO_SRGB      (1u << 6)

#define TONEMAPPER_LINEAR   0u
#define TONEMAPPER_REINHARD 1u
#define TONEMAPPER_FILMIC   2u
#define TONEMAPPER_ACES     3u
#define TONEMAPPER_AGX      4u

#define GLOW_MODE_ADD       0u
#define GLOW_MODE_SCREEN    1u
#define GLOW_MODE_SOFTLIGHT 2u
#define GLOW_MODE_REPLACE   3u
#define GLOW_MODE_MIX       4u

float3 TonemapReinhard(float3 color) {
  float white_squared = params.tonemapper_params.x;
  float3 white_squared_color = white_squared * color;
  return (white_squared_color + color * color) / (white_squared_color + white_squared);
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
  return color_tonemapped / params.tonemapper_params.x;
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
  return color_tonemapped / params.tonemapper_params.x;
}

float3 TonemapAgx(float3 color) {
  // GLSL `m * color` with column-major literals: mul(color, m) with the same literal order as rows.
  const float3x3 rec709_to_rec2020_agx_inset_matrix = float3x3(
      0.544814746488245, 0.140416948464053, 0.0888104196149096,
      0.373787398372697, 0.754137554567394, 0.178871756420858,
      0.0813978551390581, 0.105445496968552, 0.732317823964232);
  const float3x3 agx_outset_rec2020_to_rec709_matrix = float3x3(
      1.96488741169489, -0.299313364904742, -0.164352742528393,
      -0.855988495690215, 1.32639796461980, -0.238183969428088,
      -0.108898916004672, -0.0270845997150571, 1.40253671195648);

  // SDR: output_max_value 1 and crossover 0.18 are constants in Godot 4.6; the CPU fills the rest.
  renodx::tonemap::allenwp::internal::Parameters awp;
  awp.output_max_value = 1.f;
  awp.awp_crossover_point = 0.18f;
  awp.awp_shoulder_max = awp.output_max_value - awp.awp_crossover_point;
  awp.awp_contrast = params.tonemapper_params.x;
  awp.awp_toe_a = params.tonemapper_params.y;
  awp.awp_slope = params.tonemapper_params.z;
  awp.awp_w = params.tonemapper_params.w;

  color = mul(color, rec709_to_rec2020_agx_inset_matrix);
  color = renodx::tonemap::allenwp::internal::Curve(color, awp);
  color = min(awp.output_max_value, color);
  color = mul(color, agx_outset_rec2020_to_rec709_matrix);
  return color;
}

float3 ApplyTonemapping(float3 color) {
  if (params.tonemapper == TONEMAPPER_LINEAR) {
    return color;
  }

  color = max(0.f, color);

  if (params.tonemapper == TONEMAPPER_REINHARD) {
    return TonemapReinhard(color);
  } else if (params.tonemapper == TONEMAPPER_FILMIC) {
    return TonemapFilmic(color);
  } else if (params.tonemapper == TONEMAPPER_ACES) {
    return TonemapAces(color);
  } else {
    return TonemapAgx(color);
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

  return (G0(fuv.y) * (g0x * source_glow.SampleLevel(source_glow_sampler, p0, lod) + g1x * source_glow.SampleLevel(source_glow_sampler, p1, lod)))
         + (G1(fuv.y) * (g0x * source_glow.SampleLevel(source_glow_sampler, p2, lod) + g1x * source_glow.SampleLevel(source_glow_sampler, p3, lod)));
}
#else
float4 SampleGlowLevel(float2 uv, uint lod) {
  return source_glow.SampleLevel(source_glow_sampler, uv, lod);
}
#endif

// gather_glow() * glow_intensity, then the glow map.
float3 SampleGlow(float2 uv) {
  float glow_levels[7] = {
    params.glow_levels_0_2.x, params.glow_levels_0_2.y, params.glow_levels_0_2.z,
    params.glow_levels_3_6.x, params.glow_levels_3_6.y, params.glow_levels_3_6.z, params.glow_levels_3_6.w,
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
    glow = lerp(glow, glow_map.Sample(glow_map_sampler, uv).rgb * glow, params.glow_map_strength);
  }
  return glow;
}

// Does not handle the mix blend mode.
float3 ApplyGlow(float3 color, float3 glow, float white) {
  if (params.glow_mode == GLOW_MODE_ADD) {
    return color + glow;
  } else if (params.glow_mode == GLOW_MODE_SCREEN) {
    glow = clamp(glow, 0.0, white);
    return color + glow - (color * glow / white);
  } else if (params.glow_mode == GLOW_MODE_SOFTLIGHT) {
    glow = saturate(glow);
    float3 soft_light_base = select(color <= 0.25f, ((16.0f * color - 12.0f) * color + 4.0f) * color, sqrt(color));
    return select(color > 1.0f, color, color + glow * (soft_light_base - color));
  } else {
    return glow;
  }
}

float3 ApplyColorCorrection(float3 color) {
#ifdef USE_1D_LUT
  color.r = source_color_correction.Sample(source_color_correction_sampler, float2(color.r, 0.0f)).r;
  color.g = source_color_correction.Sample(source_color_correction_sampler, float2(color.g, 0.0f)).g;
  color.b = source_color_correction.Sample(source_color_correction_sampler, float2(color.b, 0.0f)).b;
  return color;
#else
  return source_color_correction.SampleLevel(source_color_correction_sampler, color, 0.0).rgb;
#endif
}

// FXAA 3.11 compact (Godot's port of kosua20/Rendu, MIT; NVIDIA FXAA 3.11 by Timothy Lottes, BSD-3).
float FxaaQuality(float q) {
  return (q < 5 ? 1.0 : (q > 5 ? (q < 10 ? 2.0 : (q < 11 ? 4.0 : 8.0)) : 1.5));
}

float Rgb2Luma(float3 rgb) {
  return sqrt(dot(rgb, float3(0.299, 0.587, 0.114)));
}

float FxaaLuma(float2 uv, float exposure) {
  return Rgb2Luma(source_color.SampleLevel(source_color_sampler, uv, 0.0).rgb * exposure * params.luminance_multiplier);
}

float FxaaLumaOffset(float2 uv, float exposure, int2 offset) {
  return Rgb2Luma(source_color.SampleLevel(source_color_sampler, uv, 0.0, offset).rgb * exposure * params.luminance_multiplier);
}

float3 DoFxaa(float3 color, float exposure, float2 uv_interp) {
  const float EDGE_THRESHOLD_MIN = 0.0312;
  const float EDGE_THRESHOLD_MAX = 0.125;
  const int ITERATIONS = 12;
  const float SUBPIXEL_QUALITY = 0.75;

  float luma_up = FxaaLumaOffset(uv_interp, exposure, int2(0, 1));
  float luma_down = FxaaLumaOffset(uv_interp, exposure, int2(0, -1));
  float luma_left = FxaaLumaOffset(uv_interp, exposure, int2(-1, 0));
  float luma_right = FxaaLumaOffset(uv_interp, exposure, int2(1, 0));

  float luma_center = Rgb2Luma(color);

  float luma_min = min(luma_center, min(min(luma_up, luma_down), min(luma_left, luma_right)));
  float luma_max = max(luma_center, max(max(luma_up, luma_down), max(luma_left, luma_right)));

  float luma_range = luma_max - luma_min;

  if (luma_range < max(EDGE_THRESHOLD_MIN, luma_max * EDGE_THRESHOLD_MAX)) {
    return color;
  }

  float luma_down_left = FxaaLumaOffset(uv_interp, exposure, int2(-1, -1));
  float luma_up_right = FxaaLumaOffset(uv_interp, exposure, int2(1, 1));
  float luma_up_left = FxaaLumaOffset(uv_interp, exposure, int2(-1, 1));
  float luma_down_right = FxaaLumaOffset(uv_interp, exposure, int2(1, -1));

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

  return source_color.SampleLevel(source_color_sampler, final_uv, 0.0).rgb * exposure * params.luminance_multiplier;
}

// Everything Godot does after apply_tonemapping: soft-light glow, then (adjustments on) brightness, sRGB encode,
// contrast, saturation and the colour-correction LUT, else the sRGB encode alone. The result is encoded the way
// Godot writes it: sRGB when FLAG_CONVERT_TO_SRGB is set (SDR viewport), linear otherwise (HDR 2D).
float3 ApplyPostTonemap(float3 color, float2 uv) {
  if ((params.flags & FLAG_USE_GLOW) != 0u && params.glow_mode == GLOW_MODE_SOFTLIGHT) {
    color = ApplyGlow(color, ApplyTonemapping(SampleGlow(uv)), params.white);
  }

  if ((params.flags & FLAG_USE_BCS) != 0u) {
    color = color * params.bcs.x;
    color = renodx::color::srgb::Encode(color);
    color = lerp(0.5f, color, params.bcs.y);
    color = lerp(dot(float3(1.0f, 1.0f, 1.0f), color) * (1.0 / 3.0), color, params.bcs.z);

    if ((params.flags & FLAG_USE_COLOR_CORRECTION) != 0u) {
      color = ApplyColorCorrection(saturate(color));
    } else if ((params.flags & FLAG_CONVERT_TO_SRGB) == 0u) {
      color = renodx::color::srgb::Decode(color);
    }
  } else if ((params.flags & FLAG_CONVERT_TO_SRGB) != 0u) {
    color = renodx::color::srgb::Encode(color);
  }
  return color;
}

float4 GodotTonemap(float2 uv_interp, float4 frag_coord) {
  float4 color = source_color.SampleLevel(source_color_sampler, uv_interp, 0.0f);
  color.rgb *= params.luminance_multiplier;

  float exposure = params.exposure;
  if ((params.flags & FLAG_USE_AUTO_EXPOSURE) != 0u) {
    exposure *= 1.0 / (source_auto_exposure.Load(int3(0, 0, 0)).r * params.luminance_multiplier / params.auto_exposure_scale);
  }
  color.rgb *= exposure;

  if ((params.flags & FLAG_USE_FXAA) != 0u) {
    color.rgb = DoFxaa(color.rgb, exposure, uv_interp);
  }

  if ((params.flags & FLAG_USE_GLOW) != 0u && params.glow_mode != GLOW_MODE_SOFTLIGHT) {
    float3 glow = SampleGlow(uv_interp);
    if (params.glow_mode == GLOW_MODE_MIX) {
      color.rgb = color.rgb * (1.0 - params.glow_intensity) + glow;
    } else {
      color.rgb = ApplyGlow(color.rgb, glow, params.white);
    }
  }

  const bool output_srgb = (params.flags & FLAG_CONVERT_TO_SRGB) != 0u;

  // An HDR 2D viewport (no sRGB conversion here) also stays vanilla: Godot's final blit converts it to sRGB and clamps
  // it, so HDR written here would only be clipped.
  if (RENODX_TONE_MAP_TYPE == 0.f || !output_srgb) {
    float3 vanilla = ApplyPostTonemap(ApplyTonemapping(color.rgb), uv_interp);
    if ((params.flags & FLAG_USE_8_BIT_DEBANDING) != 0u) {
      // screen_space_dither(gl_FragCoord.xy, 255.0)
      float3 dither = frac(dot(float2(171.0, 231.0), frag_coord.xy) / float3(103.0, 71.0, 97.0));
      vanilla += (dither - 0.5) / 255.0;
    }
    // The SDR viewport target was RGBA8 UNORM before the upgrade; the HDR 2D one was already float.
    color.rgb = output_srgb ? saturate(vanilla) : vanilla;
    return color;
  }

  float3 untonemapped = color.rgb;

  // Linear, and Reinhard with white 1 (identity), only clip at the UNORM write: the operator output is no SDR
  // reference there, so the vanilla chain runs on RenoDRT's neutral SDR instead (graded == neutral without
  // adjustments, i.e. a plain RenoDRT tone map of the scene).
  const bool is_hard_clip = params.tonemapper == TONEMAPPER_LINEAR
                            || (params.tonemapper == TONEMAPPER_REINHARD && params.tonemapper_params.x == 1.f);
  float3 sdr_reference = is_hard_clip ? renodx::tonemap::renodrt::NeutralSDR(untonemapped) : ApplyTonemapping(untonemapped);
  float3 graded_sdr = renodx::color::srgb::Decode(saturate(ApplyPostTonemap(sdr_reference, uv_interp)));

  // The operator (Filmic/ACES/AgX/Reinhard) is part of the vanilla look, so it stays in graded_sdr and RenoDRT's
  // neutral SDR is the reference: midtones keep Godot's curve, highlights extend past it. Godot's 8-bit debanding
  // dither is left out: the target is float now.
  float3 tonemapped = renodx::draw::ToneMapPass(untonemapped, graded_sdr);
  // The target keeps Godot's sRGB-shaped encoding so the 2D/UI canvas blends over it as in vanilla.
  color.rgb = renodx::draw::RenderIntermediatePass(tonemapped);
  return color;
}
