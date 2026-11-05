// Godot 4.6 final viewport blit (D3D12; seen in 4.6.1 and 4.6.3).
#define GODOT_VERSION 406
#include "./blit.hlsl"

float4 main(float2 uv: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotBlit(uv, frag_coord);
}
