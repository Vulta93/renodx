#version 150
// RENODX: floatBitsToUint (GLSL 3.30) is used by the PsychoV port below (psycho17_CopySign).
#extension GL_ARB_shader_bit_encoding : require
// RENODX: explicit uniform-block member offsets (GLSL 4.40 core; NVIDIA exposes it on this 4.3 context).
#extension GL_ARB_enhanced_layouts : require

void clip( float v ) { if ( v < 0.0 ) { discard; } }
void clip( vec2 v ) { if ( any( lessThan( v, vec2( 0.0 ) ) ) ) { discard; } }
void clip( vec3 v ) { if ( any( lessThan( v, vec3( 0.0 ) ) ) ) { discard; } }
void clip( vec4 v ) { if ( any( lessThan( v, vec4( 0.0 ) ) ) ) { discard; } }

float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }
vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }
vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }
vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }

vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture( sampler, texcoord.xy ); }
vec4 tex2D( sampler2DShadow sampler, vec3 texcoord ) { return vec4( texture( sampler, texcoord.xyz ) ); }

vec4 tex2D( sampler2D sampler, vec2 texcoord, vec2 dx, vec2 dy ) { return textureGrad( sampler, texcoord.xy, dx, dy ); }
vec4 tex2D( sampler2DShadow sampler, vec3 texcoord, vec2 dx, vec2 dy ) { return vec4( textureGrad( sampler, texcoord.xyz, dx, dy ) ); }

vec4 texCUBE( samplerCube sampler, vec3 texcoord ) { return texture( sampler, texcoord.xyz ); }
vec4 texCUBE( samplerCubeShadow sampler, vec4 texcoord ) { return vec4( texture( sampler, texcoord.xyzw ) ); }

vec4 tex1Dproj( sampler1D sampler, vec2 texcoord ) { return textureProj( sampler, texcoord ); }
vec4 tex2Dproj( sampler2D sampler, vec3 texcoord ) { return textureProj( sampler, texcoord ); }
vec4 tex3Dproj( sampler3D sampler, vec4 texcoord ) { return textureProj( sampler, texcoord ); }

vec4 tex1Dbias( sampler1D sampler, vec4 texcoord ) { return texture( sampler, texcoord.x, texcoord.w ); }
vec4 tex2Dbias( sampler2D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xy, texcoord.w ); }
vec4 tex3Dbias( sampler3D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }
vec4 texCUBEbias( samplerCube sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }

vec4 tex1Dlod( sampler1D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.x, texcoord.w ); }
vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }
vec4 tex3Dlod( sampler3D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }
vec4 texCUBElod( samplerCube sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }

float dot3 ( vec3 a, vec3 b ) { return dot( a, b ); }
float dot3 ( vec3 a, vec4 b ) { return dot( a, b.xyz ); }
float dot3 ( vec4 a, vec3 b ) { return dot( a.xyz, b ); }
float dot3 ( vec4 a, vec4 b ) { return dot( a.xyz, b.xyz ); }
vec4 h4tex2D ( sampler2D image, vec2 texcoord ) { return tex2D( image, texcoord ); }
float tex2Ddepth_fast ( sampler2D image, vec2 texcoord ) { return tex2D( image, texcoord ).x; }
vec2 screenPosToTexcoord ( vec2 pos, vec4 bias_scale ) { return ( pos * bias_scale.zw + bias_scale.xy ); }
uniform vec4 _fa_ [25];
uniform sampler2D samp_glaremap;
uniform sampler2D samp_viewdepthmap;
uniform sampler2D samp_postdistortionmap;
uniform sampler2D samp_viewcolormap;
uniform sampler2D samp_cbconversionlut;
uniform sampler2D samp_screenoverlay;
uniform sampler2D samp_grainmap;
uniform sampler2D samp_dynamiccc;

// RENODX: settings. RenoDX pushes ShaderInjectData (shared.h) through ReShade's OpenGL push-constant path, which uploads
// it as a UBO at binding 0 (the layout param's binding); an unbound uniform block also defaults to binding 0. Only the
// fields used here are declared, each at its byte offset in ShaderInjectData; addon.cpp static_asserts the same offsets,
// so a change to shared.h that would move them fails the build instead of silently feeding wrong values.
layout( std140 ) uniform RenoDXShaderInjection {
	layout( offset = 0 ) float peak_white_nits;
	layout( offset = 4 ) float diffuse_white_nits;
	layout( offset = 8 ) float graphics_white_nits;
	layout( offset = 16 ) float tone_map_type;
	layout( offset = 20 ) float tone_map_exposure;
	layout( offset = 24 ) float tone_map_highlights;
	layout( offset = 28 ) float tone_map_shadows;
	layout( offset = 32 ) float tone_map_contrast;
	layout( offset = 36 ) float tone_map_saturation;
	layout( offset = 44 ) float tone_map_blowout;
	layout( offset = 52 ) float tone_map_hue_correction;
	layout( offset = 88 ) float intermediate_encoding;
} shader_injection;

#define RENODX_TONE_MAP_TYPE         shader_injection.tone_map_type
#define RENODX_PEAK_WHITE_NITS       shader_injection.peak_white_nits
#define RENODX_DIFFUSE_WHITE_NITS    shader_injection.diffuse_white_nits
#define RENODX_GRAPHICS_WHITE_NITS   shader_injection.graphics_white_nits
#define RENODX_INTERMEDIATE_ENCODING shader_injection.intermediate_encoding
#define RENODX_TONE_MAP_EXPOSURE     shader_injection.tone_map_exposure
#define RENODX_TONE_MAP_HIGHLIGHTS   shader_injection.tone_map_highlights
#define RENODX_TONE_MAP_SHADOWS      shader_injection.tone_map_shadows
#define RENODX_TONE_MAP_CONTRAST     shader_injection.tone_map_contrast
#define RENODX_TONE_MAP_SATURATION   shader_injection.tone_map_saturation
#define RENODX_TONE_MAP_BLOWOUT      shader_injection.tone_map_blowout
#define RENODX_TONE_MAP_HUE_CORRECTION shader_injection.tone_map_hue_correction

// Tone Mapper values (addon.cpp ToneMapType): 0 Vanilla, 1 None, 2 PsychoV (here).
#define TONE_MAP_TYPE_PSYCHOV 2.0

// renodx::draw::DecodeColor / EncodeColor for the encodings the intermediate can use (draw.hlsl; sign-preserving
// DecodeSafe / EncodeSafe). 0 none, 1 sRGB, 2 gamma 2.2, 3 gamma 2.4.
vec3 renodx_srgb_decode( vec3 c ) { return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) ); }
vec3 renodx_srgb_encode( vec3 c ) { return mix( c * 12.92, 1.055 * pow( c, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), c ) ); }
vec3 renodx_decode( vec3 c, float encoding ) {
	if ( encoding == 1.0 ) return sign( c ) * renodx_srgb_decode( abs( c ) );
	if ( encoding == 2.0 ) return sign( c ) * pow( abs( c ), vec3( 2.2 ) );
	if ( encoding == 3.0 ) return sign( c ) * pow( abs( c ), vec3( 2.4 ) );
	return c;
}
vec3 renodx_encode( vec3 c, float encoding ) {
	if ( encoding == 1.0 ) return sign( c ) * renodx_srgb_encode( abs( c ) );
	if ( encoding == 2.0 ) return sign( c ) * pow( abs( c ), vec3( 1.0 / 2.2 ) );
	if ( encoding == 3.0 ) return sign( c ) * pow( abs( c ), vec3( 1.0 / 2.4 ) );
	return c;
}

// RENODX: PsychoV 17, verbatim GLSL port from src/games/doom-tda/include/psychov_17.glsl (same port as
// src/games/indygreatcircle/include/psychov_17.glsl; HLSL original renodx::tonemap::psychov::psychotm_test17).
// Inlined because plain .glsl replacements are embedded as text without #include support. Include guard removed.
// ---- begin psychov_17.glsl ----
/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */


// How to use:
//   - Include this file directly; it has no dependencies.
//   - Use psycho17_SDR(color, cone_response) for SDR output with BT.709 gamut compression.
//   - Use psycho17_HDR(color, peak, cone_response) for HDR output with BT.2020 gamut compression.
//   - For full control, call psychov_17(...) directly.
//
// GLSL port note: matrix literals are transposed relative to the HLSL source
// because GLSL `mat3(...)` constructors take columns, while HLSL
// `float3x3(...)` takes rows. All `mul(A, B)` map to `A * B` and all
// `mul(M, v)` map to `M * v`. File-scope const matrices that the HLSL built
// via `mul()` or `psycho17_Invert3x3` are reproduced here with GLSL's `*`
// and pre-expanded constant matrices for GLSL const-expression compatibility.

float psycho17_Select(bool condition, float true_value, float false_value) {
  return condition ? true_value : false_value;
}

float psycho17_DivideSafe(float dividend, float divisor, float fallback) {
  return psycho17_Select(divisor == 0.0, fallback, dividend / divisor);
}

vec3 psycho17_DivideSafe(vec3 dividend, vec3 divisor, vec3 fallback) {
  return vec3(
      psycho17_DivideSafe(dividend.x, divisor.x, fallback.x),
      psycho17_DivideSafe(dividend.y, divisor.y, fallback.y),
      psycho17_DivideSafe(dividend.z, divisor.z, fallback.z));
}

const float psycho17_EPSILON = 1e-20;
const float psycho17_MB_NEAR_WHITE_EPSILON = 1e-14;
const float psycho17_INTERVAL_MAX = 3.402823466e+38;
const float psycho17_CIE1702_RAY_T_MAX = 1e20;
const uint psycho17_FLT32_SIGN = 0x80000000u;
const uint psycho17_FLT32_MAGNITUDE = 0x7FFFFFFFu;

// Scalar order below lists HLSL matrix columns because GLSL mat3 constructors take columns.
const mat3 psycho17_STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT = mat3(
    1.94735469, 0.68990272, 0.00000000,
    -1.41445123, 0.34832189, 0.00000000,
    0.36476327, 0.00000000, 1.93485343);
const vec3 psycho17_LMS_WEIGHTS = vec3(
    psycho17_STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT[0][1],
    psycho17_STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT[1][1],
    0.0371597069161);
const mat3 psycho17_BT709_TO_XYZ_MAT = mat3(
    0.4123907993, 0.2126390059, 0.0193308187,
    0.3575843394, 0.7151686788, 0.1191947798,
    0.1804807884, 0.0721923154, 0.9505321522);
const mat3 psycho17_XYZ_TO_BT709_MAT = mat3(
    3.2409699419, -0.9692436363, 0.0556300797,
    -1.5373831776, 1.8759675015, -0.2039769589,
    -0.4986107603, 0.0415550574, 1.0569715142);
const mat3 psycho17_BT2020_TO_XYZ_MAT = mat3(
    0.6369580483, 0.2627002120, 0.0000000000,
    0.1446169036, 0.6779980715, 0.0280726930,
    0.1688809752, 0.0593017165, 1.0609850577);
const mat3 psycho17_AP1_TO_XYZ_MAT = mat3(
    0.6624541811, 0.2722287168, -0.0055746495,
    0.1340042065, 0.6740817658, 0.0040607335,
    0.1561876870, 0.0536895174, 1.0103391003);
  const mat3 psycho17_XYZ_TO_BT2020_MAT = mat3(
    1.7166511880, -0.6666843518, 0.0176398574,
    -0.3556707838, 1.6164812366, -0.0427706133,
    -0.2533662814, 0.0157685458, 0.9421031212);
const mat3 psycho17_XYZ_TO_LMS_MAT = mat3(
    0.2670502842655792, -0.38706882411220156, 0.026727793989083093,
    0.8471990148492798, 1.165429935890458, -0.02729131667566509,
    -0.03470416612462053, 0.10302286696614202, 0.5333267257603284);
const mat3 psycho17_LMS_TO_XYZ_MAT = mat3(
    1.811462963687362, 0.6069124057314326, -0.05972505917019778,
    -1.3081492612535466, 0.41590625928300007, 0.08684090099781154,
    0.3705694640609474, -0.04084825533137022, 1.854361773496562);
  const mat3 psycho17_XYZ_TO_LMS_2006_MAT = mat3(
    0.185082982238733, -0.134433056469973, 0.000789456671966863,
    0.584081279463687, 0.405752392775348, -0.000912281325916184,
    -0.0240722415044404, 0.0358252602217631, 0.0198490812339463);
  const mat3 psycho17_LMS_TO_XYZ_2006_MAT = mat3(
    2.628474773947687, 0.8765342837340053, -0.06425592569153737,
    -3.761263499279893, 1.200303808551587, 0.2047635992112653,
    9.976357133974499, -1.103378592864622, 49.93266413348866);
const mat3 psycho17_LMS_TO_LMS_WEIGHTED_MAT = mat3(
    psycho17_LMS_WEIGHTS.x, 0.0, 0.0,
    0.0, psycho17_LMS_WEIGHTS.y, 0.0,
    0.0, 0.0, psycho17_LMS_WEIGHTS.z);
const mat3 psycho17_XYZ_TO_LMS_WEIGHTED_MAT =
    psycho17_LMS_TO_LMS_WEIGHTED_MAT * psycho17_XYZ_TO_LMS_MAT;
const mat3 psycho17_BT709_TO_LMS_WEIGHTED_MAT =
    psycho17_XYZ_TO_LMS_WEIGHTED_MAT * psycho17_BT709_TO_XYZ_MAT;
const mat3 psycho17_BT2020_TO_LMS_WEIGHTED_MAT =
    psycho17_XYZ_TO_LMS_WEIGHTED_MAT * psycho17_BT2020_TO_XYZ_MAT;

float psycho17_CopySign(float mag, float sgn) {
  uint sign_bits = floatBitsToUint(sgn) & psycho17_FLT32_SIGN;
  uint mag_bits = floatBitsToUint(mag) & psycho17_FLT32_MAGNITUDE;
  return uintBitsToFloat(sign_bits | mag_bits);
}

vec3 psycho17_CopySign(vec3 mag, vec3 sgn) {
  uvec3 sign_bits = floatBitsToUint(sgn) & psycho17_FLT32_SIGN;
  uvec3 mag_bits = floatBitsToUint(mag) & psycho17_FLT32_MAGNITUDE;
  return uintBitsToFloat(sign_bits | mag_bits);
}

float psycho17_SignPow(float x, float exponent) {
  return psycho17_CopySign(pow(abs(x), exponent), x);
}

float psycho17_ContrastSafe(float x, float contrast, float mid_gray) {
  return psycho17_SignPow(x / mid_gray, contrast) * mid_gray;
}

float psycho17_Highlights(float x, float highlights, float mid_gray) {
  if (highlights > 1.0) {
    return max(x, mix(x, mid_gray * pow(x / mid_gray, highlights), x));
  } else if (highlights < 1.0) {
    return min(x, x / (1.0 + mid_gray * pow(x / mid_gray, 2.0 - highlights) - x));
  }
  return x;
}

float psycho17_Shadows(float x, float shadows, float mid_gray) {
  if (shadows > 1.0) {
    return max(x, x * (1.0 + (x * mid_gray / pow(x / mid_gray, shadows))));
  } else if (shadows < 1.0) {
    return clamp(x * (1.0 - (x * mid_gray / pow(x / mid_gray, 2.0 - shadows))), 0.0, x);
  }
  return x;
}

vec3 psycho17_BT709ToLMS(vec3 bt709) {
  return psycho17_XYZ_TO_LMS_MAT * (psycho17_BT709_TO_XYZ_MAT * bt709);
}

vec3 psycho17_BT2020ToLMS(vec3 bt2020) {
  return psycho17_XYZ_TO_LMS_MAT * (psycho17_BT2020_TO_XYZ_MAT * bt2020);
}

vec3 psycho17_AP1ToLMS(vec3 ap1) {
  return psycho17_XYZ_TO_LMS_MAT * (psycho17_AP1_TO_XYZ_MAT * ap1);
}

vec3 psycho17_XYZFromxyY(vec3 xyY) {
  vec3 xyz;
  xyz.xz = vec2(xyY.x, 1.0 - xyY.x - xyY.y) / xyY.y * xyY.z;
  xyz.y = xyY.z;
  return xyz;
}

vec3 psycho17_LMSToBT709(vec3 lms) {
  return psycho17_XYZ_TO_BT709_MAT * (psycho17_LMS_TO_XYZ_MAT * lms);
}

vec3 psycho17_LMSToBT2020(vec3 lms) {
  return psycho17_XYZ_TO_BT2020_MAT * (psycho17_LMS_TO_XYZ_MAT * lms);
}

float psycho17_YFFromLMS(vec3 lms) {
  return (psycho17_STOCKMAN_SHARP_LMS_TO_XFYFZF_MAT * lms).y;
}

float psycho17_YFFromBT709(vec3 bt709) {
  return psycho17_YFFromLMS(psycho17_BT709ToLMS(bt709));
}

float psycho17_YFFromAP1(vec3 ap1) {
  return psycho17_YFFromLMS(psycho17_AP1ToLMS(ap1));
}

vec3 psycho17_WeighLMS(vec3 lms) {
  return lms * psycho17_LMS_WEIGHTS;
}

vec3 psycho17_UnweighLMS(vec3 lms_weighted) {
  return lms_weighted / psycho17_LMS_WEIGHTS;
}

vec3 psycho17_WeightedLMSFromMacleodBoynton(vec2 mb, float l_plus_m) {
  return vec3(mb.x, 1.0 - mb.x, mb.y) * l_plus_m;
}

vec3 psycho17_WeightedLMSFromMacleodBoynton(vec3 mb) {
  return psycho17_WeightedLMSFromMacleodBoynton(mb.xy, mb.z);
}

vec3 psycho17_MacleodBoyntonFromWeightedLMS(vec3 weighted_lms) {
  float y_mb = max(weighted_lms.x + weighted_lms.y, 0.0);
  float inv = psycho17_DivideSafe(1.0, y_mb, 0.0);
  return vec3(weighted_lms.x * inv, weighted_lms.z * inv, y_mb);
}

vec3 psycho17_MacleodBoyntonFromLMS(vec3 lms) {
  return psycho17_MacleodBoyntonFromWeightedLMS(psycho17_WeighLMS(lms));
}

vec3 psycho17_LMSFromMacleodBoynton(vec3 mb) {
  return psycho17_WeightedLMSFromMacleodBoynton(mb) / psycho17_LMS_WEIGHTS;
}

vec2 psycho17_D65MacleodBoyntonXY() {
  return psycho17_MacleodBoyntonFromWeightedLMS(
             psycho17_LMS_TO_LMS_WEIGHTED_MAT
             * (psycho17_XYZ_TO_LMS_MAT
                * psycho17_XYZFromxyY(vec3(vec2(0.31272, 0.32903), 1.0))))
      .xy;
}

float psycho17_Cross2(vec2 a, vec2 b) {
  return a.x * b.y - a.y * b.x;
}

bool psycho17_RaySegmentHit2D(vec2 origin, vec2 direction, vec2 a, vec2 b, out float t_hit) {
  t_hit = 0.0;
  vec2 edge = b - a;
  float denom = psycho17_Cross2(direction, edge);
  if (abs(denom) <= psycho17_EPSILON) return false;

  vec2 a_origin = a - origin;
  float t = psycho17_Cross2(a_origin, edge) / denom;
  float u = psycho17_Cross2(a_origin, direction) / denom;
  if (t < 0.0 || u < 0.0 || u > 1.0) return false;

  t_hit = t;
  return true;
}

float psycho17_RayMaxT_RGBTriangleInMB(
    vec2 origin,
    vec2 direction,
    vec2 r,
    vec2 g,
    vec2 b,
    out bool has_solution) {
  has_solution = false;
  if (dot(direction, direction) <= psycho17_MB_NEAR_WHITE_EPSILON) return 0.0;

  float t_best = psycho17_INTERVAL_MAX;
  float t_hit;
  bool hit_any = false;

  if (psycho17_RaySegmentHit2D(origin, direction, r, g, t_hit)) {
    t_best = min(t_best, t_hit);
    hit_any = true;
  }
  if (psycho17_RaySegmentHit2D(origin, direction, g, b, t_hit)) {
    t_best = min(t_best, t_hit);
    hit_any = true;
  }
  if (psycho17_RaySegmentHit2D(origin, direction, b, r, t_hit)) {
    t_best = min(t_best, t_hit);
    hit_any = true;
  }

  has_solution = hit_any;
  return hit_any ? max(t_best, 0.0) : 0.0;
}

float psycho17_NeutwoPeakClip(float x, float peak, float clip) {
  float peak_safe = max(peak, 0.0);
  float clip_safe = max(clip, peak_safe);
  float x2 = x * x;
  float clip2 = clip_safe * clip_safe;
  float peak2 = peak_safe * peak_safe;
  // RENODX (wolfensteintno): fma() is GLSL 4.00; same expression without the fused rounding for GLSL 1.50.
  float denominator_squared = x2 * (clip2 - peak2) + clip2 * peak2;
  return (clip_safe * peak_safe * x) * inversesqrt(max(denominator_squared, 1e-20));
}

float psycho17_NeutwoScaleFromRayT(float t_peak, float t_clip) {
  float t_peak_safe = max(t_peak, 0.0);
  float t_clip_safe = max(t_clip, t_peak_safe);
  return clamp(psycho17_NeutwoPeakClip(1.0, t_peak_safe, t_clip_safe), 0.0, 1.0);
}

float psycho17_SoftCompressionActivationFromRayT(float t_peak, float knee) {
  float outside = 1.0 - clamp(t_peak, 0.0, 1.0);
  return psycho17_DivideSafe(outside, outside + max(knee, 1e-6), 0.0);
}

float psycho17_SoftCompressionActivationFromRayT(float t_peak) {
  return psycho17_SoftCompressionActivationFromRayT(t_peak, 0.08);
}

vec3 psycho17_NakaRushton(
    vec3 x,
    vec3 peak,
    vec3 anchor_in,
    vec3 anchor_out,
    float cone_response_exponent) {
  vec3 peak_minus_anchor_out = peak - anchor_out;
  vec3 n = cone_response_exponent * peak / peak_minus_anchor_out;
  vec3 contrasted = anchor_out * pow(abs(x) / anchor_in, n);
  vec3 saturated = peak * contrasted / (contrasted + peak_minus_anchor_out);
  return psycho17_CopySign(saturated, x);
}

float psycho17_RayExitTCIE1702(vec2 origin, vec2 direction) {
  if (dot(direction, direction) <= psycho17_MB_NEAR_WHITE_EPSILON) {
    return psycho17_CIE1702_RAY_T_MAX;
  }

  const int kEdgeCount = 7;
  const vec2 kHalfspaceNormals[7] = vec2[](
      vec2(-0.043889, -0.006807),
      vec2(-0.007821, -0.008564),
      vec2(-0.000604, -0.007942),
      vec2(0.0, -0.080835),
      vec2(0.953597, 0.307020),
      vec2(-0.060969, 0.019752),
      vec2(-0.106895, 0.004035));
  const float kHalfspaceNumerators[7] = float[](
      0.0065035249,
      0.00104900495,
      0.000207697044,
      0.00165556648,
      0.252472349,
      0.0241967351,
      0.0199621232);

  vec2 white_to_origin = psycho17_D65MacleodBoyntonXY() - origin;
  float t_best = psycho17_CIE1702_RAY_T_MAX;
  bool hit_any = false;

  for (int i = 0; i < kEdgeCount; ++i) {
    vec2 halfspace_normal = kHalfspaceNormals[i];
    float denom = dot(halfspace_normal, direction);
    float numerator = kHalfspaceNumerators[i] + dot(halfspace_normal, white_to_origin);
    float t = denom > 1e-8 ? numerator * (1.0 / denom) : psycho17_CIE1702_RAY_T_MAX;
    t_best = min(t_best, t);
    hit_any = hit_any || (denom > 1e-8);
  }

  return hit_any ? max(t_best, 0.0) : psycho17_CIE1702_RAY_T_MAX;
}

float psycho17_HueRelativePuritySignalFromTClip(float t_clip) {
  return clamp(psycho17_DivideSafe(1.0, t_clip, 0.0), 0.0, 1.0);
}

float psycho17_AdaptiveHueSensitivityFromTClip(float t_clip) {
  const float kMeanD65RayDistance = 0.20139844;
  const float kMaxD65RayDistance = 1.02634534;
  const float kMinSensitivity = 0.35;

  float long_ray_weight = clamp(psycho17_DivideSafe(
                                    t_clip - kMeanD65RayDistance,
                                    kMaxD65RayDistance - kMeanD65RayDistance,
                                    0.0),
                                0.0, 1.0);
  return mix(1.0, kMinSensitivity, long_ray_weight);
}

float psycho17_HueRelativePuritySignalFromMB(vec2 mb_xy, vec2 mb_anchor) {
  vec2 direction = mb_xy - mb_anchor;
  if (dot(direction, direction) <= psycho17_MB_NEAR_WHITE_EPSILON) {
    return 0.0;
  }

  return psycho17_HueRelativePuritySignalFromTClip(
      psycho17_RayExitTCIE1702(mb_anchor, direction));
}

float psycho17_HueRelativePuritySignalFromMB(vec3 mb, vec2 mb_anchor) {
  if (!(mb.z > psycho17_EPSILON)) {
    return 0.0;
  }

  return psycho17_HueRelativePuritySignalFromMB(mb.xy, mb_anchor);
}

float psycho17_HueRelativePuritySignal(vec3 lms_input, vec2 mb_anchor) {
  vec3 lms_weighted = psycho17_WeighLMS(max(lms_input, 0.0));
  vec3 mb = psycho17_MacleodBoyntonFromWeightedLMS(lms_weighted);
  if (!(mb.z > psycho17_EPSILON)) {
    return 0.0;
  }

  return psycho17_HueRelativePuritySignalFromMB(mb, mb_anchor);
}

float psycho17_D65HueSensitivity(vec2 mb_xy) {
  vec2 mb_white = psycho17_D65MacleodBoyntonXY();
  vec2 direction = mb_xy - mb_white;
  if (dot(direction, direction) <= psycho17_MB_NEAR_WHITE_EPSILON) {
    return 1.0;
  }

  return psycho17_AdaptiveHueSensitivityFromTClip(
      psycho17_RayExitTCIE1702(mb_white, direction));
}

vec3 psycho17_ToAdaptiveRelativeLMS(vec3 lms_input, vec3 current_adaptive_state_lms) {
  return psycho17_DivideSafe(lms_input, current_adaptive_state_lms, vec3(0.0));
}

vec3 psycho17_FromAdaptiveRelativeLMS(vec3 lms_relative, vec3 current_adaptive_state_lms) {
  return lms_relative * max(current_adaptive_state_lms, vec3(1e-6));
}

vec3 psycho17_ToAdaptiveRelativeWeightedLMS(vec3 lms_input, vec3 current_adaptive_state_lms) {
  return psycho17_DivideSafe(
      psycho17_WeighLMS(lms_input),
      current_adaptive_state_lms,
      vec3(0.0));
}

vec3 psycho17_FromAdaptiveRelativeWeightedLMS(
    vec3 lms_weighted_relative,
    vec3 current_adaptive_state_lms) {
  return lms_weighted_relative * max(current_adaptive_state_lms, vec3(1e-6));
}

vec2 psycho17_MBFromWeightedPrimary(vec3 weighted_lms_primary) {
  return psycho17_MacleodBoyntonFromWeightedLMS(weighted_lms_primary).xy;
}

// In GLSL the matrix is column-major, so `M[primary_index]` directly yields
// the column the HLSL extracted element-by-element.
vec3 psycho17_RGBToWeightedLMSPrimaryColumn(mat3 rgb_to_lms_weighted_mat, uint primary_index) {
  return rgb_to_lms_weighted_mat[primary_index];
}

vec3 psycho17_RGBToAdaptiveWeightedLMSPrimaryColumn(
    mat3 rgb_to_lms_weighted_mat,
    uint primary_index,
    vec3 current_adaptive_state_lms) {
  return psycho17_RGBToWeightedLMSPrimaryColumn(rgb_to_lms_weighted_mat, primary_index) / current_adaptive_state_lms;
}

void psycho17_MakeRGBTriangleInMBFromWeightedPrimaries(
    vec3 weighted_r,
    vec3 weighted_g,
    vec3 weighted_b,
    out vec2 r,
    out vec2 g,
    out vec2 b) {
  r = psycho17_MBFromWeightedPrimary(weighted_r);
  g = psycho17_MBFromWeightedPrimary(weighted_g);
  b = psycho17_MBFromWeightedPrimary(weighted_b);
}

void psycho17_MakeRGBTriangleInMBAdaptiveWeighted(
    mat3 rgb_to_lms_weighted_mat,
    vec3 current_adaptive_state_lms,
    out vec2 r,
    out vec2 g,
    out vec2 b) {
  psycho17_MakeRGBTriangleInMBFromWeightedPrimaries(
      psycho17_RGBToAdaptiveWeightedLMSPrimaryColumn(
          rgb_to_lms_weighted_mat, 0u, current_adaptive_state_lms),
      psycho17_RGBToAdaptiveWeightedLMSPrimaryColumn(
          rgb_to_lms_weighted_mat, 1u, current_adaptive_state_lms),
      psycho17_RGBToAdaptiveWeightedLMSPrimaryColumn(
          rgb_to_lms_weighted_mat, 2u, current_adaptive_state_lms),
      r,
      g,
      b);
}

vec3 psycho17_ClampWeightedLMSToCIE1702(vec3 lms_weighted_input, float purity_max) {
  vec3 lms_weighted_clamped = max(lms_weighted_input, 0.0);
  vec3 mb = psycho17_MacleodBoyntonFromWeightedLMS(lms_weighted_clamped);
  float y_mb = mb.z;
  vec2 ls = mb.xy;
  if (!(y_mb > psycho17_EPSILON)) {
    return vec3(lms_weighted_clamped.x, lms_weighted_clamped.y, 0.0);
  }

  vec2 mb_white = psycho17_D65MacleodBoyntonXY();
  vec2 direction = ls - mb_white;
  if (dot(direction, direction) <= psycho17_MB_NEAR_WHITE_EPSILON) {
    return lms_weighted_clamped;
  }

  float t_clip = psycho17_RayExitTCIE1702(mb_white, direction);
  float t_final = min(1.0, max(purity_max, 0.0) * t_clip);
  vec2 ls_out = mb_white + direction * t_final;
  return psycho17_WeightedLMSFromMacleodBoynton(ls_out, y_mb);
}

vec3 psycho17_ClampWeightedLMSToCIE1702(vec3 lms_weighted_input) {
  return psycho17_ClampWeightedLMSToCIE1702(lms_weighted_input, 1.0);
}

vec3 psycho17_GamutCompressWeightedLMSCoreRGBBoundFromWeightedInput(
    vec3 lms_weighted_input,
    vec2 bound_r,
    vec2 bound_g,
    vec2 bound_b,
    float strength) {
  vec3 lms_weighted_clamped = psycho17_ClampWeightedLMSToCIE1702(max(lms_weighted_input, 0.0));
  vec3 mb = psycho17_MacleodBoyntonFromWeightedLMS(lms_weighted_clamped);
  float y_mb = mb.z;
  vec2 ls = mb.xy;
  if (!(y_mb > psycho17_EPSILON)) {
    return vec3(lms_weighted_clamped.x, lms_weighted_clamped.y, 0.0);
  }

  vec2 mb_white = psycho17_D65MacleodBoyntonXY();
  vec2 direction = ls - mb_white;
  if (dot(direction, direction) <= psycho17_MB_NEAR_WHITE_EPSILON) {
    return lms_weighted_clamped;
  }

  bool has_peak = false;
  float t_peak = psycho17_RayMaxT_RGBTriangleInMB(
      mb_white, direction, bound_r, bound_g, bound_b, has_peak);
  float t_clip = psycho17_RayExitTCIE1702(mb_white, direction);
  if (!has_peak) {
    t_peak = t_clip;
  }

  float t_hard = clamp(t_peak, 0.0, 1.0);
  float t_soft = psycho17_NeutwoScaleFromRayT(min(t_peak, t_clip), t_clip);
  float soft_mix = clamp(strength, 0.0, 1.0) * psycho17_SoftCompressionActivationFromRayT(t_peak);
  float t_final = mix(t_hard, t_soft, soft_mix);

  vec2 ls_out = mb_white + t_final * direction;
  return psycho17_WeightedLMSFromMacleodBoynton(ls_out, y_mb);
}

vec3 psycho17_GamutCompressWeightedLMSCoreRGBBoundFromAdaptiveWeightedInput(
    vec3 lms_weighted_input,
    vec3 current_adaptive_state_lms,
    mat3 bound_rgb_to_lms_weighted_mat,
    float strength) {
  vec2 bound_r;
  vec2 bound_g;
  vec2 bound_b;
  psycho17_MakeRGBTriangleInMBAdaptiveWeighted(
      bound_rgb_to_lms_weighted_mat,
      current_adaptive_state_lms,
      bound_r,
      bound_g,
      bound_b);
  return psycho17_GamutCompressWeightedLMSCoreRGBBoundFromWeightedInput(
      lms_weighted_input,
      bound_r,
      bound_g,
      bound_b,
      strength);
}

vec3 psycho17_GamutCompressLMSBoundAdaptive(
    vec3 lms_input,
    vec3 current_adaptive_state_lms,
    mat3 bound_rgb_to_lms_weighted_mat,
    float strength) {
  vec3 lms_weighted_relative =
      psycho17_ToAdaptiveRelativeWeightedLMS(lms_input, current_adaptive_state_lms);
  vec3 lms_weighted_relative_out =
      psycho17_GamutCompressWeightedLMSCoreRGBBoundFromAdaptiveWeightedInput(
          lms_weighted_relative,
          current_adaptive_state_lms,
          bound_rgb_to_lms_weighted_mat,
          strength);
  return psycho17_UnweighLMS(
      psycho17_FromAdaptiveRelativeWeightedLMS(
          lms_weighted_relative_out,
          current_adaptive_state_lms));
}

vec3 psycho17_GamutCompressAdaptiveRelativeWeightedLMSBound(
    vec3 lms_weighted_relative_input,
    vec3 current_adaptive_state_lms,
    mat3 bound_rgb_to_lms_weighted_mat,
    float strength) {
  return psycho17_GamutCompressWeightedLMSCoreRGBBoundFromAdaptiveWeightedInput(
      lms_weighted_relative_input,
      current_adaptive_state_lms,
      bound_rgb_to_lms_weighted_mat,
      strength);
}

float psycho17_AdaptiveHueSensitivity(vec2 mb_xy, vec2 mb_anchor) {
  vec2 direction = mb_xy - mb_anchor;
  if (dot(direction, direction) <= psycho17_MB_NEAR_WHITE_EPSILON) {
    return 1.0;
  }

  return psycho17_AdaptiveHueSensitivityFromTClip(
      psycho17_RayExitTCIE1702(mb_anchor, direction));
}

struct psycho17_MBPurityDebug {
  vec3 rgbOut;
  vec3 rgbT0;
  vec3 rgbEdgeGamut;
  float tMaxGamut;
  float tFinal;
  float purityCur01;
  float purity01_in;
  float purity01_used;
};

vec3 psycho17_BT2020ToLMS2006(vec3 bt2020) {
  return psycho17_XYZ_TO_LMS_2006_MAT * (psycho17_BT2020_TO_XYZ_MAT * bt2020);
}

vec3 psycho17_LMS2006ToBT2020(vec3 lms) {
  return psycho17_XYZ_TO_BT2020_MAT * (psycho17_LMS_TO_XYZ_2006_MAT * lms);
}

vec2 psycho17_MB2006FromLMS(vec3 lms) {
  float t = lms.x + lms.y;
  if (t <= 0.0) {
    return vec2(0.0);
  }

  return vec2(
      psycho17_DivideSafe(lms.x, t, 0.0),
      psycho17_DivideSafe(lms.z, t, 0.0));
}

vec3 psycho17_LMS2006FromMBT(vec2 mb, float t) {
  return vec3(mb.x * t, (1.0 - mb.x) * t, mb.y * t);
}

vec2 psycho17_MB2006WhiteD65() {
  vec3 d65_xyz = psycho17_XYZFromxyY(vec3(vec2(0.31272, 0.32903), 1.0));
  return psycho17_MB2006FromLMS(psycho17_XYZ_TO_LMS_2006_MAT * d65_xyz);
}

void psycho17_IntervalLower0(float a, float b, out float lo, out float hi) {
  if (abs(a) < psycho17_EPSILON) {
    if (b >= 0.0) {
      lo = -psycho17_INTERVAL_MAX;
      hi = psycho17_INTERVAL_MAX;
    } else {
      lo = 1.0;
      hi = 0.0;
    }
    return;
  }

  float t0 = psycho17_DivideSafe(-b, a, 0.0);
  if (a > 0.0) {
    lo = t0;
    hi = psycho17_INTERVAL_MAX;
  } else {
    lo = -psycho17_INTERVAL_MAX;
    hi = t0;
  }
}

psycho17_MBPurityDebug psycho17_ApplyBT2020MBPurity(
    vec3 rgb2020_linear,
    float purity01,
    float curve_gamma,
    vec2 mb_white_override,
    float t_min) {
  psycho17_MBPurityDebug result;
  result.rgbOut = rgb2020_linear;
  result.rgbT0 = rgb2020_linear;
  result.rgbEdgeGamut = rgb2020_linear;
  result.tMaxGamut = 0.0;
  result.tFinal = 0.0;
  result.purityCur01 = 0.0;
  result.purity01_in = 0.0;
  result.purity01_used = 0.0;

  vec3 lms = psycho17_BT2020ToLMS2006(rgb2020_linear);
  float t = lms.x + lms.y;
  if (t <= t_min) {
    return result;
  }

  vec2 white = (mb_white_override.x >= 0.0 && mb_white_override.y >= 0.0)
                   ? mb_white_override
                   : psycho17_MB2006WhiteD65();
  vec2 mb0 = psycho17_MB2006FromLMS(lms);
  vec2 direction = mb0 - white;
  if (dot(direction, direction) < psycho17_MB_NEAR_WHITE_EPSILON) {
    return result;
  }

  vec3 rgb_t0 = psycho17_LMS2006ToBT2020(psycho17_LMS2006FromMBT(white, t));
  result.rgbT0 = rgb_t0;

  vec3 a = rgb2020_linear - rgb_t0;
  float t_lo = 0.0;
  float t_hi = psycho17_INTERVAL_MAX;
  float lo;
  float hi;

  psycho17_IntervalLower0(a.x, rgb_t0.x, lo, hi);
  t_lo = max(t_lo, lo);
  t_hi = min(t_hi, hi);

  psycho17_IntervalLower0(a.y, rgb_t0.y, lo, hi);
  t_lo = max(t_lo, lo);
  t_hi = min(t_hi, hi);

  psycho17_IntervalLower0(a.z, rgb_t0.z, lo, hi);
  t_lo = max(t_lo, lo);
  t_hi = min(t_hi, hi);

  if (t_hi < t_lo) {
    result.rgbOut = max(rgb2020_linear, vec3(0.0));
    result.rgbEdgeGamut = rgb_t0;
    return result;
  }

  float t_max = max(0.0, t_hi);
  result.tMaxGamut = t_max;
  result.purityCur01 = (t_max > psycho17_EPSILON)
                           ? clamp(psycho17_DivideSafe(1.0, t_max, 0.0), 0.0, 1.0)
                           : 0.0;
  result.rgbEdgeGamut = a * t_max + rgb_t0;

  result.purity01_in = clamp(purity01, 0.0, 1.0);
  float gamma = max(curve_gamma, 1e-6);
  result.purity01_used = pow(result.purity01_in, gamma);
  result.tFinal = result.purity01_used * t_max;

  vec2 mb_final = white + result.tFinal * direction;
  result.rgbOut = max(psycho17_LMS2006ToBT2020(psycho17_LMS2006FromMBT(mb_final, t)), vec3(0.0));
  return result;
}

float psycho17_Purity01FromBT2020MB(vec3 rgb2020_linear, vec2 mb_white_override, float t_min) {
  return psycho17_ApplyBT2020MBPurity(rgb2020_linear, 1.0, 1.0, mb_white_override, t_min).purityCur01;
}

vec3 CorrectHueAndPurityMB_BT2020(
    vec3 target_color_bt2020,
    vec3 reference_color_bt2020,
    float hue_strength,
    float purity_strength,
    float curve_gamma,
    vec2 mb_white_override,
    float t_min) {
  if (purity_strength <= 0.0 && hue_strength <= 0.0) {
    return target_color_bt2020;
  }

  float applied_purity01 = mix(
      psycho17_ApplyBT2020MBPurity(target_color_bt2020, 1.0, 1.0, mb_white_override, t_min).purityCur01,
      psycho17_ApplyBT2020MBPurity(reference_color_bt2020, 1.0, 1.0, mb_white_override, t_min).purityCur01,
      clamp(purity_strength, 0.0, 1.0));

  if (hue_strength <= 0.0) {
    return psycho17_ApplyBT2020MBPurity(
               target_color_bt2020, applied_purity01, curve_gamma, mb_white_override, t_min)
        .rgbOut;
  }

  vec3 target_lms = psycho17_BT2020ToLMS2006(target_color_bt2020);
  float target_t = target_lms.x + target_lms.y;
  if (target_t <= t_min) {
    return target_color_bt2020;
  }

  float hue_blend = clamp(hue_strength, 0.0, 1.0);
  if (hue_blend <= 0.0) {
    return psycho17_ApplyBT2020MBPurity(
               target_color_bt2020, applied_purity01, curve_gamma, mb_white_override, t_min)
        .rgbOut;
  }

  vec3 reference_lms = psycho17_BT2020ToLMS2006(reference_color_bt2020);
  vec2 white = (mb_white_override.x >= 0.0 && mb_white_override.y >= 0.0)
                   ? mb_white_override
                   : psycho17_MB2006WhiteD65();

  vec2 target_direction = psycho17_MB2006FromLMS(target_lms) - white;
  vec2 reference_direction = psycho17_MB2006FromLMS(reference_lms) - white;
  float target_len_sq = dot(target_direction, target_direction);
  float reference_len_sq = dot(reference_direction, reference_direction);

  if (target_len_sq < psycho17_MB_NEAR_WHITE_EPSILON
      && reference_len_sq < psycho17_MB_NEAR_WHITE_EPSILON) {
    return psycho17_ApplyBT2020MBPurity(
               target_color_bt2020, applied_purity01, curve_gamma, mb_white_override, t_min)
        .rgbOut;
  }

  vec2 target_unit = (target_len_sq > psycho17_MB_NEAR_WHITE_EPSILON)
                         ? target_direction * inversesqrt(target_len_sq)
                         : vec2(0.0);
  vec2 reference_unit = (reference_len_sq > psycho17_MB_NEAR_WHITE_EPSILON)
                            ? reference_direction * inversesqrt(reference_len_sq)
                            : target_unit;
  if (target_len_sq <= psycho17_MB_NEAR_WHITE_EPSILON) {
    target_unit = reference_unit;
  }

  vec2 blended_unit = mix(target_unit, reference_unit, hue_blend);
  float blended_len_sq = dot(blended_unit, blended_unit);
  if (blended_len_sq <= psycho17_MB_NEAR_WHITE_EPSILON) {
    blended_unit = (hue_blend >= 0.5) ? reference_unit : target_unit;
    blended_len_sq = dot(blended_unit, blended_unit);
  }
  blended_unit *= inversesqrt(max(blended_len_sq, 1e-20));

  float seed_len = sqrt(max(target_len_sq, 0.0));
  if (seed_len <= 1e-6) {
    seed_len = sqrt(max(reference_len_sq, 0.0));
  }
  seed_len = max(seed_len, 1e-6);

  vec3 seed_bt2020 = psycho17_LMS2006ToBT2020(
      psycho17_LMS2006FromMBT(white + blended_unit * seed_len, target_t));

  return psycho17_ApplyBT2020MBPurity(
             seed_bt2020, applied_purity01, curve_gamma, mb_white_override, t_min)
      .rgbOut;
}

vec3 CorrectHueAndPurityMB_BT2020(
    vec3 target_color_bt2020,
    vec3 reference_color_bt2020,
    float hue_strength,
    float purity_strength,
    float curve_gamma,
    vec2 mb_white_override) {
  return CorrectHueAndPurityMB_BT2020(
      target_color_bt2020, reference_color_bt2020, hue_strength, purity_strength,
      curve_gamma, mb_white_override, 1e-7);
}

vec3 CorrectHueAndPurityMB_BT2020(
    vec3 target_color_bt2020,
    vec3 reference_color_bt2020,
    float hue_strength,
    float purity_strength,
    float curve_gamma) {
  return CorrectHueAndPurityMB_BT2020(
      target_color_bt2020, reference_color_bt2020, hue_strength, purity_strength,
      curve_gamma, vec2(-1.0), 1e-7);
}

vec3 CorrectHueAndPurityMB_BT2020(
    vec3 target_color_bt2020,
    vec3 reference_color_bt2020,
    float hue_strength,
    float purity_strength) {
  return CorrectHueAndPurityMB_BT2020(
      target_color_bt2020, reference_color_bt2020, hue_strength, purity_strength,
      1.0, vec2(-1.0), 1e-7);
}

vec3 CorrectHueAndPurityMB_BT2020(
    vec3 target_color_bt2020,
    vec3 reference_color_bt2020,
    float hue_strength) {
  return CorrectHueAndPurityMB_BT2020(
      target_color_bt2020, reference_color_bt2020, hue_strength, 1.0,
      1.0, vec2(-1.0), 1e-7);
}

vec3 CorrectHueAndPurityMB_BT2020(
    vec3 target_color_bt2020,
    vec3 reference_color_bt2020) {
  return CorrectHueAndPurityMB_BT2020(
      target_color_bt2020, reference_color_bt2020, 1.0, 1.0,
      1.0, vec2(-1.0), 1e-7);
}

vec3 psycho17_ApplyPurityAndHueFromBT2020(
    vec3 bt2020_source,
    vec3 bt2020_target,
    float purity_amount,
    float hue_amount,
    float clamp_purity_loss,
    float eps,
    bool compress_gamut,
    bool use_adaptive_hue_sensitivity) {
  if (purity_amount <= 0.0 && hue_amount <= 0.0) {
    return bt2020_target;
  }

  vec2 mb_white = psycho17_MB2006WhiteD65();
  float applied_purity_strength = purity_amount;
  float purity_loss_clamp = clamp_purity_loss;
  float target_purity01 = 0.0;
  float source_purity01 = 0.0;
  bool has_purity_pair = false;

  if (applied_purity_strength > 0.0 && purity_loss_clamp > 0.0) {
    target_purity01 = psycho17_Purity01FromBT2020MB(bt2020_target, mb_white, eps);
    source_purity01 = psycho17_Purity01FromBT2020MB(bt2020_source, mb_white, eps);
    has_purity_pair = true;

    float transferred_purity01 = mix(target_purity01, source_purity01, applied_purity_strength);
    float no_purity_loss01 = max(transferred_purity01, target_purity01);
    transferred_purity01 = mix(transferred_purity01, no_purity_loss01, purity_loss_clamp);

    float purity_span01 = source_purity01 - target_purity01;
    if (abs(purity_span01) > eps) {
      applied_purity_strength = clamp((transferred_purity01 - target_purity01) / purity_span01, 0.0, 1.0);
    } else {
      applied_purity_strength = 0.0;
    }
  }

  float applied_hue_strength = clamp(hue_amount, 0.0, 1.0);
  if (applied_hue_strength > 0.0 && use_adaptive_hue_sensitivity) {
    vec3 target_lms = psycho17_BT2020ToLMS2006(bt2020_target);
    vec2 target_direction = psycho17_MB2006FromLMS(target_lms) - mb_white;
    if (dot(target_direction, target_direction) > psycho17_MB_NEAR_WHITE_EPSILON) {
      float target_t_clip = psycho17_RayExitTCIE1702(mb_white, target_direction);
      applied_hue_strength *= psycho17_AdaptiveHueSensitivityFromTClip(target_t_clip);
    }
  }

  vec3 bt2020_out;
  if (applied_hue_strength <= 0.0) {
    if (!has_purity_pair) {
      target_purity01 = psycho17_Purity01FromBT2020MB(bt2020_target, mb_white, eps);
      source_purity01 = psycho17_Purity01FromBT2020MB(bt2020_source, mb_white, eps);
      has_purity_pair = true;
    }

    float applied_purity01 = mix(target_purity01, source_purity01, applied_purity_strength);
    bt2020_out = psycho17_ApplyBT2020MBPurity(bt2020_target, applied_purity01, 1.0, mb_white, eps).rgbOut;
  } else {
    bt2020_out = CorrectHueAndPurityMB_BT2020(
        bt2020_target,
        bt2020_source,
        applied_hue_strength,
        applied_purity_strength,
        1.0,
        mb_white,
        eps);
  }

  if (!compress_gamut) {
    return bt2020_out;
  }

  vec3 lms_out = psycho17_GamutCompressLMSBoundAdaptive(
      psycho17_BT2020ToLMS(bt2020_out),
      vec3(1.0),
      psycho17_BT2020_TO_LMS_WEIGHTED_MAT,
      1.0);
  return psycho17_LMSToBT2020(lms_out);
}

vec3 psycho17_ApplyPurityAndHueFromBT2020(
    vec3 bt2020_source,
    vec3 bt2020_target,
    float purity_amount,
    float hue_amount,
    float clamp_purity_loss,
    float eps,
    bool compress_gamut) {
  return psycho17_ApplyPurityAndHueFromBT2020(
      bt2020_source, bt2020_target, purity_amount, hue_amount,
      clamp_purity_loss, eps, compress_gamut, false);
}

vec3 psycho17_ApplyPurityAndHueFromBT2020(
    vec3 bt2020_source,
    vec3 bt2020_target,
    float purity_amount,
    float hue_amount,
    float clamp_purity_loss,
    float eps) {
  return psycho17_ApplyPurityAndHueFromBT2020(
      bt2020_source, bt2020_target, purity_amount, hue_amount,
      clamp_purity_loss, eps, true, false);
}

vec3 psycho17_ApplyPurityAndHueFromBT2020(
    vec3 bt2020_source,
    vec3 bt2020_target,
    float purity_amount,
    float hue_amount,
    float clamp_purity_loss) {
  return psycho17_ApplyPurityAndHueFromBT2020(
      bt2020_source, bt2020_target, purity_amount, hue_amount,
      clamp_purity_loss, 1e-7, true, false);
}

vec3 psycho17_ApplyPurityAndHueFromBT2020(
    vec3 bt2020_source,
    vec3 bt2020_target,
    float purity_amount,
    float hue_amount) {
  return psycho17_ApplyPurityAndHueFromBT2020(
      bt2020_source, bt2020_target, purity_amount, hue_amount,
      0.0, 1e-7, true, false);
}

vec3 psycho17_ApplyPurityAndHueFromBT2020(
    vec3 bt2020_source,
    vec3 bt2020_target,
    float purity_amount) {
  return psycho17_ApplyPurityAndHueFromBT2020(
      bt2020_source, bt2020_target, purity_amount, 1.0,
      0.0, 1e-7, true, false);
}

vec3 psycho17_ApplyPurityAndHueFromBT2020(
    vec3 bt2020_source,
    vec3 bt2020_target) {
  return psycho17_ApplyPurityAndHueFromBT2020(
      bt2020_source, bt2020_target, 1.0, 1.0,
      0.0, 1e-7, true, false);
}

// See psychov_17.hlsl for the full design notes; they are omitted here to keep
// the GLSL port focused on the executable code.

vec3 psychov_17(
    vec3 bt709_linear_input,
    float peak_value,
    float exposure,
    float highlights,
    float shadows,
    float contrast,
    float purity_scale,
    float bleaching_intensity,
    float clip_point,
    float hue_restore,
    float adaptation_contrast,
    int white_curve_mode,
    float cone_response_exponent,
    vec3 current_adaptive_state_bt709,
    vec3 current_background_state_bt709,
    float gamut_compression,
    int gamut_compression_mode,
    float adaptive_normalization) {
  vec3 bt709_scene = bt709_linear_input * exposure;

  vec3 lms_in = psycho17_BT709ToLMS(bt709_scene);
  vec3 lms_peak = psycho17_BT709ToLMS(vec3(peak_value));
  vec3 current_adaptive_state_lms = psycho17_BT709ToLMS(current_adaptive_state_bt709);
  vec3 desired_background_state_lms = psycho17_BT709ToLMS(current_background_state_bt709);
  vec3 lms_working = lms_in;
  if (true) {
    // noop
  } else if (gamut_compression == 0.0) {
    lms_working = psycho17_GamutCompressLMSBoundAdaptive(
        lms_in,
        current_adaptive_state_lms,
        psycho17_LMS_TO_LMS_WEIGHTED_MAT,
        1.0);
  } else if (gamut_compression_mode == 0) {
    lms_working = psycho17_GamutCompressLMSBoundAdaptive(
        lms_in,
        current_adaptive_state_lms,
        psycho17_BT709_TO_LMS_WEIGHTED_MAT,
        1.0);
  } else {
    lms_working = psycho17_GamutCompressLMSBoundAdaptive(
        lms_in,
        current_adaptive_state_lms,
        psycho17_BT2020_TO_LMS_WEIGHTED_MAT,
        1.0);
  }

  float yf_input = psycho17_YFFromLMS(lms_working);
  float yf_midgray = psycho17_YFFromBT709(vec3(0.18));
  float yf_target = yf_input;

  // Stage 1: apply UI highlight/shadow/contrast controls in luminosity space.
  if (highlights != 1.0) {
    yf_target = psycho17_Highlights(yf_target, highlights, yf_midgray);
  }
  if (shadows != 1.0) {
    yf_target = psycho17_Shadows(yf_target, shadows, yf_midgray);
  }
  if (contrast != 1.0) {
    yf_target = psycho17_ContrastSafe(yf_target, contrast, yf_midgray);
  }

  float yf_scale = psycho17_DivideSafe(yf_target, yf_input, 1.0);

  vec3 lms_graded = lms_working * yf_scale;
  if (purity_scale != 1.0) {
    vec3 lms_graded_relative = psycho17_ToAdaptiveRelativeLMS(
        lms_graded,
        current_adaptive_state_lms);
    vec3 mb = psycho17_MacleodBoyntonFromLMS(lms_graded_relative);
    vec2 mb_white = psycho17_MacleodBoyntonFromLMS(vec3(1.0)).xy;
    vec2 mb_scaled = mix(mb_white, mb.xy, purity_scale);
    lms_graded = psycho17_FromAdaptiveRelativeLMS(
        psycho17_LMSFromMacleodBoynton(vec3(mb_scaled, mb.z)),
        current_adaptive_state_lms);
  }

  vec3 lms_cones = lms_graded;

  if (bleaching_intensity != 0.0) {
    vec3 stimulus_trolands = max(current_adaptive_state_lms, 0.0) * 203.0 * 4.0;
    vec3 availability = vec3(1.0) / (vec3(1.0) + stimulus_trolands / 20000.0);
    availability = mix(vec3(1.0), availability, bleaching_intensity);

    float y = lms_cones.x + lms_cones.y;
    float white_y = current_adaptive_state_lms.x + current_adaptive_state_lms.y;
    vec3 white_at_y = current_adaptive_state_lms * (y / white_y);
    vec3 delta = (lms_cones - white_at_y) * availability;
    lms_cones = white_at_y + delta;
  }

  // Naka-Rushton is scale-equivariant if input, peak, and anchors are all
  // normalized by the same adaptive LMS state, so keep the absolute-LMS form.
  vec3 display_scaled = psycho17_NakaRushton(
      lms_cones,
      lms_peak,
      current_adaptive_state_lms,
      desired_background_state_lms,
      cone_response_exponent);
  vec3 display_scaled_relative_weighted = psycho17_ToAdaptiveRelativeWeightedLMS(
      display_scaled,
      current_adaptive_state_lms);

  if (hue_restore > 0.0) {
    vec3 lms_cones_relative_weighted = psycho17_ToAdaptiveRelativeWeightedLMS(
        lms_cones,
        current_adaptive_state_lms);
    vec3 mb_source =
        psycho17_MacleodBoyntonFromWeightedLMS(lms_cones_relative_weighted);
    vec3 mb_display_target =
        psycho17_MacleodBoyntonFromWeightedLMS(display_scaled_relative_weighted);
    vec3 mb_adapted_bg = psycho17_MacleodBoyntonFromLMS(vec3(1.0));

    vec2 source_offset = mb_source.xy - mb_adapted_bg.xy;
    vec2 display_target_offset = mb_display_target.xy - mb_adapted_bg.xy;
    float src2 = dot(source_offset, source_offset);
    float display_tgt2 = dot(display_target_offset, display_target_offset);
    if (src2 > 1e-7 && display_tgt2 > 1e-7) {
      float inv_target_radius = inversesqrt(display_tgt2);
      float target_radius = display_tgt2 * inv_target_radius;
      float source_t_clip = psycho17_RayExitTCIE1702(mb_adapted_bg.xy, source_offset);
      float display_t_clip = psycho17_RayExitTCIE1702(mb_adapted_bg.xy, display_target_offset);
      // Scale hue restoration by purity loss relative to the adapted neutral anchor.
      float source_purity_signal = psycho17_HueRelativePuritySignalFromTClip(source_t_clip);
      float display_purity_signal = psycho17_HueRelativePuritySignalFromTClip(display_t_clip);
      float purity_signal_loss = clamp(display_purity_signal / source_purity_signal, 0.0, 1.0);
      float hue_sensitivity = psycho17_AdaptiveHueSensitivityFromTClip(display_t_clip);
      float restore_weight = hue_sensitivity * hue_restore * purity_signal_loss;
      if (restore_weight > 0.0) {
        float inv_source_radius = inversesqrt(src2);
        vec2 source_dir = source_offset * inv_source_radius;
        vec2 display_target_dir = display_target_offset * inv_target_radius;
        vec2 blended_dir = mix(display_target_dir, source_dir, restore_weight);
        float blended_len2 = dot(blended_dir, blended_dir);
        if (blended_len2 > 1e-7) {
          blended_dir *= inversesqrt(blended_len2);
        } else {
          blended_dir = display_target_dir;
        }

        // Keep display-scaled chroma radius and y_MB; only replace hue direction.
        vec2 mb_restored_xy = mb_adapted_bg.xy + blended_dir * target_radius;
        vec3 mb_restored = vec3(mb_restored_xy, mb_display_target.z);
        display_scaled_relative_weighted =
            psycho17_WeightedLMSFromMacleodBoynton(mb_restored);
      }
    }
  }

  if (gamut_compression != 0.0) {
    if (gamut_compression_mode == 0) {
      display_scaled_relative_weighted = psycho17_GamutCompressAdaptiveRelativeWeightedLMSBound(
          display_scaled_relative_weighted,
          current_adaptive_state_lms,
          psycho17_BT709_TO_LMS_WEIGHTED_MAT,
          gamut_compression);
    } else if (gamut_compression_mode == 1) {
      display_scaled_relative_weighted = psycho17_GamutCompressAdaptiveRelativeWeightedLMSBound(
          display_scaled_relative_weighted,
          current_adaptive_state_lms,
          psycho17_BT2020_TO_LMS_WEIGHTED_MAT,
          gamut_compression);
    }
  }

  // Scale back from first-site adaptation;
  return psycho17_LMSToBT709(
      psycho17_UnweighLMS(
          psycho17_FromAdaptiveRelativeWeightedLMS(
              display_scaled_relative_weighted,
              current_adaptive_state_lms)));
}

vec3 psycho17_SDR(vec3 bt709_input, float cone_response) {
  return psychov_17(
      bt709_input,     // BT709 input
      1.0,             // SDR peak
      1.0,             // Exposure
      1.0,             // Highlights
      1.0,             // Shadows
      1.0,             // Contrast
      1.0,             // Purity scale
      1.0,             // Bleaching intensity
      100.0,           // Clip point
      1.0,             // Hue restore
      1.0,             // Adaptation contrast
      0,               // White curve mode
      cone_response,   // Cone response exponent
      vec3(0.18),      // Current adaptive state BT709
      vec3(0.18),      // Current background state BT709
      1.0,             // Gamut compression
      0,               // BT709 gamut compression mode
      1.0);            // Adaptive normalization
}

vec3 psycho17_SDR(vec3 bt709_input) {
  return psycho17_SDR(bt709_input, 1.0);
}

vec3 psycho17_HDR(vec3 bt709_input, float peak, float cone_response) {
  return psychov_17(
      bt709_input,     // BT709 input
      peak,            // HDR peak relative to SDR white
      1.0,             // Exposure
      1.0,             // Highlights
      1.0,             // Shadows
      1.0,             // Contrast
      1.0,             // Purity scale
      1.0,             // Bleaching intensity
      100.0,           // Clip point
      1.0,             // Hue restore
      1.0,             // Adaptation contrast
      0,               // White curve mode
      cone_response,   // Cone response exponent
      vec3(0.18),      // Current adaptive state BT709
      vec3(0.18),      // Current background state BT709
      1.0,             // Gamut compression
      1,               // BT2020 gamut compression mode
      1.0);            // Adaptive normalization
}

vec3 psycho17_HDR(vec3 bt709_input, float peak) {
  return psycho17_HDR(bt709_input, peak, 1.0);
}

vec3 psycho17_HDR(vec3 bt709_input) {
  return psycho17_HDR(bt709_input, 1000.0 / 203.0, 1.0);
}
// ---- end psychov_17.glsl ----

in vec4 gl_FragCoord;

out vec4 out_FragColor0;

void main() {
	// RENODX: max-channel factor of the HDR scene (>= 1), see bridge in / out below.
	float renodx_scene_max = 1.0;
	vec2 viewTexCoord = screenPosToTexcoord( gl_FragCoord.xy, _fa_[0 ] );
	vec2 windowTexCoord = screenPosToTexcoord( gl_FragCoord.xy, _fa_[1 ] );
	windowTexCoord.y = 1.0 - windowTexCoord.y;
	vec2 offset;
	{
		vec2 scale = 2.0 * viewTexCoord.xy - 1.0;
		vec2 blah = saturate( vec2( 0.8 - scale.x * scale.x, 0.8 - scale.y * scale.y ) );
		offset = _fa_[2 ].xy * blah;
	};
	viewTexCoord += offset;
	vec4 glareTex = h4tex2D( samp_glaremap, viewTexCoord );
	float clipZ;
	{
		{
			clipZ = tex2Ddepth_fast( samp_viewdepthmap, viewTexCoord.xy ) * 2.0 - 1.0;
		};
		{
			clipZ = clipZ * _fa_[3 ].w / ( clipZ + _fa_[3 ].z ) - _fa_[3 ].w;
		};
	};
	float dof;
	{
		float a = saturate( ( _fa_[4 ].x - clipZ) * _fa_[5 ].x ) * _fa_[6 ].x;
		float b = saturate( (clipZ - _fa_[7 ].x) * _fa_[5 ].y ) * _fa_[8 ].x;
		dof = max(a, b);
		dof = (clipZ > 0.0) ? dof : _fa_[9 ].x;
		dof = dof* _fa_[10 ].x + _fa_[10 ].y;
	};
	float blur = 0.0;
	{
		vec2 perturb = vec2( 0.0 );
		vec4 distortion = h4tex2D( samp_postdistortionmap, viewTexCoord.xy ) - 0.5;
		perturb += distortion.xy * 0.1;
		blur += saturate( distortion.w ) * 16.0;
		viewTexCoord.xy += perturb.xy;
	};
	float blurMip = ( dof * _fa_[11 ].x ) + blur;
	blurMip = log2( blurMip * 5.0 + 1.0 );
	vec4 final;
	{
		if ( blurMip > 0.5 ) {
			float offset = 0.2;
			float sampleDof = blurMip - 1.5;
			vec2 xyStep = _fa_[0 ].zw * pow( 2, sampleDof - offset );
			sampleDof += offset;
			final = tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( xyStep.x, xyStep.y ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( -xyStep.x, xyStep.y ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( 0.0 , 0.0 ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( xyStep.x, -xyStep.y ), 0.0, sampleDof ) );
			final += tex2Dlod( samp_viewcolormap, vec4( viewTexCoord + vec2( -xyStep.x, -xyStep.y ), 0.0, sampleDof ) );
			final *= ( 1.0 / 5.0 );
		} else {
			final = tex2Dlod( samp_viewcolormap, vec4( viewTexCoord, 0.0, blurMip ) );
		}
	};
	{ if ( _fa_[12 ].x != 0.0 ) {
			int numsamples = 32;
			vec4 col = vec4( 0.0 );
			for ( int i = 0; i < numsamples; ++i ) {
				float scale = 1.0 - ( _fa_[13 ].x ) * ( i / float( numsamples - 1.0 ) );
				col += tex2D( samp_viewcolormap, scale * viewTexCoord.xy + ( 1.0 - scale ) * _fa_[14 ].xy ) * ( numsamples - i ) * ( numsamples - i );
			}
			col = col / ( numsamples * numsamples * numsamples / 4 );
			float luminance = saturate( dot3( col.xyz, vec3( 0.33, 0.59, 0.11 ) ) ) * _fa_[15 ].x;
			final = mix( final, col, sqrt( luminance ) );
		}
	};
	{
		float sharpenAmount = 0.35;
		float maskIntensity = 7.0;
		sharpenAmount = max( 0.0, sharpenAmount - blurMip);
		if( sharpenAmount > 0.0 ) {
			vec3 blurred = tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy, 0.0, blurMip + 1.0 ) ).xyz;
			float finalLuminance;
			float blurredLuminance;
			{
				vec3 ituBT601 = vec3( 0.299, 0.587, 0.114 );
				finalLuminance = dot( final.xyz * final.xyz, ituBT601 );
			};
			{
				vec3 ituBT601 = vec3( 0.299, 0.587, 0.114 );
				blurredLuminance = dot( blurred.xyz * blurred.xyz, ituBT601 );
			};
			{
				vec3 unsharpMask = vec3( saturate( maskIntensity * (finalLuminance - blurredLuminance) ) );
				vec3 sharpen = 5 * final.xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy + vec2( _fa_[0 ].z, 0.0 ), 0.0, blurMip ) ).xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy - vec2( _fa_[0 ].z, 0.0 ), 0.0, blurMip ) ).xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy + vec2( 0.0, _fa_[0 ].w ), 0.0, blurMip ) ).xyz -
				tex2Dlod( samp_viewcolormap, vec4( viewTexCoord.xy - vec2( 0.0, _fa_[0 ].w ), 0.0, blurMip ) ).xyz;
				sharpen = mix( sharpen, final.xyz, unsharpMask );
				final.xyz = mix( final.xyz, sharpen, sharpenAmount );
			}
		}
	};
	{
		final = final * _fa_[16 ] + glareTex;
	};
	{ if ( any( notEqual( _fa_[2 ].xy, vec2( 0.0 ) ) ) ) {
			vec2 tc2 = screenPosToTexcoord( gl_FragCoord.xy, _fa_[0 ] );
			vec2 offset;
			{
				vec2 scale = 2.0 * viewTexCoord.xy - 1.0;
				vec2 blah = saturate( vec2( 0.8 - scale.x * scale.x, 0.8 - scale.y * scale.y ) );
				offset = _fa_[2 ].xy * blah;
			};
			tc2 -= offset;
			vec4 glareTex2 = tex2D( samp_glaremap, tc2.xy );
			vec4 final2 = tex2Dlod( samp_viewcolormap, vec4( tc2.xy, 0.0, blurMip ) ) * _fa_[16 ] + glareTex2;
			final = mix( final, final2, 0.5 );
		}
	};
	{
		vec3 tmpCol = vec3( dot3( final.xyz, vec3( 0.33, 0.59, 0.11 ) ) );
		final.xyz = mix( final.xyz, tmpCol, _fa_[17 ].x );
		// RENODX bridge in: identical to vanilla for every pixel <= 1.0. Above white the colour is divided by its
		// max channel (hue kept), so the game's curve, overlay, grain and LUT see the SDR range they expect.
		renodx_scene_max = max( 1.0, max( final.x, max( final.y, final.z ) ) );
		final.xyz /= renodx_scene_max;
		tmpCol.x = h4tex2D( samp_cbconversionlut, vec2( final.x, 0.0 ) ).x;
		tmpCol.y = h4tex2D( samp_cbconversionlut, vec2( final.y, 0.0 ) ).y;
		tmpCol.z = h4tex2D( samp_cbconversionlut, vec2( final.z, 0.0 ) ).z;
		tmpCol *= _fa_[18 ].x;
		final.xyz = mix( tmpCol.xyz, final.xyz, min( _fa_[19 ].x * dof, 1.0 ) );
	};
	{
		final.xyz *= _fa_[20 ].y + h4tex2D( samp_screenoverlay, windowTexCoord ).xyz * _fa_[20 ].x;
	};
	{
		vec2 graincoord = ( viewTexCoord.xy * _fa_[21 ].xy ) + _fa_[21 ].zw;
		vec3 grain = h4tex2D( samp_grainmap, graincoord ).xyz * _fa_[22 ].x + _fa_[22 ].y;
		float tempLum = dot( final.xyz, vec3( 0.3, 0.59, 0.11 ) );
		tempLum = saturate( ( tempLum * _fa_[22 ].z ) + _fa_[22 ].w );
		grain *= tempLum;
		final.xyz += grain;
		if ( _fa_[23 ].x == 1.0 ) {
			final.xyz = vec3( tempLum );
		}
	};
	{
		float ccTexDim = 16.0; float ccDim = 16.0;
		vec2 duvCC = vec2( 1.0 / ( ccTexDim * ccTexDim ), 1.0 / ccTexDim );
		vec3 tmp = saturate( final.xyz ) * ( ccDim - 1.0 );
		vec2 tcRG = tmp.xy * vec2( 1.0 / ( ccTexDim * ccTexDim ), ( 1.0 / ccTexDim ) ) + ( 0.5 * duvCC );
		float tcB = tmp.z;
		float tcBFrac = fract( tcB );
		float tcB1 = ceil( tcB ) * ( 1.0 / ccTexDim );
		float tcB0 = floor( tcB ) * ( 1.0 / ccTexDim );
		vec3 resCol0 = tex2Dlod( samp_dynamiccc, vec4( tcRG.x + tcB0, tcRG.y, 0, 0 ) ).xyz;
		vec3 resCol1 = tex2Dlod( samp_dynamiccc, vec4( tcRG.x + tcB1, tcRG.y, 0, 0 ) ).xyz;
		vec3 resCol = mix( resCol0, resCol1, tcBFrac );
		// RENODX bridge out: the graded SDR result scaled back up by the same factor. In this gamma-encoded space
		// (x^(1/2.2)) a scale is the same as scaling the linear colour, so this restores the HDR range on the grade.
		final.xyz = resCol * renodx_scene_max;
	};
	// RENODX: tone mapping and Game Brightness. Not in Vanilla.
	if ( RENODX_TONE_MAP_TYPE != 0.0 ) {
		vec3 renodx_linear = renodx_decode( final.xyz, RENODX_INTERMEDIATE_ENCODING );
		if ( RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_PSYCHOV ) {
			// The graded scene (game curve + LUT, extended above white by the bridge) is the untonemapped_graded signal;
			// measured steady highlights reach >= 20x game white (2026-10-07). Slider mapping as in the HLSL PsychoV-17 mods
			// (darktide, rotsp, wutheringwaves): saturation -> purity, blowout -> bleaching; peak relative to game white.
			renodx_linear = psychov_17(
				renodx_linear,
				RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS,  // peak, relative to game white
				RENODX_TONE_MAP_EXPOSURE,
				RENODX_TONE_MAP_HIGHLIGHTS,
				RENODX_TONE_MAP_SHADOWS,
				RENODX_TONE_MAP_CONTRAST,
				RENODX_TONE_MAP_SATURATION,      // purity scale
				RENODX_TONE_MAP_BLOWOUT,         // bleaching intensity
				100.0,                           // clip point (unused by test17)
				RENODX_TONE_MAP_HUE_CORRECTION,  // hue restore
				1.0,                             // adaptation contrast
				0,                               // white curve mode
				1.0,                             // cone response exponent
				vec3( 0.18 ),                    // current adaptive state (BT.709)
				vec3( 0.18 ),                    // current background state (BT.709)
				1.0,                             // gamut compression
				1,                               // gamut compression mode: BT.2020 (HDR10 output)
				1.0 );                           // adaptive normalization
		}
		// Game Brightness: the HUD is drawn on top of this output and the Display Proxy scales the whole frame by UI
		// Brightness (graphics white), so the scene alone is scaled by Game / UI in linear light here
		// (renodx::draw::RenderIntermediatePass convention).
		final.xyz = renodx_encode( renodx_linear * ( RENODX_DIFFUSE_WHITE_NITS / RENODX_GRAPHICS_WHITE_NITS ), RENODX_INTERMEDIATE_ENCODING );
	}
	out_FragColor0.xyz = final.xyz;
	out_FragColor0.w = 1.0;
	if ( _fa_[24 ].x == 1.0 ) {
		out_FragColor0.xyz = vec3( dof );
	}
}