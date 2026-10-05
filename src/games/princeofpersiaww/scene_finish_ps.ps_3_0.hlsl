#include "./shared.h"

// Scene finish pass, drawn by the addon in place of the game's scene blit (render-target texture -> backbuffer,
// ps_1_1 0x2059E26C), the last draw before the glow and the HUD.
// Input: the scene texture (sRGB-encoded SDR values in a float16 target) and the blit's constant c0.
// There is no game tone mapper: vanilla simply clipped at the 8-bit targets.
// Output: tone mapped scene in the intermediate encoding at Game Brightness relative to UI Brightness, so the HUD
// drawn afterwards stays at UI Brightness (SwapChainPass scales everything by UI Brightness).
// Pattern: clivebarkersjericho Output_*.ps_3_0 (Vanilla = clip like the 8-bit target, otherwise ToneMapPass).
static const float TONE_MAP_TYPE_NEUTWO = 5.f;  // addon-only value, not a renodx::draw type

sampler2D scene_texture : register(s0);
float4 scene_finish_params : register(c49);  // xy = 1 / render target size
float4 game_blit_c0 : register(c48);         // c0 of the original blit

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
  const float4 scene = tex2Dlod(scene_texture, float4((vpos + 0.5f) * scene_finish_params.xy, 0.f, 0.f));
  // Original blit (ps_1_1 0x2059E26C): r0 = t0 * c0 + (t0 - c0), without the 8-bit clamp.
  const float4 color = scene * game_blit_c0 + (scene - game_blit_c0);
  const float3 untonemapped = renodx::color::srgb::DecodeSafe(color.rgb);

  float3 tonemapped;
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    tonemapped = saturate(untonemapped);
  } else if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_NEUTWO) {
    // Neutwo: near-identity up to about half of peak, then rolls off to peak; hue kept (max-channel scale).
    // The game's highlights are SDR colours pushed past white by additive blending, mostly 1-4x paper white,
    // so this keeps the "None" look and only protects against clipping at peak. Pattern: games/batmanaa/common.hlsli.
    // Peak relative to SDR white, moved into the pre-gamma-correction domain (pattern: games/rotsp/common.hlsl).
    float peak = RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
    [branch]
    if (RENODX_GAMMA_CORRECTION != 0.f) {
      peak = renodx::color::correct::Gamma(peak, RENODX_GAMMA_CORRECTION > 0.f, RENODX_GAMMA_CORRECTION == 1.f ? 2.2f : 2.4f);
    }
    tonemapped = renodx::tonemap::neutwo::MaxChannel(max(0, ApplyUserGrading(untonemapped)), peak);
  } else if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_UNTONEMAPPED) {
    tonemapped = ApplyUserGrading(untonemapped);
  } else {
    // RenoDRT: ToneMapPass applies the same user sliders itself (BuildConfig reads RENODX_TONE_MAP_*).
    tonemapped = renodx::draw::ToneMapPass(untonemapped);
  }

  return float4(renodx::draw::RenderIntermediatePass(tonemapped), color.a);
}
