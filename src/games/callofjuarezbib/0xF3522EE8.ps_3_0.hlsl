// Main tonemap composite: exposure + curve + DOF blend + desaturation + glow.
// Output is gamma 2.0 encoded (sqrt), read by the final grade 0x1F6ABA21.
#include "./common.hlsli"

float4 CONST_100 : register(c0);  // .z glow scale, .w curve white param
float4 CONST_101 : register(c1);  // .w desaturation amount
float4 CONST_70 : register(c2);   // .y exposure scale

sampler2D s_clr : register(s0);   // HDR scene
sampler2D s_avg : register(s1);   // 1x1 adaptation
sampler2D s_msk : register(s2);   // DOF mask
sampler2D s_blur : register(s3);  // blurred scene (from 0xDA9C7047 chain)
sampler2D s_glow : register(s4);  // glow

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float avg = tex2Dlod(s_avg, float4(0, 0, 0, 0)).x;
  float4 clr = tex2D(s_clr, uv);

  float3 x = clr.rgb * CONST_70.y * avg;
  float3 y = CoJReinhard(x, CONST_100.w);

  float4 msk = tex2D(s_msk, uv);
  float dof = saturate(msk.w * 2.f - 1.f);
  float4 blur = tex2D(s_blur, uv);
  float blur_mix = saturate(max(blur.w, msk.z));
  float3 glow = tex2D(s_glow, uv).rgb;

  float3 color;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    color = renodx::color::gamma::EncodeSafe(CoJUntonemapped(x, y), 2.f);
    color = lerp(color, blur.rgb, dof);
    color = lerp(color, blur.rgb, blur_mix);
    float lum = dot(color, 1.f / 3.f);
    color = lerp(color, lum, CONST_101.w);
    color = max(0, glow * CONST_100.z + color);
  } else {
    color = sqrt(y);
    color = saturate(lerp(color, blur.rgb, dof));
    color = saturate(lerp(color, blur.rgb, blur_mix));
    float lum = saturate(dot(color, 1.f / 3.f));
    color = saturate(lerp(color, lum, CONST_101.w));
    color = saturate(glow * CONST_100.z + color);
  }

  return float4(color, clr.a);
}
