#include "./shared.h"
#include "./hejldawson_extended.hlsli"

cbuffer cbShaderParams : register(b0) {
  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);
}

SamplerState _texDiffuse_s : register(s0);
SamplerState _texHDRBloom_s : register(s1);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texHDRBloom : register(t1);

// The scene buffer holds y = 1.04x / (x + 0.2); this returns x (linear).
float3 DecodeScene(float3 encoded) {
  return 0.2f * encoded / (1.04f - encoded);
}

void main(
    float4 v0 : SV_Position0,
    float2 v1 : TEXCOORD0,
    out float4 o0 : SV_Target0) {
  float3 diffuse_linear = DecodeScene(texDiffuse.Sample(_texDiffuse_s, v1.xy).xyz);
  float4 bloom = texHDRBloom.Sample(_texHDRBloom_s, v1.xy);

  // Hejl-Dawson on (x - 0.004); the bloom alpha scales the scene term before the curve.
  float3 hejl_input = max(0.f, diffuse_linear * (bloom.w * (diffuse_linear - 1.f) + 1.f) - 0.004f);
  float3 hejl_gamma = (hejl_input * (6.2f * hejl_input + 0.5f)) / (hejl_input * (6.2f * hejl_input + 1.7f) + 0.06f);

  // Vanilla post-steps, kept in one place for the extended modes: white scale (cb0[0].y) and
  // brightness (cb0[0].w) are gamma space operations, bloom is a screen blend in gamma space.
  float white_scale = cbShaderParams.Value0.y;
  float brightness_gamma = cbShaderParams.Value0.w;
  float3 vanilla_gamma = hejl_gamma / white_scale;
  vanilla_gamma = 1.f - (1.f - bloom.xyz) * (1.f - vanilla_gamma);
  float3 vanilla_sdr = pow(saturate(vanilla_gamma), 2.2f * brightness_gamma);  // linear, clipped at white like the 8-bit output

  // ---------------------------------------------------------------------------
  // Hejl-Dawson Extended (3): vanilla colour, extended luminance.
  // - The vanilla SDR image (vanilla_sdr) supplies hue and saturation. The game's curve clips
  //   each channel at white, so hot lights go toward white instead of keeping their raw
  //   channel ratios and turning orange/red (the raw scene signal itself is red-tinted).
  // - Luminance follows the vanilla image and rises towards souperman9's extended curve (the
  //   game's curve extended linearly above mid grey, same post-steps in linear light) where the
  //   scene exceeds white or the vanilla image is already clipped to white. That carries the
  //   clipped highlights up to Peak (Neutwo on the max channel).
  // - The extension is built from a soft blur of the decoded scene. Lights such as street lamps
  //   are hard-edged polygons in the scene buffer; the game hides those edges under its clipped
  //   plateau and bloom, but keeping the range above white would expose them as sharp boxes.
  // - The scene buffer cannot exceed x = 5 (y = 1.04x / (x + 0.2) in 8 bits), so the extension
  //   alone never exceeds source_max below; Neutwo maps that to Peak.
  // ---------------------------------------------------------------------------
  if (injectedData.toneMapType == 3.f) {
    float2 scene_size;
    texDiffuse.GetDimensions(scene_size.x, scene_size.y);
    // Gaussian (sigma 8 px, truncated) over 10x10 bilinear taps, 3 px apart and centred on the pixel
    // (offsets +-1.5, 4.5 ... 13.5 px). Each tap is a 2x2 box, so the hard edges of a light do not
    // turn into steps. The blur is wide on purpose: the extension then falls off smoothly around a
    // light, like a glow, instead of copying the clipped shapes of the scene buffer.
    static const float blur_weights[5] = {0.169420f, 0.144126f, 0.103058f, 0.059536f, 0.023860f};
    float3 blurred_linear = 0.f;
    [unroll]
    for (int ty = -5; ty < 5; ty++) {
      [unroll]
      for (int tx = -5; tx < 5; tx++) {
        float3 tap = texDiffuse.SampleLevel(_texDiffuse_s, v1.xy + 1.5f * float2(2 * tx + 1, 2 * ty + 1) / scene_size, 0).xyz;
        blurred_linear += blur_weights[abs(2 * tx + 1) / 2] * blur_weights[abs(2 * ty + 1) / 2] * DecodeScene(tap);
      }
    }

    float3 curve_input = max(0.f, blurred_linear - 0.004f);
    float3 extended_gamma = renodx::color::gamma::EncodeSafe(HejlDawson::ApplyExtended(curve_input, 2.2f), 2.2f) / white_scale;
    extended_gamma += bloom.xyz * (1.f - saturate(extended_gamma));  // bloom screen blend, no bloom above white
    float3 extended_sdr = pow(max(0.f, extended_gamma), 2.2f * brightness_gamma);

    float vanilla_y = renodx::color::y::from::BT709(vanilla_sdr);
    // Applies where the scene exceeds white, and also where the vanilla image is already at white
    // (bloom plateau around a light): there the extension ramps up smoothly towards the light
    // instead of ending in a flat white shelf against it.
    float extension_weight = max(
        smoothstep(1.f, 2.f, renodx::color::y::from::BT709(blurred_linear)),
        smoothstep(0.85f, 1.f, vanilla_y));
    float target_y = lerp(vanilla_y, max(vanilla_y, renodx::color::y::from::BT709(extended_sdr)), extension_weight);
    float3 color = vanilla_sdr * renodx::math::DivideSafe(target_y, vanilla_y, 1.f);

    float peak = injectedData.toneMapPeakNits / injectedData.toneMapGameNits;
    // Brightest value the game can deliver (x = 5) through the same chain.
    float source_max = pow(
        renodx::color::gamma::Encode(HejlDawson::ApplyExtended(4.996f, 2.2f), 2.2f) / white_scale,
        2.2f * brightness_gamma);

    color = renodx::color::grade::UserColorGrading(
        color,
        injectedData.colorGradeExposure,
        injectedData.colorGradeHighlights,
        injectedData.colorGradeShadows,
        injectedData.colorGradeContrast,
        injectedData.colorGradeSaturation);
    color = renodx::tonemap::neutwo::MaxChannel(color, peak, max(source_max, peak));

    o0.rgb = renodx::draw::RenderIntermediatePass(color);
    o0.w = 1;
    return;
  }

  // Vanilla / None / ACES.
  float3 tonemapped_gamma = hejl_gamma;
  if (injectedData.toneMapType != 0.f) {
    // Vanilla Hejl-Dawson output for 0.18 input, linearised: HBD(0.18 - 0.004)^2.2.
    const float midgray = 0.2254f;

    renodx::tonemap::Config config = renodx::tonemap::config::Create();
    config.type = injectedData.toneMapType;
    config.peak_nits = injectedData.toneMapPeakNits;
    config.game_nits = injectedData.toneMapGameNits;

    // The vanilla post-steps below still run after Apply(): divide by cb0[0].y (white-point
    // scale, ~0.87) and pow(cb0[0].w) (in-game Brightness), both in gamma 2.2 space.
    // For the brightest pixels they turn Apply()'s maximum M into M^w / y^(2.2*w),
    // which pushed highlights ~35% past Peak Brightness. Pre-compensate the peak handed
    // to the tonemapper so the FINAL output tops out exactly at Peak Brightness.
    if (white_scale > 0.f && brightness_gamma > 0.f) {
      float target_max = injectedData.toneMapPeakNits / injectedData.toneMapGameNits;
      float compensated_max = pow(target_max * pow(white_scale, 2.2f * brightness_gamma), 1.f / brightness_gamma);
      config.peak_nits = compensated_max * injectedData.toneMapGameNits;
    }
    config.exposure = injectedData.colorGradeExposure;
    config.highlights = injectedData.colorGradeHighlights;
    config.shadows = injectedData.colorGradeShadows;
    config.contrast = injectedData.colorGradeContrast;
    config.saturation = injectedData.colorGradeSaturation;
    config.mid_gray_value = midgray;
    config.mid_gray_nits = midgray * 100.f;
    if (injectedData.toneMapHueCorrection != 0.f) {
      config.hue_correction_type = renodx::tonemap::config::hue_correction_type::CUSTOM;
      config.hue_correction_color = hejl_gamma;
      config.hue_correction_strength = injectedData.toneMapHueCorrection;
    }

    tonemapped_gamma = pow(abs(renodx::tonemap::config::Apply(diffuse_linear, config)), 1.f / 2.2f);
  }

  // Vanilla post-steps: white scale, bloom screen blend and user brightness, all in gamma space.
  float3 output_gamma = tonemapped_gamma / white_scale;
  output_gamma = 1.f - (1.f - bloom.xyz) * (1.f - output_gamma);
  output_gamma = pow(output_gamma, brightness_gamma);

  // o0 is gamma 2.2 here. Linearise and hand it to the standard RenoDX intermediate pass
  // (Game -> UI nits scaling + gamma 2.2 encode).
  o0.rgb = renodx::draw::RenderIntermediatePass(renodx::color::gamma::DecodeSafe(output_gamma));
  o0.w = 1;
}
