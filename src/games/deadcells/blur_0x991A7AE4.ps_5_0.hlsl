// 0x991A7AE4: 5-tap glow blur (Heaps), run twice per frame on the 642x362 glow layer
// (render target 2 of 0x01A7A161 / 0x8F0EAF1C, later added to the scene by 0x40BF5761).
// Reconstructed 1:1 from the disassembly of renodx-dev/dump/0x991A7AE4.ps_5_0.cso.
//
// Fix: in the original the glow layer is R8G8B8A8_UNORM, so every additive sprite blend into it and the blur's own
// write are clamped to 0..1. After the float16 upgrade the layer kept summing; its alpha measured 3.5-7.2 across
// the whole frame. 0x40BF5761 blends the glow with source alpha, so the glow reached the scene several times too
// bright ("lightsaber" sword). Clamping the taps (= the sprite blends, all additive and non-negative) and the
// output restores the original 8-bit behaviour.
#include "./common.hlsl"

cbuffer _params : register(b1) { float4 fragmentParams[3]; }

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 position : SV_POSITION,
    float2 uv : uv,
    out float4 o0 : SV_TARGET0)
{
  const float2 near_offset = fragmentParams[2].xy * fragmentParams[1].y;
  const float2 far_offset = 2.f * fragmentParams[1].z * fragmentParams[2].xy;

  float4 blurred = saturate(t0.Sample(s0_s, uv)) * fragmentParams[0].x;
  blurred += saturate(t0.Sample(s0_s, uv - near_offset)) * fragmentParams[0].y;
  blurred += saturate(t0.Sample(s0_s, uv + near_offset)) * fragmentParams[0].y;
  blurred += saturate(t0.Sample(s0_s, uv - far_offset)) * fragmentParams[0].z;
  blurred += saturate(t0.Sample(s0_s, uv + far_offset)) * fragmentParams[0].z;
  o0 = saturate(blurred);
}
