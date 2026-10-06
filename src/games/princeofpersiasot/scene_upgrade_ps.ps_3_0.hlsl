#include "./common.hlsli"

// Bridge out, drawn by the addon right after the game's soft-glow composite (before the next draw; fallback at
// present), only in frames where the bridge-in pass ran. Overlays drawn later (menu text, the pause screen's blur
// layers built from the glow textures) then blend over HDR, as they blend over SDR in vanilla.
// Inputs: graded_texture = the frame after the glow composite (intermediate encoding): the SDR frame plus the game's
// additive glow and blended blur; hdr_texture = the HDR frame saved before the glow chain (HDR scene, HUD and fades,
// intermediate encoding).
// The range above SDR is rebuilt proportionally: the game's result is scaled by hdr / neutral_sdr luminance
// (renodx::color::correct::Luminance; hue of the game's result kept), so the composite's mix (~59% frame + glow,
// ~41% blur) carries over to the HDR values. Below white hdr == neutral_sdr and the game's result is unchanged.
// Verified with a test ramp (0 -> 20x SDR white): Tone Mapper None matched the prediction at every position.
// Also applies the Roll-off tone mapper (below).
sampler2D graded_texture : register(s0);
sampler2D hdr_texture : register(s1);

float4 main(float2 vpos : VPOS) : COLOR {
  const float2 uv = ScenePassUV(vpos);
  const float4 graded = tex2Dlod(graded_texture, float4(uv, 0.f, 0.f));
  const float3 hdr_linear = renodx::draw::DecodeColor(tex2Dlod(hdr_texture, float4(uv, 0.f, 0.f)).rgb, RENODX_INTERMEDIATE_ENCODING);
  const float3 graded_linear = renodx::draw::DecodeColor(graded.rgb, RENODX_INTERMEDIATE_ENCODING);

  const float y_hdr = renodx::color::y::from::BT709(hdr_linear);
  const float y_neutral = renodx::color::y::from::BT709(BridgeSDR(hdr_linear));
  float3 upgraded = renodx::color::correct::Luminance(graded_linear, y_neutral, y_hdr);

  // Tone Mapper Roll-off: applied here, on the final frame, instead of in the scene pass. The glow composite blends
  // its blur over small highlights and scales them down (test pattern: 8 px squares to 0.39x, 32 px to 0.47x, 128 px
  // to 0.97x, at any strength), so a curve that reaches Peak in the scene pass leaves small highlights far below Peak.
  // Exponential roll-off (renodx::tonemap::ExponentialRollOff, clip version) on the max channel, colour scaled so hue
  // is kept (same scaling as neutwo::MaxChannel): identity up to 0.6x Peak, Peak at the white clip (or Peak when
  // higher). White clip: 20x Game Brightness (~4060 nits at 203), the brightest steady highlight the game produces:
  // the courtyard fire measured on the final output with Tone Mapper None, Peak 10000: frame maximum ~2600-3750 nits
  // max channel (CLL) over several screenshots, plus a little margin. Rare one-frame spikes (up to ~4600) are left out;
  // they are clamped at Peak by SwapChainPass.
  // The max channel is taken in the swap chain encoding colour space (BT.2020 for HDR10), where SwapChainPass clamps
  // at Peak. Measured in BT.709, an orange flame's red channel at Peak is only ~0.73x Peak in BT.2020, which capped the
  // fire at ~1000 nits with Peak 1360.
  // Units: the intermediate frame is linear, gamma-corrected, relative to UI Brightness (SwapChainPass multiplies by
  // it), so the parameters are converted to that unit, and into the pre-swapchain-gamma domain when that is on.
  // The HUD drawn before the glow is included but sits far below the roll-off start.
  [branch]
  if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_ROLLOFF) {
    float peak = RENODX_PEAK_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS;
    float clip = max(20.f * RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS, peak);
    float rolloff_start = 0.6f * peak;
    [branch]
    if (RENODX_SWAP_CHAIN_GAMMA_CORRECTION != 0.f) {
      const float gamma = RENODX_SWAP_CHAIN_GAMMA_CORRECTION == 1.f ? 2.2f : 2.4f;
      peak = renodx::color::correct::Gamma(peak, true, gamma);
      clip = renodx::color::correct::Gamma(clip, true, gamma);
      rolloff_start = renodx::color::correct::Gamma(rolloff_start, true, gamma);
    }
    upgraded = max(0, upgraded);
    const float max_channel = renodx::math::Max(renodx::color::convert::ColorSpaces(
        upgraded, renodx::color::convert::COLOR_SPACE_BT709, RENODX_SWAP_CHAIN_ENCODING_COLOR_SPACE));
    const float new_max = renodx::tonemap::ExponentialRollOff(max_channel, rolloff_start, peak, clip);
    upgraded *= max_channel != 0 ? (new_max / max_channel) : 1.f;
  }
  return float4(renodx::draw::EncodeColor(upgraded, RENODX_INTERMEDIATE_ENCODING), graded.a);
}
