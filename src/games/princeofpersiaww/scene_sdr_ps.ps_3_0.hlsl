#include "./common.hlsli"

// Scene pass 1, drawn by the addon in place of the game's first scene blit of the frame (scene texture ->
// backbuffer): the offset blit when it runs, else the grade blit. Writes the SDR scene (sRGB-encoded, <= 1.0) with
// that blit applied exactly like the original, including the offset blit's rgb = saturate(t0 + (2 * c0 - 1)), so the
// game's glow, blur and grade passes that follow run as designed. The HDR scene is kept aside by the addon and
// restored in scene_finish_ps.
sampler2D scene_texture : register(s0);

float4 main(float2 vpos : VPOS) : COLOR {
  const float4 scene = tex2Dlod(scene_texture, float4(ScenePassUV(vpos), 0.f, 0.f));
  const float blit_alpha = scene.a * game_blit_c0.a + (scene.a - game_blit_c0.a);
  const float3 neutral_sdr = SceneNeutralSDR(SceneUntonemapped(scene_texture, vpos));
  return float4(saturate(renodx::color::srgb::EncodeSafe(neutral_sdr) + (2.f * game_offset_c0.rgb - 1.f)),
                saturate(blit_alpha));
}
