#include "./common.hlsli"

// Scene pass 1, drawn by the addon in place of the game's offset blit (scene texture T -> backbuffer). Writes the SDR
// scene (sRGB-encoded, <= 1.0) with the game's per-frame colour offset applied, exactly like the original blit
// (rgb = saturate(sdr + (2 * c0 - 1))), so the game's glow, shadows and colour effects that follow run as designed.
// The HDR scene is kept aside by the addon and restored in scene_finish_ps.
sampler2D scene_texture : register(s0);

float4 main(float2 vpos : VPOS) : COLOR {
  const float alpha = tex2Dlod(scene_texture, float4(ScenePassUV(vpos), 0.f, 0.f)).a;
  const float3 neutral_sdr_encoded =
      renodx::color::srgb::EncodeSafe(SceneNeutralSDR(SceneUntonemapped(scene_texture, vpos)));
  return float4(saturate(neutral_sdr_encoded + (2.f * game_blit_c0.rgb - 1.f)), saturate(alpha));
}
