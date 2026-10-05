#include "./shared.h"

// Scene finish pass, drawn by the addon right before the first HUD/overlay draw of the frame.
// Input: a copy of the backbuffer clone holding the finished 3D scene (SDR-encoded values, can exceed 1.0).
// Output: the same scene written back in the intermediate encoding at Game Brightness relative to UI Brightness,
// so the HUD drawn afterwards stays at UI Brightness (SwapChainPass scales everything by UI Brightness).
sampler2D scene_texture : register(s0);
float4 scene_finish_params : register(c49);  // xy = 1 / render target size

float4 main(float2 vpos : VPOS) : COLOR {
  const float4 color = tex2Dlod(scene_texture, float4((vpos + 0.5f) * scene_finish_params.xy, 0.f, 0.f));
  const float3 linear_color = renodx::color::srgb::DecodeSafe(color.rgb);
  return float4(renodx::draw::RenderIntermediatePass(linear_color), color.a);
}
