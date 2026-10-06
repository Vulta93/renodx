#include "./common.hlsli"

// Scene pass 2, drawn by the addon after the game's glow B / blur effects and before the HUD.
// Inputs: graded_texture = the game's result of its effects on the SDR scene (sRGB-encoded), scene_texture = the
// HDR scene saved before the blit. The HDR scene is rebuilt on top of the game's SDR result with
// renodx::tonemap::UpgradeToneMap(untonemapped, neutral_sdr, graded_sdr) (graded SDR bridge, handle-sdr-tonemap-lut):
// the game's look and effects come from graded_sdr, the range above SDR from untonemapped - neutral_sdr.
// Output: tone mapped scene in the intermediate encoding at Game Brightness relative to UI Brightness, so the HUD
// drawn afterwards stays at UI Brightness (SwapChainPass scales everything by UI Brightness).
static const float TONE_MAP_TYPE_ROLLOFF = 4.f;  // addon-only value (renodx::draw uses 0-3)

sampler2D graded_texture : register(s0);
sampler2D scene_texture : register(s1);

// Color Grading sliders, with the same slider mapping as renodx::draw::ToneMapPass
// (flare curve, Blowout -> dechroma, Highlight Saturation -> blowout). Pattern: games/batmanaa/common.hlsli.
float3 ApplyUserGrading(float3 color) {
  const renodx::color::grade::Config config = renodx::color::grade::config::Create(
      RENODX_TONE_MAP_EXPOSURE,
      RENODX_TONE_MAP_HIGHLIGHTS,
      RENODX_TONE_MAP_SHADOWS,
      RENODX_TONE_MAP_CONTRAST,
      0.10f * pow(RENODX_TONE_MAP_FLARE, 10.f),
      RENODX_TONE_MAP_SATURATION,
      RENODX_TONE_MAP_BLOWOUT,
      0.f,
      color,
      renodx::color::grade::config::hue_correction_type::INPUT,
      -1.f * (RENODX_TONE_MAP_HIGHLIGHT_SATURATION - 1.f));
  return renodx::color::grade::config::ApplyUserColorGrading(color, config);
}

float4 main(float2 vpos : VPOS) : COLOR {
  const float4 graded = tex2Dlod(graded_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  const float3 graded_sdr = renodx::color::srgb::DecodeSafe(graded.rgb);

  float3 tonemapped;
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    // The game's own SDR result, clipped like the original 8-bit target.
    tonemapped = saturate(graded_sdr);
  } else {
    const float3 untonemapped = SceneUntonemapped(scene_texture, vpos);
    tonemapped = ApplyUserGrading(
        renodx::tonemap::UpgradeToneMap(untonemapped, SceneNeutralSDR(untonemapped), graded_sdr, 1.f));
  }
  float3 output = renodx::draw::RenderIntermediatePass(tonemapped);

  // Tone Mapper Roll-off (pattern: games/princeofpersiasot/scene_upgrade_ps): exponential roll-off
  // (renodx::tonemap::ExponentialRollOff, clip version) on the max channel, colour scaled so hue is kept: identity up
  // to 0.6x Peak, Peak at the white clip (or Peak when higher). White clip: 7.5x Game Brightness (~1520 nits at 203),
  // the brightest steady highlight the game produces: fire cores on the ship deck measured with Tone Mapper None, Peak
  // 10000: up to ~1100 nits luminance / ~1350 nits max channel at 203 nits Game Brightness, plus margin. Transient
  // additive stacks (flaming arrows, up to ~8800 nits: sprites piling up in the float16 target, vanilla clipped them at
  // white) are deliberately left out; they are clamped at Peak by SwapChainPass.
  // Applied to the final linear values (after gamma correction, relative to UI Brightness) with the max channel taken
  // in the swap chain encoding colour space (BT.2020 for HDR10), exactly where SwapChainPass clamps at Peak. On the
  // BT.709 max channel an orange flame's red at Peak is only ~0.73x Peak in BT.2020 (measured in the SoT mod: fire
  // capped ~27% below Tone Mapper None).
  [branch]
  if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_ROLLOFF) {
    float3 color = max(0, renodx::draw::DecodeColor(output, RENODX_INTERMEDIATE_ENCODING));
    float peak = RENODX_PEAK_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS;
    float clip = max(7.5f * RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS, peak);
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

  return float4(output, graded.a);
}
