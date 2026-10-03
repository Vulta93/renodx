// Chrome Engine 5 (Call of Juarez: Gunslinger) post-process composite.
// One uber-shader with 6 optional features -> 64 permutations (0x*.ps_3_0.hlsl in this
// folder each declare their own registers + COJ_* flags and include this file).
//
// Vanilla: the lit scene (8-bit, clipped) plus glow is saturated - that clip is the
// game's only "tone mapping" - then saturation, noise, desaturate/tint, a 1D curve LUT
// (s_crv, optionally in gamma 2.2), levels, colorize and an overlay multiply.
// The result is linear; the final pass 0x4003CC02 applies GAMMA.
//
// HDR ("Extended"): the vanilla grade is rebuilt on the unclipped float16 scene (CoJGradeExt),
// the sky's few over-white values are expanded (CoJExpandHDR, Sky HDR Boost) and the sun is
// brightened separately.
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

// ---------------------------------------------------------------------------------------
// Extended finishing pipeline ("Extended" tone mapper): the same steps as CoJGrade, but on
// the UNCLIPPED scene. Exact wherever vanilla did not clip; above white each step continues
// instead of clamping:
//  - weights / masks (dot products) stay saturated, colour values are only floored at 0;
//  - the s_crv LUT continues linearly with the slope of its last segment (clamped);
//  - the levels pow() continues along its tangent at 1;
//  - the tint's luminance weight stays <= 1 (its "lum * c" would otherwise square highlights).
static const float COJ_CURVE_SLOPE_MIN = 0.5f;
static const float COJ_CURVE_SLOPE_MAX = 1.f;

float3 CoJCurveExt(float3 x) {
  float3 xc = min(x, 1.f);
  float3 coords = xc * 0.96875f + 0.015625f;
  float3 curve = float3(
      tex2Dlod(s_crv, float4(coords.x, coords.x, 0.f, 0.f)).x,
      tex2Dlod(s_crv, float4(coords.y, coords.y, 0.f, 0.f)).y,
      tex2Dlod(s_crv, float4(coords.z, coords.z, 0.f, 0.f)).z);
  // Last LUT segment: texel centres 30.5/32 and 31.5/32 (1/31 apart in x).
  float3 prev = float3(
      tex2Dlod(s_crv, float4(0.953125f, 0.953125f, 0.f, 0.f)).x,
      tex2Dlod(s_crv, float4(0.953125f, 0.953125f, 0.f, 0.f)).y,
      tex2Dlod(s_crv, float4(0.953125f, 0.953125f, 0.f, 0.f)).z);
  float3 top = float3(
      tex2Dlod(s_crv, float4(0.984375f, 0.984375f, 0.f, 0.f)).x,
      tex2Dlod(s_crv, float4(0.984375f, 0.984375f, 0.f, 0.f)).y,
      tex2Dlod(s_crv, float4(0.984375f, 0.984375f, 0.f, 0.f)).z);
  float3 slope = clamp((top - prev) * 31.f, COJ_CURVE_SLOPE_MIN, COJ_CURVE_SLOPE_MAX);
  return curve + max(x - 1.f, 0.f) * slope;
}

float3 CoJPowExt(float3 x, float3 p) {
  return pow(min(x, 1.f), p) + max(x - 1.f, 0.f) * p;
}

float3 CoJGradeExt(float3 c, float noise) {
  c = max(0, c);
  float lum = dot(c, COJ_LUMA);
  c = max(0, lerp(lum.xxx, c, CONST_102.w));

#if COJ_NOISE
  c = max(0, c + noise);
#endif

#if COJ_DESATURATE
  float desat_lum = saturate(dot(c, v_pp_desaturate_factor_lum.rgb));
  float3 tint = desat_lum * v_pp_desaturate_tint__weight.rgb;
  float3 masked = c * v_pp_desaturate_tint_masked.rgb - tint;
  float mask = saturate(dot(c, v_pp_desaturate_factor_mask.rgb));
  float3 desat = max(0, mask * masked + tint);
  c = max(0, c * v_pp_desaturate_tint__weight.w + desat);
#endif

  bool curves_gamma = f_curves_new.x > 0.f;
  float3 x = curves_gamma ? exp2(log2(c) * (1.f / 2.2f)) : c;
  float3 curve = CoJCurveExt(x);
  c = curves_gamma ? exp2(log2(max(curve, 1e-6f)) * 2.2f) : curve;

#if COJ_LEVELS
  c = max(0, c * CONST_103.rgb + CONST_104.rgb);
  c = CoJPowExt(c, float3(CONST_104.w, CONST_105.w, CONST_106.w));
  c = max(0, c * CONST_105.rgb + CONST_106.rgb);
#endif

#if COJ_TINT
  float tint_lum = saturate(dot(CONST_101.rgb, c));
  float3 tinted = max(0, tint_lum * c * CONST_102.rgb);
  c = max(0, lerp(c, tinted, CONST_101.w));
#endif

  return c;
}

// SDR -> HDR expansion. With correct lighting the game renders almost nothing above
// white, so HDR is created from the finished vanilla image: luminance below the
// "Highlight Start" knee is untouched (vanilla), above it a quadratic curve
// (slope 1 at the knee, continuous) maps white (1.0) to R x game white, where
// R = lerp(1, peak / game, "HDR Boost"). Hue is kept (luminance scaling).
float3 CoJExpandHDR(float3 sdr, float boost) {
  float knee = saturate(CUSTOM_HIGHLIGHT_START);
  float peak_ratio = max(1.f, RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS);
  float r = lerp(1.f, peak_ratio, saturate(boost));

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

// Display map (Neutwo-style): exact up to game white (1.0), above it a Neutwo shoulder
// (slope 1 at white) towards peak. Scales by the max channel, so hue is kept.
float3 CoJDisplayMap(float3 c, float peak_ratio) {
  float m = max(c.r, max(c.g, c.b));
  if (m <= 1.f) return c;
  float p = max(1.f, peak_ratio);
  float new_m = 1.f + renodx::tonemap::Neutwo(m - 1.f, max(1e-3f, p - 1.f));
  return c * (new_m / m);
}

// Sun detection without a marker: the sky has nothing above white of its own, so a SKY pixel
// (alpha < 0, see the sky marker) above white is the sun sprite (vanilla value ~2.2 x white).
// Clouds in front of the sun lower it continuously, so they occlude the boost naturally.
float CoJSkyCover(float4 c) {
  return saturate(-c.a);
}

float CoJSunMarker(float4 c) {
  return smoothstep(1.05f, 1.8f, max(c.r, max(c.g, c.b))) * CoJSkyCover(c);
}

// The sun blurs below use few taps over a wide radius. A world object in front of the sun (the
// gun) punches a hole in the marker, and every tap that lands on it makes a visible step: ghost
// copies of the gun's outline. So each blur is divided by the SKY weight it collected instead of
// the full kernel weight: the sun carries on behind the occluder. Floor 0.5 of the kernel weight
// keeps the result continuous (and bounded) where almost nothing is sky.

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
  float4 clr_a = tex2Dlod(s_clr, uv_lod);
  float3 clr = clr_a.rgb;
  // Sky marker: 0x3848A019 writes alpha as -1 - a in HDR mode (see common.hlsli).
  float sky_mask = (RENODX_TONE_MAP_TYPE > 0.f) ? saturate(-clr_a.a) : 0.f;
  // Glow chain: vanilla 8-bit (<= 1); clamp in case it gets upgraded.
  float3 glow = saturate(tex2Dlod(s_glow, uv_lod).rgb);
#if COJ_BLUR
  float4 blur = tex2Dlod(s_blur, uv_lod);
#endif

  // Sun disc: marked by 0x795E3B26 with a huge value (only in HDR mode). The sprite has a
  // hard edge and a flat outer rim, so the marker is blurred (centre + 4 rings of 8 taps,
  // bell-shaped weights) into a smooth radial falloff: full boost in the core, fading
  // gradually through the rim into the sky with no plateau.
  float sun_mask = 0.f;
  // The sun blurs below only matter near the sun: the addon passes the sun sprite's screen position
  // and disc radius (0 = no sun this frame). Farther away than the widest blur reach plus the disc
  // radius every tap would read zero, so skip them (same result, a lot cheaper). Distances are in
  // uv-x units with y scaled so they are round in pixels on a 16:9 screen, like the taps.
  float sun_dist = length((uv - float2(CUSTOM_SUN_UV_X, CUSTOM_SUN_UV_Y)) * float2(1.f, 9.f / 16.f));
  float sun_reach = max(0.032f * CUSTOM_SUN_REACH, (CUSTOM_SUN_HALO > 0.f) ? 0.12f * CUSTOM_SUN_HALO_RADIUS : 0.f);
  bool sun_near = CUSTOM_SUN_DISC_RADIUS > 0.f && sun_dist < (sun_reach + CUSTOM_SUN_DISC_RADIUS) * 1.1f + 0.01f;
  if (RENODX_TONE_MAP_TYPE > 0.f && CUSTOM_SUN_BRIGHTNESS > 0.f && sun_near) {
    static const float2 dirs[8] = {
        float2(1.f, 0.f), float2(0.7071f, 0.7071f), float2(0.f, 1.f), float2(-0.7071f, 0.7071f),
        float2(-1.f, 0.f), float2(-0.7071f, -0.7071f), float2(0.f, -1.f), float2(0.7071f, -0.7071f)};
    static const float ring_weight[4] = {0.9f, 0.7f, 0.45f, 0.25f};
    const float2 aspect = float2(1.f, 16.f / 9.f);
    float sum = CoJSunMarker(clr_a);
    float cover = CoJSkyCover(clr_a);
    float weight = 1.f;
    [unroll] for (int ring = 0; ring < 4; ++ring) {
      float radius = 0.008f * (ring + 1) * CUSTOM_SUN_REACH;
      float ring_sum = 0.f;
      float ring_cover = 0.f;
      [unroll] for (int k = 0; k < 8; ++k) {
        float4 tap = tex2Dlod(s_clr, float4(uv + dirs[k] * aspect * radius, 0.f, 0.f));
        ring_sum += CoJSunMarker(tap);
        ring_cover += CoJSkyCover(tap);
      }
      sum += ring_sum / 8.f * ring_weight[ring];
      cover += ring_cover / 8.f * ring_weight[ring];
      weight += ring_weight[ring];
    }
    float m = pow(saturate(sum / max(cover, 0.5f * weight) * 1.6f), max(0.1f, CUSTOM_SUN_FALLOFF));
    sun_mask = m * m * (3.f - 2.f * m);
  }

  // Sun core profile: the sprite is a FLAT-topped disc (measured), so a radial "distance from
  // the centre" is synthesised from a Gaussian-weighted blur of the marker (4 rings x 12 taps,
  // rotated per ring, centre excluded): ~1 at the disc centre, ~0.4 at its rim, smooth between.
  // Only evaluated near the sun.
  float sun_core = 0.f;
  [branch] if (sun_mask > 0.f) {
    const float2 paspect = float2(1.f, 16.f / 9.f);
    float p_sum = 0.f;
    float p_cover = 0.f;
    float p_w = 0.f;
    [unroll] for (int pr = 1; pr <= 4; ++pr) {
      float rel = pr / 4.f;
      float pradius = 0.032f * CUSTOM_SUN_REACH * rel;
      float pw = rel * exp(-2.f * rel * rel);
      float pring = 0.f;
      float pring_cover = 0.f;
      [unroll] for (int pk = 0; pk < 12; ++pk) {
        float ang = pk * (6.2831853f / 12.f) + pr * 2.3999632f;
        float4 ptap = tex2Dlod(s_clr, float4(uv + float2(cos(ang), sin(ang)) * paspect * pradius, 0.f, 0.f));
        pring += CoJSunMarker(ptap);
        pring_cover += CoJSkyCover(ptap);
      }
      p_sum += pring / 12.f * pw;
      p_cover += pring_cover / 12.f * pw;
      p_w += pw;
    }
    sun_core = p_sum / max(p_cover, 0.5f * p_w);
  }

  // Sun halo: a much wider, faint glow around the sun (SDR has a broad glare
  // there that the HDR sun lacks). Wide blur of the same sun marker (6 rings x 8 taps, bell
  // weights); the halo takes the sun's own hue (marker-weighted, normalised colour).
  float halo_mask = 0.f;
  float3 halo_col = 1.f;
  if (RENODX_TONE_MAP_TYPE > 0.f && CUSTOM_SUN_BRIGHTNESS > 0.f && CUSTOM_SUN_HALO > 0.f && sun_near) {
    static const float2 hdirs[8] = {
        float2(1.f, 0.f), float2(0.7071f, 0.7071f), float2(0.f, 1.f), float2(-0.7071f, 0.7071f),
        float2(-1.f, 0.f), float2(-0.7071f, -0.7071f), float2(0.f, -1.f), float2(0.7071f, -0.7071f)};
    static const float hring_weight[6] = {0.92f, 0.72f, 0.5f, 0.3f, 0.16f, 0.08f};
    const float2 haspect = float2(1.f, 16.f / 9.f);
    float mk0 = CoJSunMarker(clr_a);
    float h_sum = mk0;
    float h_cover = CoJSkyCover(clr_a);
    float h_w = 1.f;
    float3 h_col = mk0 * clr / max(1e-4f, max(clr.r, max(clr.g, clr.b)));
    [unroll] for (int hr = 0; hr < 6; ++hr) {
      float hradius = 0.02f * (hr + 1) * CUSTOM_SUN_HALO_RADIUS;
      float ring_m = 0.f;
      float ring_cov = 0.f;
      float3 ring_c = 0.f;
      [unroll] for (int hk = 0; hk < 8; ++hk) {
        float4 hs4 = tex2Dlod(s_clr, float4(uv + hdirs[hk] * haspect * hradius, 0.f, 0.f));
        float3 hs = hs4.rgb;
        float hm = CoJSunMarker(hs4);
        ring_m += hm;
        ring_cov += CoJSkyCover(hs4);
        ring_c += hm * hs / max(1e-4f, max(hs.r, max(hs.g, hs.b)));
      }
      h_sum += ring_m / 8.f * hring_weight[hr];
      h_cover += ring_cov / 8.f * hring_weight[hr];
      h_col += ring_c / 8.f * hring_weight[hr];
      h_w += hring_weight[hr];
    }
    float hmask = saturate(h_sum / max(h_cover, 0.5f * h_w));
    halo_mask = hmask * hmask * (3.f - 2.f * hmask);
    halo_col = h_col / max(1e-4f, h_sum);
  }

  // Vanilla composite (the game's clip at white is its only "tone mapping").
  float glow_scale = CONST_100.w * (RENODX_TONE_MAP_TYPE > 0.f ? CUSTOM_GLOW_STRENGTH : 1.f);
  float3 color = clr;
#if COJ_BLUR
  color = saturate(lerp(color, blur.rgb, saturate(blur.a) * ((RENODX_TONE_MAP_TYPE > 0.f) ? CUSTOM_DOF_STRENGTH : 1.f)));
#endif
  color = saturate(glow * glow_scale + color);
  float3 graded;
  if (RENODX_TONE_MAP_TYPE > 3.5f) {
    // "Extended": vanilla pipeline on the unclipped scene, then a display map (exact up to
    // white, Neutwo shoulder to peak). The DOF blur chain is 8-bit: keep the sharp pixel's
    // part above white.
    float3 xr = min(max(0, clr), 1000.f);
#if COJ_BLUR
    float3 blur_hdr = blur.rgb + max(0, xr - saturate(xr));
    xr = lerp(xr, blur_hdr, saturate(blur.a) * CUSTOM_DOF_STRENGTH);
#endif
    xr += glow * glow_scale;
    graded = CoJGradeExt(xr, noise);
    // Sky only: the game's sky has almost nothing above white, so it keeps the Sky HDR Boost
    // expansion (on the part up to white; anything above white is passed through). The factor
    // comes from the grain-free image so grain does not boil in the clouds.
    if (sky_mask > 0.f) {
#if COJ_NOISE
      float3 sky_clean = CoJGradeExt(xr, 0.f);
#else
      float3 sky_clean = graded;
#endif
      float3 sky_base = saturate(sky_clean);
      float y_base = renodx::color::y::from::BT709(sky_base);
      float y_exp = renodx::color::y::from::BT709(CoJExpandHDR(sky_base, CUSTOM_SKY_HDR_BOOST));
      float3 sky_over = graded - saturate(graded);
      float3 sky_graded = saturate(graded) * ((y_base > 0.f) ? (y_exp / y_base) : 1.f) + sky_over;
      graded = lerp(graded, sky_graded, sky_mask);
    }
    // Highlight Gain: scales only the part above white (1 = the game's own values).
    float mg = max(graded.r, max(graded.g, graded.b));
    if (mg > 1.f) graded *= (1.f + CUSTOM_HIGHLIGHT_GAIN * (mg - 1.f)) / mg;
    graded = CoJDisplayMap(graded, RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS);
  } else {
    graded = CoJGrade(color, noise);
  }

  color = graded;

  if (sun_mask > 0.f) {
    // Sun up to "Sun Brightness" x peak (relative to game white), keeping its hue.
    float peak_ratio = RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
    // Radial core: from the pixel's own (unmarked) level at the disc rim up to Sun Brightness x
    // peak at the centre. Sun Core Shape sets the bell: 0 = broad dome, 100 = small hot core.
    float mc = max(1e-4f, max(color.r, max(color.g, color.b)));
    float t = saturate((sun_core - 0.4f) / 0.55f);
    float shape = pow(t, lerp(0.5f, 3.f, saturate(CUSTOM_SUN_PROFILE)));
    // Scaled by this pixel's own sun detection, so the sprite's soft edge and clouds in front of
    // the sun fade the boost continuously (no cut-out).
    float level = lerp(mc, max(mc, peak_ratio * CUSTOM_SUN_BRIGHTNESS), shape * CoJSunMarker(clr_a));
    color = lerp(color, color * (level / mc), sun_mask);
  }

  if (halo_mask > 0.f) {
    // 100% = up to half the peak (in game-white units) at the mask's maximum; not added on the
    // sun core itself (already at its brightness). Sky pixels only: on world pixels (the gun) the
    // sparse-tap halo showed as pale blocks on the dark surface.
    float halo_peak = RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
    color += halo_col * (halo_mask * (1.f - sun_mask) * CoJSkyCover(clr_a) * CUSTOM_SUN_HALO * halo_peak * 0.5f);
  }

#if COJ_OVERLAY
  color *= tex2Dlod(s_overlay, uv_lod).rgb;
#endif

  if (RENODX_TONE_MAP_TYPE > 0.f) return float4(CoJEncodeHDR(color), 0.f);
  return float4(color, 0.f);
}
