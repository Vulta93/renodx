#include "./common.hlsli"

// Scene pass 2, drawn by the addon after the game's glow B / blur effects and before the HUD.
// Inputs: graded_texture = the game's result of its effects on the SDR scene (sRGB-encoded), scene_texture = the
// HDR scene saved before the blit. The HDR scene is rebuilt on top of the game's SDR result with
// renodx::tonemap::UpgradeToneMap(untonemapped, neutral_sdr, graded_sdr) (graded SDR bridge, handle-sdr-tonemap-lut):
// the game's look and effects come from graded_sdr, the range above SDR from untonemapped - neutral_sdr.
// Output: tone mapped scene in the intermediate encoding at Game Brightness relative to UI Brightness, so the HUD
// drawn afterwards stays at UI Brightness (SwapChainPass scales everything by UI Brightness).
static const float TONE_MAP_TYPE_NEUTWO = 5.f;  // addon-only value, not a renodx::draw type
static const float SCENE_MAX_WHITE = 7.5f;      // see the Neutwo branch

sampler2D graded_texture : register(s0);
sampler2D scene_texture : register(s1);

// User sliders for the None / Neutwo paths, with the same slider mapping as ToneMapPass
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
    const float3 untonemapped_graded =
        renodx::tonemap::UpgradeToneMap(untonemapped, SceneNeutralSDR(untonemapped), graded_sdr, 1.f);

    [branch]
    if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_NEUTWO) {
      // Neutwo with a white clip (pattern: games/batmanaa/common.hlsli): identity-like up to the brightest value
      // the game produces and reaching Peak exactly there, so it only compresses what would exceed the display.
      // SCENE_MAX_WHITE = the brightest steady highlight the game produces, relative to SDR white: fire cores on
      // the ship deck measured with Tone Mapper None, Peak 10000: up to ~1100 nits luminance / ~1350 nits max
      // channel at 203 nits Game Brightness (~5.5-6.7x), plus margin. Transient additive stacks (flaming arrows,
      // up to ~8800 nits: sprites piling up in the float16 target, vanilla clipped them at white) are deliberately
      // not part of the range; they exceed Peak and are clamped by SwapChainPass instead of compressing the fire.
      // When Peak is at or above SCENE_MAX_WHITE nothing is compressed (clip = peak).
      // Both values are moved into the pre-gamma-correction domain (pattern: games/rotsp/common.hlsl).
      float peak = RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
      float clip = max(SCENE_MAX_WHITE, peak);
      [branch]
      if (RENODX_GAMMA_CORRECTION != 0.f) {
        const float gamma = RENODX_GAMMA_CORRECTION == 1.f ? 2.2f : 2.4f;
        peak = renodx::color::correct::Gamma(peak, true, gamma);
        clip = renodx::color::correct::Gamma(clip, true, gamma);
      }
      tonemapped = renodx::tonemap::neutwo::MaxChannel(max(0, ApplyUserGrading(untonemapped_graded)), peak, clip);
    } else if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_UNTONEMAPPED) {
      tonemapped = ApplyUserGrading(untonemapped_graded);
    } else {
      // RenoDRT: ToneMapPass applies the same user sliders itself (BuildConfig reads RENODX_TONE_MAP_*).
      tonemapped = renodx::draw::ToneMapPass(untonemapped_graded);
    }
  }

  return float4(renodx::draw::RenderIntermediatePass(tonemapped), graded.a);
}
