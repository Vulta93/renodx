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
sampler2D graded_texture : register(s0);
sampler2D hdr_texture : register(s1);

float4 main(float2 vpos : VPOS) : COLOR {
  const float2 uv = ScenePassUV(vpos);
  const float4 graded = tex2Dlod(graded_texture, float4(uv, 0.f, 0.f));
  const float3 hdr_linear = renodx::draw::DecodeColor(tex2Dlod(hdr_texture, float4(uv, 0.f, 0.f)).rgb, RENODX_INTERMEDIATE_ENCODING);
  const float3 graded_linear = renodx::draw::DecodeColor(graded.rgb, RENODX_INTERMEDIATE_ENCODING);

  const float y_hdr = renodx::color::y::from::BT709(hdr_linear);
  const float y_neutral = renodx::color::y::from::BT709(BridgeSDR(hdr_linear));
  const float3 upgraded = renodx::color::correct::Luminance(graded_linear, y_neutral, y_hdr);
  return float4(renodx::draw::EncodeColor(upgraded, RENODX_INTERMEDIATE_ENCODING), graded.a);
}
