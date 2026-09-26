// Gujian 3 (Vision Engine) - final tonemap / composite pass.
// Runs after lens flare, before SMAA and the UI. Replaces the vanilla
// exponential curve + 0..1 clamp with RenoDX tone mapping.
#include "./shared.h"

Texture2D<float4> t0 : register(t0);  // scene (HDR)
Texture2D<float4> t1 : register(t1);  // bloom
Texture2D<float4> t2 : register(t2);  // low-res glow / tint
Texture2D<float4> t3 : register(t3);  // dither noise
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
SamplerState s3 : register(s3);

// $Globals: afRGBA_Modulate[32] @ c154, afRGBA_Offset[16] @ c186,
// fParam_GammaCorrection @ c202.x, fParam_DitherOffsetScale @ c202.yz,
// fParam_TonemapMaxMappingLuminance @ c208.xy
cbuffer cb0 : register(b0) {
  float4 cb0[209];
}

// Vanilla curve: (1 - e^-kx) * (1 - b e^-kx)^2
float3 VanillaCurve(float3 x, float k, float b) {
  float3 e = exp2(-x * k * 1.44269502f);
  float3 shoulder = 1.f - b * e;
  return (1.f - e) * (shoulder * shoulder);
}

void main(
    float2 v0 : TEXCOORD0,
    float2 v1 : TEXCOORD1,
    float2 v2 : TEXCOORD2,
    float2 v3 : TEXCOORD3,
    float2 v4 : TEXCOORD4,
    out float4 o0 : SV_Target0) {
  // Glow texture -> additive term and multiplier (each with its own saturation / scale-bias)
  float3 glow = t2.Sample(s2, v3).rgb;
  float3 glow_add = glow * cb0[186].x + cb0[186].y;
  float3 glow_mul = glow * cb0[188].x + cb0[188].y;

  float avg = dot(glow_add, 1.f / 3.f);
  glow_add = saturate(cb0[187].x * (glow_add - avg) + avg);
  glow_add = glow_add * cb0[186].z + cb0[186].w;

  avg = dot(glow_mul, 1.f / 3.f);
  glow_mul = cb0[189].x * (glow_mul - avg) + avg;
  glow_mul = glow_mul * cb0[188].z + cb0[188].w;

  float3 bloom = t1.Sample(s1, v1).rgb;
  float3 additive = saturate((bloom * glow_mul + glow_add) * cb0[154].y);

  float4 scene = t0.Sample(s0, v0);
  float alpha = saturate(dot(scene, cb0[155]));  // luma for AA

  float3 x = max(5.96046448e-08f, scene.rgb * glow_mul * cb0[154].x);
  const float k = cb0[208].x;
  const float b = cb0[208].y;
  const float gamma = cb0[202].x;

  // ---- Vanilla SDR path (1:1 with the original) ----
  float3 curve = saturate(VanillaCurve(x, k, b));
  float3 composed = additive * (1.f - curve) + curve;  // screen blend
  composed = min(composed + 2.38418579e-07f, 1.f);
  float3 vanilla_encoded = exp2(log2(composed) * gamma);

  if (RENODX_TONE_MAP_TYPE == 0.f) {
    float dither = t3.Sample(s3, v4).x;
    vanilla_encoded += dither * cb0[202].y + cb0[202].z;
    o0.rgb = renodx::draw::RenderIntermediatePass(renodx::color::srgb::DecodeSafe(vanilla_encoded));
    o0.a = alpha;
    return;
  }

  // ---- HDR path ----
  // Untonemapped: the curve's linear toe (slope at 0 = k * (1 - b)^2), no clamp.
  float slope = max(k * (1.f - b) * (1.f - b), 1e-4f);
  float3 hdr_composed = x * slope + additive * (1.f - curve);
  float3 hdr_encoded = renodx::math::SignPow(hdr_composed, gamma.xxx);

  float3 untonemapped = renodx::color::srgb::DecodeSafe(hdr_encoded);
  float3 sdr_graded = renodx::color::srgb::DecodeSafe(vanilla_encoded);

  float3 color = renodx::draw::ToneMapPass(untonemapped, sdr_graded);
  o0.rgb = renodx::draw::RenderIntermediatePass(color);
  o0.a = alpha;
}
