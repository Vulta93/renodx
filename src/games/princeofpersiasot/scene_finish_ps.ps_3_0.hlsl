#include "./common.hlsli"

// Scene pass, drawn by the addon right before the first HUD/overlay draw of the frame (the marker draw).
// Input: a copy of the backbuffer clone holding the finished 3D scene (sRGB-encoded SDR values; additive/overbright
// blending in the float16 target can exceed 1.0). There is no game tone mapper: vanilla simply clipped at the
// 8-bit backbuffer.
// Output: graded HDR scene (Vanilla: clipped like the 8-bit target) in the intermediate encoding at Game Brightness
// relative to UI Brightness, so the HUD drawn afterwards stays at UI Brightness (SwapChainPass scales everything by UI
// Brightness). Pattern: clivebarkersjericho Output_*.ps_3_0.
// The Roll-off tone mapper is not applied here but in scene_upgrade_ps, after the game's glow composite.
sampler2D scene_texture : register(s0);

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
  const float4 color = tex2Dlod(scene_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  const float3 untonemapped = renodx::color::srgb::DecodeSafe(color.rgb);

  float3 graded;
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    graded = saturate(untonemapped);
  } else {
    graded = ApplyUserGrading(untonemapped);
  }

  return float4(renodx::draw::RenderIntermediatePass(graded), color.a);
}
