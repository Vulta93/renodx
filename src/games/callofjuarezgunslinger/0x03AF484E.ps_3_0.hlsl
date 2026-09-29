// Damage "tear" HUD effect, step 2 (also runs in normal frames as a pass-through):
// out = mask^2 * (1 - k*s) + k*s, s = s_rfr (a copy of the composite output). In HDR mode s is
// the HDR-encoded image: decode, blend in linear light (written as k*s*(1-mask^2) + mask^2, the
// same thing without negative values above white), encode again.
#include "./common.hlsli"

float4 CONST_110 : register(c0);
float4 CONST_111 : register(c1);

sampler2D s_nrm : register(s0);
sampler2D s_mask : register(s1);
sampler2D s_rfr : register(s2);

float4 main(float4 v0 : TEXCOORD0, float4 v1 : TEXCOORD1, float2 v2 : TEXCOORD2) : COLOR {
  float a = saturate(v2.x);
  float k = CONST_110.w * (1.f - a * a);

  float4 nrm = tex2D(s_nrm, v1.xy);
  float2 n = float2(nrm.w, nrm.y) * 2.f - 1.f;
  float w = dot(n, v1.zw);
  float x = dot(n, v0.xy);

  float3 uvz = float3(x, -w, -w) * CONST_111.y + v0.zww;
  float k_y = -w * CONST_110.z;
  float k_z = w * CONST_110.z + v2.y;
  k = k * k_z + k_y;
  k = k + 1.f;
  k = k * k;
  k = k * k;

  float4 s = tex2Dlod(s_rfr, float4(uvz.xy, 0.f, 0.f));

  float m = saturate(tex2D(s_mask, v1.xy).y * CONST_110.x + CONST_110.y);
  m = m * m;

  float alpha = k * s.a * (1.f - m) + m;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    float3 color = k * CoJDecodeHDR(s.rgb) * (1.f - m) + m;
    return float4(CoJEncodeHDR(color), alpha);
  }
  return float4(k * s.rgb * (1.f - m) + m, alpha);
}
