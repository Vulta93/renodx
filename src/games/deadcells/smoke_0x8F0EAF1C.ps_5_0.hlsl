// Dead Cells (Heaps) main sprite shader, incl. noise-based smoke/fog dissolve.
// Reconstructed 1:1 from renodx-dev/dump/0x8F0EAF1C.ps_5_0.cso.
// Writes 4 render targets: colour, distortion field, glow, light-scatter.
//
// Fix: the game expects 8-bit targets, which silently clamp every value to 0..1.
// With the R8G8B8A8_UNORM -> float16 resource upgrade that clamp is gone, so alpha,
// distortion offsets and glow can go below 0 / above 1 ("odd looking smoke",
// over-bright glows). We re-apply the clamp here, except for the colour's RGB so
// the world keeps HDR headroom.
#include "./common.hlsl"

cbuffer cb0 : register(b0) { float4 cb0[1]; }
cbuffer cb1 : register(b1) { float4 cb1[2]; }

Texture2D<float4> t0 : register(t0);  // sprite texture
Texture2D<float4> t1 : register(t1);  // smoke noise texture
SamplerState s0_s : register(s0);
SamplerState s1_s : register(s1);

void main(
    float4 position : SV_POSITION,
    float4 pixelColor : pixelColor,
    float2 calculatedUV : calculatedUV,
    float lscatContrib : lscatContrib,
    float displaceBias : displaceBias,
    float3 glowColor : glowColor,
    float3 lscatColor : lscatColor,
    float4 absolutePosition : absolutePosition2_,
    out float4 o0 : SV_TARGET0,
    out float4 o1 : SV_TARGET1,
    out float4 o2 : SV_TARGET2,
    out float4 o3 : SV_TARGET3)
{
  // --- smoke noise (world-space, scrolling) ---
  float4 r0 = absolutePosition.xyxy * cb1[0].yzyz;
  r0 /= cb1[1].z;
  float scroll = cb0[0].x * cb1[1].y;
  r0 = scroll * float4(-0.6f, 1.0f, 0.07f, 0.38f) + r0;
  float noise = t1.Sample(s1_s, r0.xy).x * t1.Sample(s1_s, r0.zw).x;
  noise = exp2(log2(noise) * cb1[1].w);          // pow(noise, cb1[1].w)
  noise = cb1[1].x * (noise - 1.0f) + 1.0f;      // lerp(1, noise, cb1[1].x)
  noise *= cb1[0].w;

  // --- sprite colour ---
  float4 color = t0.Sample(s0_s, calculatedUV) * pixelColor;
  color.a *= noise;

  // FIX: alpha back to 0..1, RGB non-negative (HDR headroom kept)
  color.a = saturate(color.a);
  color.rgb = max(0.0f, color.rgb);

  o0 = color * (1.0f - displaceBias);

  // --- distortion field ---
  float4 base = float4(0.5f, 0.5f, 0.0f, color.a * cb1[0].x);
  o1 = saturate(lerp(base, color, displaceBias));  // FIX: clamp like 8-bit target

  // --- glow and light-scatter layers ---
  o2 = saturate(float4(glowColor, color.a));                 // FIX
  o3 = saturate(float4(lscatColor, color.a * lscatContrib)); // FIX
}
