// Godot 4.7.2 Forward+ tonemap, 3D LUT, bilinear glow upscale (Vulkan; seen in 4.7.2).
#define GODOT_VERSION 407
#include "./tonemap.hlsl"

float4 main([[vk::location(0)]] float2 uv_interp: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotTonemap(uv_interp, frag_coord);
}
