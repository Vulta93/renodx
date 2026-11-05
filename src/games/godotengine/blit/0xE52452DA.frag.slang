// Godot final viewport blit (Vulkan; seen in 4.4.1).
#define GODOT_VERSION 404
#include "./blit.hlsl"

float4 main([[vk::location(0)]] float2 uv: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotBlit(uv, frag_coord);
}
