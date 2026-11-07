// Godot final viewport blit (Vulkan; seen in 4.2.2 and 4.3).
#define GODOT_VERSION 403
#include "./blit.hlsl"

float4 main([[vk::location(0)]] float2 uv: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotBlit(uv, frag_coord);
}
