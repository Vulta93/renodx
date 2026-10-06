#include "./common.hlsli"

// Scene pass, drawn by the addon right before the first HUD/overlay draw of the frame (the marker draw).
// Input: a copy of the backbuffer clone holding the finished 3D scene (sRGB-encoded SDR values; additive/overbright
// blending in the float16 target can exceed 1.0). There is no game tone mapper: vanilla simply clipped at the
// 8-bit backbuffer.
// Output: tone mapped scene in the intermediate encoding at Game Brightness relative to UI Brightness, so the HUD
// drawn afterwards stays at UI Brightness (SwapChainPass scales everything by UI Brightness).
// Pattern: clivebarkersjericho Output_*.ps_3_0 (Vanilla = clip like the 8-bit target, otherwise ToneMapPass).
static const float TONE_MAP_TYPE_NEUTWO = 5.f;  // addon-only value, not a renodx::draw type
static const float SCENE_MAX_WHITE = 20.f;      // see the Neutwo branch

sampler2D scene_texture : register(s0);

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
  const float4 color = tex2Dlod(scene_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  const float3 untonemapped = renodx::color::srgb::DecodeSafe(color.rgb);

  float3 tonemapped;
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    tonemapped = saturate(untonemapped);
  } else if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_NEUTWO) {
    // Neutwo with a white clip (pattern: games/batmanaa/common.hlsli): identity-like up to the brightest value
    // the game produces and reaching Peak exactly there, so it only compresses what would exceed the display.
    // SCENE_MAX_WHITE = the brightest steady highlight the game produces, relative to SDR white: the courtyard fire
    // measured with Tone Mapper None, Peak 10000, Game Brightness 203, default grading (after the glow bridge):
    // the flickering fire's frame maximum ranged ~2600-3750 nits max channel (CLL) / ~1550-2300 nits luminance,
    // so 20x (~4060 nits) covers it with a little margin. Rare one-frame spikes (up to ~4600 CLL) are deliberately
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
    tonemapped = renodx::tonemap::neutwo::MaxChannel(max(0, ApplyUserGrading(untonemapped)), peak, clip);
  } else if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_UNTONEMAPPED) {
    tonemapped = ApplyUserGrading(untonemapped);
  } else {
    // RenoDRT: ToneMapPass applies the same user sliders itself (BuildConfig reads RENODX_TONE_MAP_*).
    tonemapped = renodx::draw::ToneMapPass(untonemapped);
  }

  return float4(renodx::draw::RenderIntermediatePass(tonemapped), color.a);
}
