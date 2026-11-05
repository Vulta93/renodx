// Godot 4.4.1 Forward+ tonemap, 1D LUT, bilinear glow upscale (D3D12; seen in 4.4.1).
#define GODOT_VERSION 404
#define USE_1D_LUT
#include "./tonemap.hlsl"

float4 main(float2 uv_interp: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotTonemap(uv_interp, frag_coord);
}
