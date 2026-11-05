// Godot 4.4.1 final viewport blit (D3D12; seen in 4.4.1). Its input signature has no SV_Position.
#define GODOT_VERSION 404
#include "./blit.hlsl"

float4 main(float2 uv: TEXCOORD0) : SV_Target {
  return GodotBlit(uv, (float4)0);
}
