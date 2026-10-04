// Shared body of the Fable III (Lionhead Albion engine) final scene composite. The game compiles several permutations of this
// shader (different constant registers / samplers, optional dust planes and screen-space displacement). Each permutation file
// declares its own constants and samplers (names as in the original CTAB), defines FABLE3_HAS_DUST / FABLE3_HAS_DISPLACEMENT
// when the original has those inputs (and FABLE3_HAS_DOF when it has the depth buffer + depth-of-field inputs; dust needs it
// too), then includes this file.
//
// Vanilla: displacement -> optional screen-space AA -> dust planes -> depth of field -> luma tone curve (1D texture built
// every frame from the luminance histogram) used as a colour scale + saturate -> screen blend with bloom -> saturation ->
// pow(x, g_GlobalGammaAdjustment). Reconstructed 1:1 from the ps_3_0 disassembly; the RENODX_EFFECT_* switches only skip
// parts of it.

// Vanilla+ highlight expansion. The driver is the brightest channel of the RAW HDR scene colour (before the game's exposure and tone
// curve). Measured in game (debug false-colour view): ordinary surfaces sit below 1, sunlit cloth and fountain spray reach 2-4, the sky
// 2-5 and thin specular edges 6-8, so there is no brightness threshold that separates one kind of highlight from another. The curve is
// therefore a single smooth climb: up to the knee the image is exactly vanilla, above it the brightest channel of the vanilla result
// moves toward the display peak, slowly saturating:
//   u = max(driver - knee, 0) / range;  r = u^2 / (1 + u^2) * weight;  max' = max + (peak - max) * r
// r is 0 with zero slope at the knee (C1), monotonic, and max' never exceeds the peak. Scaling the whole colour by max'/max keeps the
// vanilla hue ratios. Values are in the game's linear space where 1.0 = Game Brightness; peak = Peak / Game Brightness.
// weight protects what is close to the camera (the player characters are always there, and in the raw scene their sunlit cloth is as
// bright as the sky, so brightness cannot tell them apart; scene alpha does not mark them either): 0 up to NEAR_PROTECT_START
// view-depth units, 1 from NEAR_PROTECT_END, smooth in between. The sky (depth >= 200) and
// distant objects get the full lift. Known cost: effects very close to the camera (own spells, a nearby campfire) stay vanilla.
// Without a depth buffer (the dialogue composite) nothing is lifted.
// Rejected drivers (all tried in game): exposed scene luminance (never reached the knee), vanilla output brightness (the sky is always
// 1.0), raw brightness with a high threshold (kills the little range the game has) and with neutral-colour rejection (also rejects
// white spray).
static const float NEAR_PROTECT_START = 8.f;
static const float NEAR_PROTECT_END = 12.f;

float3 ExpandVanillaHighlights(float3 vanilla_linear, float driver, float weight) {
  float range = max(RENODX_HIGHLIGHT_RANGE, 1e-3f);
  float u = max(driver - RENODX_HIGHLIGHT_KNEE, 0.f) / range;
  float expansion = (u * u) / (1.f + u * u) * weight;
  float max_channel = max(vanilla_linear.r, max(vanilla_linear.g, vanilla_linear.b));
  float peak = max(RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS, 1.f);
  float expanded_max = max_channel + (peak - max_channel) * expansion;
  return vanilla_linear * (expanded_max / max(max_channel, 1e-6f));
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  static const float3 LUMA_WEIGHTS = float3(0.2125f, 0.7154f, 0.0721f);
  static const float3 EDGE_LUMA_WEIGHTS = float3(0.212f, 0.716f, 0.072f);

  float2 scene_uv = uv;
#ifdef FABLE3_HAS_DISPLACEMENT
  [branch]
  if (RENODX_EFFECT_DISPLACEMENT != 0.f) {
    float4 displacement = tex2D(g_DisplacementSampler, uv);
    scene_uv = float2(displacement.y - displacement.x, displacement.w - displacement.z) * g_DisplacementScale.xy + uv;
  }
#endif

  float3 scene = tex2D(g_HDRSampler, scene_uv).rgb;

  // The bool constant must be tested on its own so that the compiler binds it to b0 (combined with a float test it is
  // turned into a float constant, which the game never sets).
  [branch]
  if (g_EnableScreenspaceAA) {
    [branch]
    if (RENODX_EFFECT_ANTIALIASING != 0.f) {
      float2 texel = g_InputResolution.xy;
      float luma_north = dot(tex2Dlod(g_HDRSampler, float4(scene_uv + texel * float2(0.f, -0.7f), 0.f, 0.f)).rgb, EDGE_LUMA_WEIGHTS);
      float luma_west = dot(tex2Dlod(g_HDRSampler, float4(scene_uv + texel * float2(-0.7f, 0.f), 0.f, 0.f)).rgb, EDGE_LUMA_WEIGHTS);
      float luma_east = dot(tex2Dlod(g_HDRSampler, float4(scene_uv + texel * float2(0.7f, 0.f), 0.f, 0.f)).rgb, EDGE_LUMA_WEIGHTS);
      float luma_south = dot(tex2Dlod(g_HDRSampler, float4(scene_uv + texel * float2(0.f, 0.7f), 0.f, 0.f)).rgb, EDGE_LUMA_WEIGHTS);
      float2 gradient = float2(luma_south - luma_north, luma_east - luma_west);
      float edge = sqrt(dot(gradient, gradient));

      // The blur offset is in UV units (not texels); this matches the original.
      float blur_scale = saturate(edge * texel.x * 0.5f);
      static const float3 BLUR_TAPS[12] = {
        float3(0.f, -1.f, 3.f), float3(-1.f, 0.f, 3.f), float3(1.f, 0.f, 3.f), float3(0.f, 1.f, 3.f),
        float3(-1.f, -1.f, 2.f), float3(-1.f, 1.f, 2.f), float3(1.f, -1.f, 2.f), float3(1.f, 1.f, 2.f),
        float3(0.f, -2.f, 1.f), float3(-2.f, 0.f, 1.f), float3(2.f, 0.f, 1.f), float3(0.f, 2.f, 1.f)};
      float3 blurred = scene * 5.f;
      [unroll]
      for (int i = 0; i < 12; i++) {
        blurred += tex2Dlod(g_HDRSampler, float4(scene_uv + BLUR_TAPS[i].xy * blur_scale, 0.f, 0.f)).rgb * BLUR_TAPS[i].z;
      }
      blurred /= 29.f;

      if (edge >= 0.1f) {
        scene = blurred;
      }
    }
  }

  float3 color = scene;
#ifdef FABLE3_HAS_DOF
  float depth = tex2D(g_DepthSampler, scene_uv).x;
  float inverse_depth = 1.f / (depth - g_PerspectiveConstants.x);
  float view_depth = g_PerspectiveConstants.y * inverse_depth;

#ifdef FABLE3_HAS_DUST
  [branch]
  if (RENODX_EFFECT_DUST != 0.f) {
    float2 centered_uv = scene_uv - 0.5f;
    float4 dust_samples = float4(
        tex2Dlod(g_DustSampler, float4(centered_uv * g_DustPlanes[0].xy + g_DustPlanes[0].zw, 0.f, 0.f)).x,
        tex2Dlod(g_DustSampler, float4(centered_uv * g_DustPlanes[2].xy + g_DustPlanes[2].zw, 0.f, 0.f)).x,
        tex2Dlod(g_DustSampler, float4(centered_uv * g_DustPlanes[4].xy + g_DustPlanes[4].zw, 0.f, 0.f)).x,
        tex2Dlod(g_DustSampler, float4(centered_uv * g_DustPlanes[6].xy + g_DustPlanes[6].zw, 0.f, 0.f)).x);
    float4 dust_weights = saturate((view_depth - (dust_samples * 3.f + g_DustPlanesZ)) * 0.15f) * dust_samples;
    float dust_alpha = dot(dust_weights, g_DustPlanesAlpha) * 0.25f;
    float3 dust_color = (g_DustPlanes[1] + g_DustPlanes[3] + g_DustPlanes[5] + g_DustPlanes[7]).xyz;
    scene = lerp(scene, dust_color * 0.25f, dust_alpha);
  }
#endif

  color = scene;  // after the dust blend
  float3 dof_blur = tex2D(g_DepthOfFieldBlurSampler, scene_uv).rgb;
  [branch]
  if (RENODX_EFFECT_DEPTH_OF_FIELD != 0.f) {
    [branch]
    if (g_UseOldDoFCalculation) {
      float near_distance = g_DepthOfFieldPlanes.y - view_depth;
      float far_distance = g_DepthOfFieldPlanes.z - view_depth;
      float near_blur = min(g_DepthOfFieldUnitMaxBlurNearFar.x, max(near_distance * g_DepthOfFieldPlanes.x, 0.f));
      float far_blur = min(g_DepthOfFieldUnitMaxBlurNearFar.y, max(-far_distance * g_DepthOfFieldPlanes.w, 0.f));
      float blur_amount = (far_distance >= 0.f) ? 0.f : far_blur;
      blur_amount = (near_distance <= 0.f) ? blur_amount : near_blur;
      color = lerp(scene, dof_blur * g_AmbientNormalMapDarkeningColour.w, blur_amount);
    } else {
      color = lerp(scene, dof_blur, tex2Dlod(g_DepthOfFieldFactorSampler, float4(scene_uv, 0.f, 0.f)).x);
    }
  }
#endif

  float3 bloom = 0.f;
  [branch]
  if (RENODX_EFFECT_BLOOM != 0.f) {
    bloom = saturate(tex2Dlod(g_BloomSampler, float4(scene_uv, 0.f, 0.f)).rgb * g_BloomFactor.x);
  }

  float luma = dot(LUMA_WEIGHTS, color);
  float3 desaturated = lerp(luma, color, saturate(luma * g_ColourSensitivityThreshold.x));
  float curve_gain = tex2D(g_ToneMapSampler, (luma * g_RecipMaxLuminance.x).xx).x;
  float3 tone_mapped = saturate(curve_gain * desaturated * g_SaturationBrightnessBaseAndOffset.y);

  float3 blended = bloom + tone_mapped - bloom * tone_mapped;
  blended += g_SaturationBrightnessBaseAndOffset.x * (blended - dot(LUMA_WEIGHTS, blended));

  // Vanilla output: gamma encode into the 8-bit back buffer (implicit clamp). RenderIntermediatePass encodes with 2.2 again,
  // so decode with 2.2 here: the round trip reproduces the vanilla encoded value exactly, whatever g_GlobalGammaAdjustment is.
  float3 vanilla_encoded = saturate(pow(max(blended, 0.f), g_GlobalGammaAdjustment.x));
  float3 vanilla_linear = renodx::color::gamma::DecodeSafe(vanilla_encoded, 2.2f);

  [branch]
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    return renodx::draw::RenderIntermediatePass(float4(vanilla_linear, 1.f));
  }

  // Vanilla+: the vanilla image below the knee, highlights extended toward the peak. No ToneMapPass (no RenoDRT, no grading).
  [branch]
  if (RENODX_TONE_MAP_TYPE == 4.f) {
    float driver = max(color.r, max(color.g, color.b));
#ifdef FABLE3_HAS_DOF
    float near_weight = smoothstep(NEAR_PROTECT_START, NEAR_PROTECT_END, view_depth);
#else
    float near_weight = 0.f;
#endif
    return renodx::draw::RenderIntermediatePass(float4(ExpandVanillaHighlights(vanilla_linear, driver, near_weight), 1.f));
  }

  // HDR bridge. Signals (see fable3/README.md):
  //   untonemapped = scene colour with the game's adaptive exposure applied, before the tone curve and before the per-channel clip.
  //                  The curve texture stores a gain (mapped luma / luma) that depends on luma; its first texel is the gain at
  //                  luma ~0, i.e. the exposure part of the curve without the highlight compression.
  //   graded_sdr   = the complete vanilla result (curve, per-channel clip, bloom, saturation, gamma) decoded back to linear.
  // neutral_sdr is left to the two-argument ToneMapPass (RenoDRT neutral SDR): the ratio graded_sdr / neutral_sdr carries the
  // vanilla look onto the unclipped scene, then ToneMapPass maps it to the user's peak / diffuse white.
  float exposure_gain = tex2Dlod(g_ToneMapSampler, float4(0.f, 0.f, 0.f, 0.f)).x * g_SaturationBrightnessBaseAndOffset.y;
  float3 untonemapped = color * exposure_gain;

  float3 hdr_color = renodx::draw::ToneMapPass(untonemapped, vanilla_linear);
  return renodx::draw::RenderIntermediatePass(float4(hdr_color, 1.f));
}
