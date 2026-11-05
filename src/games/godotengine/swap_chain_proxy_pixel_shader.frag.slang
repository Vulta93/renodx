// RenoDX swap chain proxy pass (Vulkan): decodes the intermediate and writes the HDR swap chain.
#include "./shared.h"

[[vk::binding(0, 0)]]
Texture2D t0 : register(t0);
[[vk::binding(1, 0)]]
SamplerState s0 : register(s0);

float4 main(float4 vpos: SV_POSITION, [[vk::location(0)]] float2 uv: TEXCOORD0) : SV_TARGET {
  return renodx::draw::SwapChainPass(t0.Sample(s0, uv));
}
