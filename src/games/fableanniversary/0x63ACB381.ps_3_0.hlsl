#include "./shared.h"

// Unreal Engine 3 post-process combine pass (Fable Anniversary).
// Vanilla: DOF blend -> bloom -> gamma 2.2 encode -> saturate (hard clip) -> 16x16x16 color grading LUT (256x16 strip)
// Output: graded gamma-encoded RGB, luma in alpha (read by the FXAA pass that follows).

float4 BloomTintAndScreenBlendThreshold : register(c0);
float4 HalfResMaskRect : register(c8);
sampler SceneColorTexture : register(s0);
sampler FilterColor1Texture : register(s1);
sampler ColorGradingLUT : register(s2);
sampler LowResPostProcessBuffer : register(s3);

struct PS_IN {
  float4 texcoord : TEXCOORD0;
  float4 texcoord1 : TEXCOORD1;
};

float4 main(PS_IN i) : COLOR {
  float4 scene = tex2Dlod(SceneColorTexture, float4(i.texcoord1.xy, 0.f, 0.f));

  // Half resolution DOF buffer stores color / 4, alpha is the weight of the sharp scene
  float2 low_res_uv = min(HalfResMaskRect.zw, max(i.texcoord1.zw, HalfResMaskRect.xy));
  float4 low_res = tex2D(LowResPostProcessBuffer, low_res_uv);
  float3 color = min(lerp(low_res.rgb * 4.f, scene.rgb, low_res.a), 65503.f);

  // Bloom is stored / 4; its weight falls off with scene luma
  float screen_blend = saturate(exp2(dot(color, float3(0.3f, 0.59f, 0.11f)) * -3.f) * BloomTintAndScreenBlendThreshold.w);
  float3 bloom = tex2D(FilterColor1Texture, i.texcoord.zw).rgb * BloomTintAndScreenBlendThreshold.rgb * 4.f;
  float3 untonemapped = bloom * screen_blend + color;

  // Vanilla LUT input: pow(x, 1 / 2.2) + saturate. HDR: RenoDRT neutral SDR instead of the hard clip.
  float3 neutral_sdr = saturate(untonemapped);
  [branch]
  if (RENODX_TONE_MAP_TYPE != 0.f) {
    neutral_sdr = renodx::tonemap::renodrt::NeutralSDR(untonemapped);
  }

  // Vanilla LUT addressing: blue selects a 16 px wide slice, two bilinear taps blended by the blue fraction.
  float3 lut_input = saturate(renodx::color::gamma::EncodeSafe(neutral_sdr, 2.2f));
  float lut_slice = floor(lut_input.b * 14.9998999f);
  float2 lut_uv = float2(
      lut_slice * 0.0625f + lut_input.r * 0.05859375f + 0.001953125f,
      lut_input.g * 0.9375f + 0.03125f);
  float3 lut_output = lerp(
      tex2D(ColorGradingLUT, lut_uv).rgb,
      tex2D(ColorGradingLUT, lut_uv + float2(0.0625f, 0.f)).rgb,
      lut_input.b * 15.f - lut_slice);
  // Vanilla encodes with pow(1 / 2.2): decode as sRGB so the Gamma Correction setting (2.2) restores the vanilla look once,
  // in RenderIntermediatePass. ToneMapPass inputs move to the same sRGB-decoded domain.
  float3 graded_sdr = renodx::color::srgb::DecodeSafe(lut_output);

  float3 output_linear = graded_sdr;
  [branch]
  if (RENODX_TONE_MAP_TYPE != 0.f) {
    output_linear = renodx::draw::ToneMapPass(
        renodx::color::correct::GammaSafe(untonemapped, true),
        graded_sdr,
        renodx::color::srgb::DecodeSafe(lut_input));
  }

  float4 o;
  o.rgb = renodx::draw::RenderIntermediatePass(output_linear);
  o.a = dot(o.rgb, float3(0.299f, 0.587f, 0.114f));  // vanilla luma weights (c6), now in the encoded intermediate
  return o;
}
