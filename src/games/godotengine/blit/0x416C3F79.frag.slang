// Godot final viewport blit (Vulkan; seen in 4.5.1 and 4.6.3).
#define GODOT_VERSION 406
#include "./blit.hlsl"

float4 main([[vk::location(0)]] float2 uv: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotBlit(uv, frag_coord);
}
