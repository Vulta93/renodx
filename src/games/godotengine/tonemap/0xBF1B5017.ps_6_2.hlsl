// Godot 4.6 Forward+ tonemap, 3D LUT, bicubic glow upscale (D3D12; seen in 4.6.3).
#define USE_GLOW_FILTER_BICUBIC
#include "./tonemap.hlsl"

float4 main(float2 uv_interp: TEXCOORD0, float4 frag_coord: SV_Position) : SV_Target {
  return GodotTonemap(uv_interp, frag_coord);
}
