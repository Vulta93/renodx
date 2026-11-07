// Godot 4.2/4.3 Forward+ tonemap, 1D LUT, bicubic glow upscale (Vulkan; seen in 4.2.2 and 4.3).
#define GODOT_VERSION 403
#define USE_GLOW_FILTER_BICUBIC
#define USE_1D_LUT
#include "./tonemap.hlsl"

float4 main([[vk::location(0)]] float2 uv_interp: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotTonemap(uv_interp, frag_coord);
}
