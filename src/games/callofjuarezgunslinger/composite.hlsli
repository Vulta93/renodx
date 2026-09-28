// Chrome Engine 5 (Call of Juarez: Gunslinger) post-process composite.
// One uber-shader with 6 optional features -> 64 permutations (0x*.ps_3_0.hlsl in this
// folder each declare their own registers + COJ_* flags and include this file).
//
// Vanilla: the lit scene (8-bit, clipped) plus glow is saturated - that clip is the
// game's only "tone mapping" - then saturation, noise, desaturate/tint, a 1D curve LUT
// (s_crv, optionally in gamma 2.2), levels, colorize and an overlay multiply.
// The result is linear; the final pass 0x4003CC02 applies GAMMA.
//
// HDR: with the packed-depth target left 8-bit the lit scene is essentially SDR, so the
// vanilla image is rebuilt exactly and its highlights are expanded (CoJExpandHDR).
#include "./common.hlsli"

static const float3 COJ_LUMA = float3(0.2125f, 0.7154f, 0.0721f);

// Everything after the glow add, in vanilla order, on a 0..1 colour.
float3 CoJGrade(float3 c, float noise) {
  float lum = dot(c, COJ_LUMA);
  c = saturate(lerp(lum.xxx, c, CONST_102.w));

#if COJ_NOISE
  c = saturate(c + noise);
#endif

#if COJ_DESATURATE
  float desat_lum = saturate(dot(c, v_pp_desaturate_factor_lum.rgb));
  float3 tint = desat_lum * v_pp_desaturate_tint__weight.rgb;
  float3 masked = c * v_pp_desaturate_tint_masked.rgb - tint;
  float mask = saturate(dot(c, v_pp_desaturate_factor_mask.rgb));
  float3 desat = saturate(mask * masked + tint);
  c = saturate(c * v_pp_desaturate_tint__weight.w + desat);
#endif

  // 1D curve LUT (32 texels: 0.96875 scale + half-texel offset)
  bool curves_gamma = f_curves_new.x > 0.f;
  float3 coords = curves_gamma ? exp2(log2(c) * (1.f / 2.2f)) : c;
  coords = coords * 0.96875f + 0.015625f;
  float3 curve = float3(
      tex2D(s_crv, coords.xx).x,
      tex2D(s_crv, coords.yy).y,
      tex2D(s_crv, coords.zz).z);
  c = curves_gamma ? exp2(log2(curve) * 2.2f) : curve;

#if COJ_LEVELS
  c = saturate(c * CONST_103.rgb + CONST_104.rgb);
  c = pow(c, float3(CONST_104.w, CONST_105.w, CONST_106.w));
  c = saturate(c * CONST_105.rgb + CONST_106.rgb);
#endif

#if COJ_TINT
  float tint_lum = dot(CONST_101.rgb, c);
  float3 tinted = saturate(tint_lum * c * CONST_102.rgb);
  c = saturate(lerp(c, tinted, CONST_101.w));
#endif

  return c;
}

// SDR -> HDR expansion. With correct lighting the game renders almost nothing above
// white, so HDR is created from the finished vanilla image: luminance below the
// "Highlight Start" knee is untouched (vanilla), above it a quadratic curve
// (slope 1 at the knee, continuous) maps white (1.0) to R x game white, where
// R = lerp(1, peak / game, "HDR Boost"). Hue is kept (luminance scaling).
float3 CoJExpandHDR(float3 sdr) {
  float knee = saturate(CUSTOM_HIGHLIGHT_START);
  float peak_ratio = max(1.f, RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS);
  float r = lerp(1.f, peak_ratio, saturate(CUSTOM_HDR_BOOST));

  float y = max(0, renodx::color::y::from::BT709(sdr));
  if (y <= knee || r <= 1.f || knee >= 1.f) return sdr;

  // Quadratic: slope 1 at the knee, ends at r. Max slope 2m-1 (the old inverse-Reinhard
  // curve reached ~35x at white and amplified every 8-bit step / texture wobble near white
  // into "boiling" clouds).
  float m = (r - knee) / (1.f - knee);
  float t = saturate((y - knee) / (1.f - knee));
  float y_new = knee + (1.f - knee) * (t + (m - 1.f) * t * t);
  return sdr * (y_new / y);
}

float4 main(float2 uv : TEXCOORD0
#if COJ_NOISE
            ,
            float4 noise_uv : TEXCOORD3
#endif
            ) : COLOR {
  float noise = 0.f;
#if COJ_NOISE
  float4 noise0 = tex2D(s_noise, noise_uv.xy);
  float4 noise1 = tex2D(s_noise, noise_uv.zw);
  noise = frac(dot(noise0, noise1) + CONST_100.z) * CONST_100.x + CONST_100.y;
#endif

  float4 uv_lod = float4(uv, 0.f, 0.f);
  float3 clr = tex2Dlod(s_clr, uv_lod).rgb;
  // Glow chain: vanilla 8-bit (<= 1); clamp in case it gets upgraded.
  float3 glow = saturate(tex2Dlod(s_glow, uv_lod).rgb);
#if COJ_BLUR
  float4 blur = tex2Dlod(s_blur, uv_lod);
#endif

  // Vanilla composite (the game's clip at white is its only "tone mapping").
  float glow_scale = CONST_100.w * (RENODX_TONE_MAP_TYPE > 0.f ? CUSTOM_GLOW_STRENGTH : 1.f);
  float3 color = clr;
#if COJ_BLUR
  color = saturate(lerp(color, blur.rgb, blur.a));
#endif
  color = saturate(glow * glow_scale + color);
  float3 graded = CoJGrade(color, noise);

  if (RENODX_TONE_MAP_TYPE > 0.f) {
    // Expansion factor from the noise-free image: film grain near white is otherwise
    // amplified by the steep part of the curve ("boiling" bright clouds).
#if COJ_NOISE
    float3 clean = CoJGrade(color, 0.f);
#else
    float3 clean = graded;
#endif
    float y_clean = renodx::color::y::from::BT709(clean);
    float y_expanded = renodx::color::y::from::BT709(CoJExpandHDR(clean));
    graded *= (y_clean > 0.f) ? (y_expanded / y_clean) : 1.f;
  }
  color = graded;

#if COJ_OVERLAY
  color *= tex2Dlod(s_overlay, uv_lod).rgb;
#endif

  if (RENODX_TONE_MAP_TYPE > 0.f) return float4(CoJEncodeHDR(color), 0.f);
  return float4(color, 0.f);
}
