// Godot 4.5 final viewport blit (D3D12; seen in 4.5.1).
#define GODOT_VERSION 405
#include "./blit.hlsl"

float4 main(float2 uv: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotBlit(uv, frag_coord);
}
