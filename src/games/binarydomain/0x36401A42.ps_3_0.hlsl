#include "./shared.h"

// Binary Domain shadow filter (ps_3_0 0x36401A42): the soft shadow pass. It reads the shadow data at 7 points around a center
// tap and blends the shadow value with weights that fall off with the difference of the second channel (a depth-like value),
// so soft edges do not leak across depth steps: out.x = sum(w * x) / sum(w), out.yzw = center tap yzw. With the original tap
// spacing the shadow edges are visibly blocky (the shadow map is low resolution). Shadow Filter Width scales the distance of
// the 6 outer taps from the center tap: 1 is the original filter, larger values spread the soft edge over a wider area.
// At width 1 the math is that of the original (ported from its disassembly).

float4 c_ps_user[32] : register(c192);  // c192 = tap weights, c194.x = depth difference scale
sampler2D shadow_texture : register(s0);

float4 main(float4 uv_e_f : TEXCOORD2, float4 uv_g_d : TEXCOORD3, float4 uv_a_b : TEXCOORD4, float2 uv_c : TEXCOORD5) : COLOR {
  const float2 center_uv = uv_g_d.zw;
  const float4 center = tex2D(shadow_texture, center_uv);
  const float4 tap_a = tex2D(shadow_texture, center_uv + (uv_a_b.xy - center_uv) * RENODX_SHADOW_FILTER_WIDTH);
  const float4 tap_b = tex2D(shadow_texture, center_uv + (uv_a_b.zw - center_uv) * RENODX_SHADOW_FILTER_WIDTH);
  const float4 tap_c = tex2D(shadow_texture, center_uv + (uv_c - center_uv) * RENODX_SHADOW_FILTER_WIDTH);
  const float4 tap_e = tex2D(shadow_texture, center_uv + (uv_e_f.xy - center_uv) * RENODX_SHADOW_FILTER_WIDTH);
  const float4 tap_f = tex2D(shadow_texture, center_uv + (uv_e_f.zw - center_uv) * RENODX_SHADOW_FILTER_WIDTH);
  const float4 tap_g = tex2D(shadow_texture, center_uv + (uv_g_d.xy - center_uv) * RENODX_SHADOW_FILTER_WIDTH);

  const float4 weights_center_a_b_c =
      saturate(c_ps_user[2].x * -abs(float4(0.f, tap_a.y - center.y, tap_b.y - center.y, tap_c.y - center.y)) + c_ps_user[0]);
  const float3 weights_e_f_g =
      saturate(c_ps_user[2].x * -abs(float3(tap_e.y - center.y, tap_f.y - center.y, tap_g.y - center.y)) + c_ps_user[0].wzy);
  const float weighted_shadow = dot(weights_center_a_b_c, float4(center.x, tap_a.x, tap_b.x, tap_c.x))
                                + dot(weights_e_f_g, float3(tap_e.x, tap_f.x, tap_g.x));
  const float total_weight = dot(weights_center_a_b_c, 1.f) + dot(weights_e_f_g, 1.f);
  return float4(weighted_shadow / total_weight, center.yzw);
}
