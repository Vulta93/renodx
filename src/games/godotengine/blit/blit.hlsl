// Godot 4 BlitShaderRD (servers/rendering/renderer_rd/shaders/blit.glsl, no USE_LAYER / lens distortion): the final
// copy of a viewport to the swap chain. Ported from Godot's MIT GLSL source (4.2.2 to 4.7.2 stable); each
// per-hash file sets GODOT_VERSION and calls GodotBlit.
//
// For a linear (HDR 2D) viewport Godot encodes to sRGB and dithers here; 4.6 also clamps, 4.7 leaves the clamp to the
// 8-bit swap chain, which RenoDX makes float. The Vanilla tone mapper keeps that clamp (emulated on 4.7); the other
// tone mappers drop the clamp and the 8-bit dither, so the tone mapper's HDR (written linear on those viewports)
// reaches the swap chain proxy in the same sRGB-shaped encoding as on an SDR viewport.
#ifndef GODOT_VERSION
#error "GODOT_VERSION must be defined by the per-hash file"
#endif

// std140 layout of the GLSL `Pos` push-constant block (64 bytes in 4.2-4.3, 80 in 4.4-4.6, 96 in 4.7).
struct Pos {
  float4 src_rect;
  float4 dst_rect;

#if GODOT_VERSION >= 404
  float rotation_sin;
  float rotation_cos;
#endif
#if GODOT_VERSION == 404
  float2 pad_rotation;
#endif
  float2 eye_center;

  float k1;
  float k2;
  float upscale;
  float aspect_ratio;

  uint layer;
  uint source_linear_or_convert_to_srgb;  // `pad1` before 4.2, `convert_to_srgb` to 4.6, `source_is_srgb` (inverted) from 4.7.
#if GODOT_VERSION >= 405
  uint use_debanding;
#endif
#if GODOT_VERSION >= 407
  uint target_color_space;

  float reference_multiplier;
  float output_max_value;
  uint pad1;
  uint pad2;
#elif GODOT_VERSION >= 405
  float pad;
#endif
};

#ifdef __SLANG__
// Vulkan: Godot's own block at offset 0 of the push-constant range, RenoDX's data right after it (shared.h).
#define GODOT_PUSH_PARAMS Pos
#if GODOT_VERSION >= 407
#define PUSH_CONSTANTS_OFFSET 96
#elif GODOT_VERSION >= 404
#define PUSH_CONSTANTS_OFFSET 80
#else
#define PUSH_CONSTANTS_OFFSET 64
#endif
#endif

#include "../shared.h"

#ifdef __SLANG__
#define data push_data.godot_params
[[vk::binding(0, 0)]] Sampler2D src_rt;
#define GODOT_SAMPLE_SRC(uv) src_rt.Sample(uv)
#else
ConstantBuffer<Pos> data : register(b1700000000);

Texture2D<float4> src_rt : register(t0);
SamplerState src_rt_sampler : register(s0);
#define GODOT_SAMPLE_SRC(uv) src_rt.Sample(src_rt_sampler, uv)
#endif

#define COLOR_SPACE_REC709_LINEAR 0u

float4 GodotBlit(float2 uv, float4 frag_coord) {
  float4 color = GODOT_SAMPLE_SRC(uv);

#if GODOT_VERSION >= 407
  if (data.target_color_space == COLOR_SPACE_REC709_LINEAR) {
    // Godot's own HDR output (the addon does not convert those swap chains).
    if (data.source_linear_or_convert_to_srgb != 0u) {
      color.rgb = renodx::color::srgb::Decode(color.rgb);
    }
    color.rgb = clamp(color.rgb, 0.0, data.output_max_value);
    color.rgb *= data.reference_multiplier;
    return color;
  }
  const bool convert_to_srgb = data.source_linear_or_convert_to_srgb == 0u;
#elif GODOT_VERSION >= 402
  const bool convert_to_srgb = data.source_linear_or_convert_to_srgb != 0u;
#else
  const bool convert_to_srgb = false;  // a pad before 4.2 (no HDR 2D viewports); not used: 4.0/4.1 keep their blit
#endif

  if (convert_to_srgb) {
    if (RENODX_TONE_MAP_TYPE == 0.f) {
#if GODOT_VERSION >= 405
      color.rgb = renodx::color::srgb::Encode(color.rgb);
      if (data.use_debanding != 0u) {
        // screen_space_dither(gl_FragCoord.xy)
        float3 dither = frac(dot(float2(171.0, 231.0), frag_coord.xy) / float3(103.0, 71.0, 97.0));
        color.rgb += (dither - 0.5) / 255.0;
      }
      color.rgb = saturate(color.rgb);
#else
      // linear_to_srgb clamps first before 4.5.
      color.rgb = renodx::color::srgb::Encode(saturate(color.rgb));
#endif
    } else {
      color.rgb = renodx::color::srgb::EncodeSafe(color.rgb);
    }
  }
  return color;
}
