#include "./shared.h"

// Display Proxy output (D3D11 side). The game (OpenGL) leaves its frame gamma-encoded with the HDR range above 1.0
// (max-channel bridge in 0xA85A9FE0.frag.glsl); the HUD is drawn on top of it before this pass.
// Tone Mapper values (settings order in addon.cpp): 0 = Vanilla, 1 = None, 2 = Roll-off (here),
// 3 = PsychoV (tone mapped in 0xA85A9FE0.frag.glsl; only SwapChainPass runs here, like None).
static const float TONE_MAP_TYPE_VANILLA = 0.f;
static const float TONE_MAP_TYPE_ROLLOFF = 2.f;

// Brightest steady highlight the game produces, in multiples of Game Brightness.
// PLACEHOLDER (Prince of Persia's value) until measured here with Tone Mapper None and Peak at 4000-10000.
static const float SCENE_WHITE_CLIP = 7.5f;

Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
float4 main(float4 vpos: SV_POSITION, float2 uv: TEXCOORD0)
    : SV_TARGET {
  // Used for OpenGL support
  uv.y = lerp(uv.y, 1.0 - uv.y, shader_injection.custom_flip_uv_y);

  float4 frame = t0.Sample(s0, uv);

  [branch]
  if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_VANILLA) {
    // The game's SDR image, clipped like its original 8-bit buffers.
    frame.rgb = saturate(frame.rgb);
  } else if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_ROLLOFF) {
    // Tone Mapper Roll-off (pattern: games/princeofpersiaww scene_finish_ps): exponential roll-off
    // (renodx::tonemap::ExponentialRollOff, clip version) on the max channel, colour scaled so hue is kept: identity up
    // to 0.6x Peak, so mid-tones and the HUD (UI white well below it) are untouched. Max channel taken in the swap
    // chain encoding colour space (BT.2020 for HDR10), where SwapChainPass clamps at Peak.
    float3 color = max(0, renodx::draw::DecodeColor(frame.rgb, RENODX_INTERMEDIATE_ENCODING));
    float peak = RENODX_PEAK_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS;
    float clip = max(SCENE_WHITE_CLIP * RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS, peak);
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
    frame.rgb = renodx::draw::EncodeColor(color, RENODX_INTERMEDIATE_ENCODING);
  }

  return renodx::draw::SwapChainPass(frame);
}
