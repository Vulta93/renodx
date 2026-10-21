#include "./shared.h"

// Display Proxy output (D3D11 side). The game (OpenGL) leaves its frame gamma-encoded with the HDR range above 1.0
// (max-channel bridge in 0xA85A9FE0.frag.glsl); the HUD is drawn on top of it before this pass.
// Tone Mapper values (settings order in addon.cpp): 0 = Vanilla (clipped here), 1 = None, 2 = PsychoV (tone mapped in
// 0xA85A9FE0.frag.glsl). None and PsychoV only go through SwapChainPass (max-channel clamp at Peak, HDR10 encode).
static const float TONE_MAP_TYPE_VANILLA = 0.f;

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
  }

  return renodx::draw::SwapChainPass(frame);
}
