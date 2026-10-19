#include "./shared.h"

// Binary Domain scene finish (ps_3_0 0x19BC57FB, a plain texture copy). The addon runs this replacement only for the
// full resolution copy that hands the finished scene to the HUD buffer; the luminance downsample chain, which uses the
// same shader, keeps the original (see addon.cpp). At this point the scene is complete: tone map, fog sprites and AA are
// done and the HUD has not been drawn, so this is where the user-facing look is applied to the whole scene at once:
// color grading, the Game / UI brightness split (HUD drawn afterwards stays at UI Brightness) and the roll-off.
// Input: the scene in the game's 2.2 encoding (vanilla encoded value in the SDR range, HDR above it).

static const float TONE_MAP_TYPE_ROLLOFF = 4.f;  // addon-only values (renodx::draw uses 0-3): Roll-off 4, PsychoV 5

sampler2D scene_texture : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  const float4 scene = tex2D(scene_texture, uv);
  float3 tonemapped = renodx::color::gamma::DecodeSafe(scene.rgb, 2.2f);

  [branch]
  if (RENODX_TONE_MAP_TYPE != renodx::draw::TONE_MAP_TYPE_VANILLA) {
    // Color Grading sliders, with the same slider mapping as renodx::draw::ToneMapPass (flare curve, Blowout ->
    // dechroma, Highlight Saturation -> blowout).
    const renodx::color::grade::Config grade_config = renodx::color::grade::config::Create(
        RENODX_TONE_MAP_EXPOSURE,
        RENODX_TONE_MAP_HIGHLIGHTS,
        RENODX_TONE_MAP_SHADOWS,
        RENODX_TONE_MAP_CONTRAST,
        0.10f * pow(RENODX_TONE_MAP_FLARE, 10.f),
        RENODX_TONE_MAP_SATURATION,
        RENODX_TONE_MAP_BLOWOUT,
        0.f,
        tonemapped,
        renodx::color::grade::config::hue_correction_type::INPUT,
        -1.f * (RENODX_TONE_MAP_HIGHLIGHT_SATURATION - 1.f));
    tonemapped = renodx::color::grade::config::ApplyUserColorGrading(tonemapped, grade_config);
  } else {
    // The original 8-bit buffers clamped to white after every blend; here the finished scene is clamped once, after the
    // haze sprites and the AA pass.
    tonemapped = saturate(tonemapped);
  }
  float3 output = renodx::draw::RenderIntermediatePass(tonemapped);

  // Roll-off: exponential roll-off (renodx::tonemap::ExponentialRollOff, clip version) on the max channel taken in the
  // swap chain encoding colour space (BT.2020 for HDR10) where SwapChainPass clamps at Peak, colour scaled so hue is
  // kept: identity up to 0.6x Peak, Peak at the white clip. The clip is a first estimate, not yet measured. Applied in the
  // Roll-off and PsychoV tone mappers.
  [branch]
  if (RENODX_TONE_MAP_TYPE >= TONE_MAP_TYPE_ROLLOFF) {
    float3 color = max(0, renodx::draw::DecodeColor(output, RENODX_INTERMEDIATE_ENCODING));
    float peak = RENODX_PEAK_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS;
    float clip = max(16.f * RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS, peak);
    float rolloff_start = 0.6f * peak;
    [branch]
    if (RENODX_SWAP_CHAIN_GAMMA_CORRECTION != 0.f) {
      const float gamma = RENODX_SWAP_CHAIN_GAMMA_CORRECTION == 1.f ? 2.2f : 2.4f;
      peak = renodx::color::correct::Gamma(peak, true, gamma);
      clip = renodx::color::correct::Gamma(clip, true, gamma);
      rolloff_start = renodx::color::correct::Gamma(rolloff_start, true, gamma);
    }
    const float max_channel = renodx::math::Max(renodx::color::convert::ColorSpaces(
        color, renodx::color::convert::COLOR_SPACE_BT709, RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE));
    const float new_max = renodx::tonemap::ExponentialRollOff(max_channel, rolloff_start, peak, clip);
    color *= max_channel != 0 ? (new_max / max_channel) : 1.f;
    output = renodx::draw::EncodeColor(color, RENODX_INTERMEDIATE_ENCODING);
  }

  return float4(output, scene.a);
}
