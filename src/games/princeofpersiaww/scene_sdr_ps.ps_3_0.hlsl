#include "./common.hlsli"

// Scene pass 1, drawn by the addon in place of the game's scene blit (scene texture -> backbuffer). Writes the
// SDR scene (sRGB-encoded, <= 1.0) so the game's glow B / blur effects that follow run exactly as designed.
// The HDR scene is kept aside by the addon and restored in scene_finish_ps.
sampler2D scene_texture : register(s0);

float4 main(float2 vpos : VPOS) : COLOR {
  const float4 scene = tex2Dlod(scene_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  const float blit_alpha = scene.a * game_blit_c0.a + (scene.a - game_blit_c0.a);
  const float3 neutral_sdr = SceneNeutralSDR(SceneUntonemapped(scene_texture, vpos));
  return float4(renodx::color::srgb::EncodeSafe(neutral_sdr), saturate(blit_alpha));
}
