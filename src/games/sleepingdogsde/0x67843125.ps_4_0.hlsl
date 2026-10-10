// Scene tone mapper: decodes the scene buffer, applies the bloom alpha factor, the Hejl-Dawson filmic curve, the white
// scale (Value0.y), a screen blend with the bloom and the in-game brightness gamma (Value0.w). Output is gamma-encoded SDR.
#include "./common.hlsl"

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

void main(
    float4 v0: SV_Position0,
    float2 v1: TEXCOORD0,
    out float4 o0: SV_Target0) {
  float3 scene = DecodeScene(texDiffuse.Sample(_texDiffuse_s, v1.xy).rgb);
  float4 bloom = texHDRBloom.Sample(_texHDRBloom_s, v1.xy);
  float white_scale = cbShaderParams.Value0.y;
  float brightness_gamma = max(cbShaderParams.Value0.w, 0.01f);

  // Vanilla: the bloom alpha scales the scene by lerp(1, scene, bloom.a), then the Hejl-Dawson filmic curve on (x - 0.004).
  float3 untonemapped = scene * lerp(1.f, scene, bloom.a);
  float3 curve_input = max(0.f, untonemapped - 0.004f);
  float3 tonemapped_gamma = (curve_input * (curve_input * 6.2f + 0.5f)) / (curve_input * (curve_input * 6.2f + 1.7f) + 0.06f);

  if (RENODX_TONE_MAP_TYPE != 0.f) {
    // Original RenoDX Sleeping Dogs structure (ShortFuse): the HDR tone mapper replaces only the filmic curve and the
    // vanilla post-steps below still run on top. The bloom screen blend in gamma space then holds back the range above
    // white where the bloom is strong, which keeps lamps and fireworks soft (expanded without it, their additive sprites
    // turn into thin, white, blocky outlines). The vanilla curve is the graded reference, so shadows (its toe), contrast
    // and hue stay vanilla; mid grey 0.18 is scaled to the curve's linear output for 0.18 (0.2254).
    renodx::draw::Config draw_config = renodx::draw::BuildConfig();
    // White scale and brightness run after the tone mapper and turn its maximum M into (M / white_scale^2.2)^brightness:
    // pre-compensate the peak so the final output tops out at Peak Brightness.
    draw_config.peak_white_nits = pow(RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS * pow(white_scale, 2.2f * brightness_gamma), 1.f / brightness_gamma)
                                  * RENODX_DIFFUSE_WHITE_NITS;
    // The 2.2 encode and decode around ToneMapPass are explicit here, so RenoDRT must not raise its peak for a later
    // gamma correction (it overshot Peak Brightness by ~10%).
    draw_config.gamma_correction = 0.f;
    tonemapped_gamma = renodx::color::gamma::EncodeSafe(
        renodx::draw::ToneMapPass(
            untonemapped * (0.2254f / 0.18f),
            renodx::color::gamma::DecodeSafe(tonemapped_gamma, 2.2f),
            draw_config),
        2.2f);
  }

  // Vanilla post-steps in gamma space: white scale, bloom screen blend, in-game brightness.
  float3 output_gamma = tonemapped_gamma / white_scale;
  output_gamma = 1.f - (1.f - bloom.rgb) * (1.f - output_gamma);
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    output_gamma = saturate(output_gamma);  // vanilla 8-bit output clamp
  }
  output_gamma = renodx::color::gamma::EncodeSafe(output_gamma, 1.f / brightness_gamma);

  o0.rgb = renodx::draw::RenderIntermediatePass(renodx::color::srgb::DecodeSafe(output_gamma));
  o0.w = 1.f;
}
