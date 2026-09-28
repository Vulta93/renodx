// Sky (4096x1024 BC1 panorama: clouds + soft sun glow). Vanilla alpha = glow weight.
// HDR: alpha written as -1 - a (sky marker, see common.hlsli) for separate Sky HDR Boost.
#include "./common.hlsli"

float4 CONST_102 : register(c0);
float4 v_pp_hdr_intensity_max : register(c1);
float4 v_pp_sky_color : register(c2);
sampler2D s_tex : register(s0);

float4 main(float2 uv : TEXCOORD1) : COLOR {
  float3 color = tex2Dlod(s_tex, float4(uv, 0.f, 0.f)).rgb * CONST_102.z + CONST_102.w * v_pp_sky_color.rgb;
  float alpha = dot(v_pp_hdr_intensity_max.rgb, color);
  if (RENODX_TONE_MAP_TYPE > 0.f) alpha = -1.f - alpha;
  return float4(color, alpha);
}
