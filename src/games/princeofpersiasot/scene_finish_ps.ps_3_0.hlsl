#include "./shared.h"

// Scene finish pass, drawn by the addon right before the first HUD/overlay draw of the frame.
// Input: a copy of the backbuffer clone holding the finished 3D scene (sRGB-encoded SDR values; additive/overbright
// blending in the float16 target can exceed 1.0). There is no game tone mapper: vanilla simply clipped at the
// 8-bit backbuffer.
// Output: tone mapped scene in the intermediate encoding at Game Brightness relative to UI Brightness, so the HUD
// drawn afterwards stays at UI Brightness (SwapChainPass scales everything by UI Brightness).
// Pattern: clivebarkersjericho Output_*.ps_3_0 (Vanilla = clip like the 8-bit target, otherwise ToneMapPass).
static const float TONE_MAP_TYPE_NEUTWO = 5.f;  // addon-only value, not a renodx::draw type

sampler2D scene_texture : register(s0);
float4 scene_finish_params : register(c49);  // xy = 1 / render target size

float4 main(float2 vpos : VPOS) : COLOR {
  const float4 color = tex2Dlod(scene_texture, float4((vpos + 0.5f) * scene_finish_params.xy, 0.f, 0.f));
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
    tonemapped = renodx::tonemap::neutwo::MaxChannel(max(0, untonemapped), peak);
  } else {
    tonemapped = renodx::draw::ToneMapPass(untonemapped);
  }

  return float4(renodx::draw::RenderIntermediatePass(tonemapped), color.a);
}
