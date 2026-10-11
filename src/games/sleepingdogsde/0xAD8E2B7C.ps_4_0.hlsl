// World-space icons (NPC dialogue indicator; alpha-tested, dither-faded): the texture colour is written into the encoded
// scene buffer without the 1.04x / (x + 0.2) encode, so white lands at y = 1 (x = 5, lamp-core level) and the pulse
// reached peak brightness in HDR. In the HDR modes the colour is scaled down (max channel, hue kept) to y = 0.82
// (x ~0.745): the scene value the HDR path outputs as SDR white, the level vanilla clips the icon to. Calibrated for
// RenoDRT at default sliders and white scale Value0.y ~0.87 (measured ~203 nits like vanilla); y = 0.9 (vanilla's own
// white point) measured 401 nits at the pulse.
#include "./shared.h"

cbuffer cbSceneryInstance : register(b0) {
  struct
  {
    float4 ColourTint;
    float4 SIColourTint;
    float4 Mask;
    float4 Value0;
  } cbSceneryInstance : packoffset(c0);
}

SamplerState _texDiffuse_s : register(s0);
SamplerState _texFadeDitherMask_s : register(s1);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texFadeDitherMask : register(t1);

void main(
    float4 v0: SV_Position0,
    float4 v1: TEXCOORD0,
    float4 v2: TEXCOORD1,
    float3 v3: TEXCOORD2,
    out float4 o0: SV_Target0) {
  float4 r0, r1;

  r0.xy = v0.xy * float2(0.25, 0.5);
  r0.xyzw = texFadeDitherMask.Sample(_texFadeDitherMask_s, r0.xy).xyzw;
  if (r0.x + -0.1 < 0) discard;

  r0.x = v1.w;
  r0.y = v2.w;
  r0.xyzw = texDiffuse.SampleBias(_texDiffuse_s, r0.xy, -2).xyzw;
  if (r0.w + -0.5 < 0) discard;

  r1.x = 1 + -r0.w;
  r0.xyz = r0.xyz * r0.xyz;
  o0.w = r0.w;
  r1.yzw = r0.xyz * cbSceneryInstance.ColourTint.xyz + -r0.xyz;
  o0.xyz = r1.xxx * r1.yzw + r0.xyz;

  if (RENODX_TONE_MAP_TYPE != 0.f) {
    o0.xyz *= 0.82f / max(0.82f, renodx::math::Max(o0.xyz));
  }
}
