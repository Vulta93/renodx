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

  return renodx::draw::RenderIntermediatePass(float4(vanilla_linear, 1.f));
}
