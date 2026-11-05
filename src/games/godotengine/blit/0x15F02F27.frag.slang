// Godot final viewport blit (Vulkan; seen in 4.7.2).
#define GODOT_VERSION 407
#include "./blit.hlsl"

float4 main([[vk::location(0)]] float2 uv: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotBlit(uv, frag_coord);
}
