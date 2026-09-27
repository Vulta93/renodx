// Tonemap pre-pass variant: as 0xDA9C7047, plus a 16-tap DOF mask average in oC0.w.
#include "./common.hlsli"

float4 CONST_100 : register(c0);  // .x bright-pass scale, .y/.z threshold, .w curve white param
float4 CONST_70 : register(c1);   // .y exposure scale

sampler2D s_clr : register(s0);
sampler2D s_avg : register(s1);
sampler2D s_msk : register(s2);

float MaskTap(float2 uv) {
  return saturate(1.f - 2.f * tex2D(s_msk, uv).w) * 0.0625f;
}

void main(float2 uv : TEXCOORD0,
          float4 t1 : TEXCOORD1,
          float4 t2 : TEXCOORD2,
          float4 t3 : TEXCOORD3,
          float4 t4 : TEXCOORD4,
          out float4 o0 : COLOR0,
          out float4 o1 : COLOR1) {
  float mask = 0.f;
  mask += MaskTap(t1.xw) + MaskTap(t1.xy) + MaskTap(t1.zy) + MaskTap(t1.zw);
  mask += MaskTap(t2.xy) + MaskTap(t2.xw) + MaskTap(t2.zy) + MaskTap(t2.zw);
  mask += MaskTap(t3.xy) + MaskTap(t3.xw) + MaskTap(t3.zy) + MaskTap(t3.zw);
  mask += MaskTap(t4.xy) + MaskTap(t4.xw) + MaskTap(t4.zy) + MaskTap(t4.zw);

  float3 x = tex2D(s_clr, uv).rgb * CONST_70.y;
  x *= tex2Dlod(s_avg, float4(0, 0, 0, 0)).x;
  float3 y = CoJReinhard(x, CONST_100.w);

  o1 = float4(CoJBrightPass(y, CONST_100), 0.f);

  if (RENODX_TONE_MAP_TYPE > 0.f) {
    o0 = float4(renodx::color::gamma::EncodeSafe(CoJUntonemapped(x, y), 2.f), mask);
  } else {
    o0 = float4(sqrt(y), mask);
  }
}
