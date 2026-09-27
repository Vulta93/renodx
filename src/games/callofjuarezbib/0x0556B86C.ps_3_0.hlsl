// Main tonemap composite variant: tonemap + DOF + desaturation + two masked
// levels/gamma grades (mask = scene alpha) + glow + overlay/vignette multiply.
#include "./common.hlsli"

float4 CONST_100 : register(c0);  // .z glow scale, .w curve white param
float4 CONST_101 : register(c1);  // .y extra DOF amount, .w desaturation (both * (1 - alpha))
float4 CONST_105 : register(c2);  // .w grade strength
float4 CONST_109 : register(c3);  // levels scale (xyz: grade A, w: grade B)
float4 CONST_110 : register(c4);  // levels offset
float4 CONST_111 : register(c5);  // gamma
float4 CONST_112 : register(c6);  // output scale
float4 CONST_113 : register(c7);  // output offset
float4 CONST_114 : register(c8);  // overlay scale (.x) / offset (.y)
float4 CONST_70 : register(c9);   // .y exposure scale

sampler2D s_clr : register(s0);
sampler2D s_avg : register(s1);
sampler2D s_msk : register(s2);
sampler2D s_overlay : register(s3);
sampler2D s_blur : register(s4);
sampler2D s_glow : register(s5);

// Vanilla desaturation + masked grades, gamma 2.0 encoded in and out (0..1).
float3 VanillaGrades(float3 color, float alpha) {
  float lum = saturate(dot(color, 1.f / 3.f));
  color = saturate(lerp(color, lum, CONST_101.w * (1.f - alpha)));

  float3 grade_a = saturate(color * CONST_109.xyz + CONST_110.xyz);
  grade_a = exp2(log2(grade_a) * CONST_111.xyz) * CONST_112.xyz + CONST_113.xyz;
  color = saturate(lerp(color, grade_a, CONST_105.w * (1.f - alpha)));

  float3 grade_b = saturate(color * CONST_109.w + CONST_110.w);
  grade_b = exp2(log2(grade_b) * CONST_111.w) * CONST_112.w + CONST_113.w;
  color = saturate(lerp(color, grade_b, CONST_105.w * alpha));
  return color;
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float avg = tex2Dlod(s_avg, float4(0, 0, 0, 0)).x;
  float4 clr = tex2D(s_clr, uv);

  float3 x = clr.rgb * CONST_70.y * avg;
  float3 y = CoJReinhard(x, CONST_100.w);

  float4 msk = tex2D(s_msk, uv);
  float dof = saturate(msk.w * 2.f - 1.f);
  float4 blur = tex2D(s_blur, uv);
  float blur_mix = saturate(max(saturate(max(blur.w, msk.z)), CONST_101.y * (1.f - clr.a)));
  float3 glow = saturate(tex2D(s_glow, uv).rgb);
  float3 overlay = saturate(CONST_114.x * tex2Dlod(s_overlay, float4(uv, 0, 0)).rgb + CONST_114.y);

  float3 color;
  if (RENODX_TONE_MAP_TYPE > 0.f) {
    color = renodx::color::gamma::EncodeSafe(CoJUntonemapped(x, y), 2.f);
    float3 blur_hdr = CoJBlurHDR(blur.rgb, color);
    color = lerp(color, blur_hdr, dof);
    color = lerp(color, blur_hdr, blur_mix);

    // Grades are clamped gamma-space ops: run them on an SDR stand-in and transfer.
    float3 hdr = renodx::color::gamma::DecodeSafe(color, 2.f);
    float3 sdr = renodx::tonemap::renodrt::NeutralSDR(hdr);
    float3 graded = renodx::color::gamma::DecodeSafe(VanillaGrades(sqrt(sdr), clr.a), 2.f);
    color = renodx::tonemap::UpgradeToneMap(hdr, sdr, graded, 1.f);
    color = renodx::color::gamma::EncodeSafe(max(0, color), 2.f);

    color = max(0, glow * CONST_100.z + color) * overlay;
  } else {
    color = sqrt(y);
    color = saturate(lerp(color, blur.rgb, dof));
    color = saturate(lerp(color, blur.rgb, blur_mix));
    color = VanillaGrades(color, clr.a);
    color = saturate(glow * CONST_100.z + color) * overlay;
  }

  return float4(color, clr.a);
}
