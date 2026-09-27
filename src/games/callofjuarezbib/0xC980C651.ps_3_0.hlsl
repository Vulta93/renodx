// Main tonemap composite variant: as 0xF3522EE8 without the desaturation step.
#include "./common.hlsli"

float4 CONST_100 : register(c0);  // .z glow scale, .w curve white param
float4 CONST_70 : register(c1);   // .y exposure scale

sampler2D s_clr : register(s0);
sampler2D s_avg : register(s1);
sampler2D s_msk : register(s2);
sampler2D s_blur : register(s3);
sampler2D s_glow : register(s4);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float avg = tex2Dlod(s_avg, float4(0, 0, 0, 0)).x;
  float4 clr = tex2D(s_clr, uv);

  float3 x = clr.rgb * CONST_70.y * avg;
  float3 y = CoJReinhard(x, CONST_100.w);

  float4 msk = tex2D(s_msk, uv);
  float dof = saturate(msk.w * 2.f - 1.f);
  float4 blur = tex2D(s_blur, uv);
  float blur_mix = saturate(max(blur.w, msk.z));
  float3 glow = saturate(tex2D(s_glow, uv).rgb);

  float3 color;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    color = renodx::color::gamma::EncodeSafe(CoJUntonemapped(x, y), 2.f);
    float3 blur_hdr = CoJBlurHDR(blur.rgb, color);
    color = lerp(color, blur_hdr, dof);
    color = lerp(color, blur_hdr, blur_mix);
    color = max(0, glow * CONST_100.z + color);
  } else {
    color = sqrt(y);
    color = saturate(lerp(color, blur.rgb, dof));
    color = saturate(lerp(color, blur.rgb, blur_mix));
    color = saturate(glow * CONST_100.z + color);
  }

  return float4(color, clr.a);
}
