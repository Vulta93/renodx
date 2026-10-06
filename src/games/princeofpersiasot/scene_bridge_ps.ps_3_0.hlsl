#include "./common.hlsli"

// Bridge in, drawn by the addon right before the game copies the frame into its soft-glow chain. The frame at this
// point (HDR scene + HUD + fades, intermediate encoding) has been saved by the addon; the backbuffer gets its SDR
// version so the glow chain and its composite run exactly as in vanilla. scene_upgrade_ps restores the HDR range right
// after the glow composite.
sampler2D frame_texture : register(s0);

float4 main(float2 vpos : VPOS) : COLOR {
  const float4 frame = tex2Dlod(frame_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  const float3 hdr_linear = renodx::draw::DecodeColor(frame.rgb, RENODX_INTERMEDIATE_ENCODING);
  return float4(renodx::draw::EncodeColor(BridgeSDR(hdr_linear), RENODX_INTERMEDIATE_ENCODING), frame.a);
}
