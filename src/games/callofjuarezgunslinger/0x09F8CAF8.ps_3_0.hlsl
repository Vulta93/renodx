// Chrome Engine 5 post-process composite (uber-shader permutation).
// Features: DOF blur, overlay, desaturate/tint. Generated from the Devkit dump - see composite.hlsli.
#define COJ_BLUR 1
#define COJ_OVERLAY 1
#define COJ_NOISE 0
#define COJ_DESATURATE 1
#define COJ_LEVELS 0
#define COJ_TINT 0

float4 CONST_100 : register(c0);
float4 CONST_102 : register(c1);
float4 f_curves_new : register(c2);
float4 v_pp_desaturate_factor_lum : register(c3);
float4 v_pp_desaturate_factor_mask : register(c4);
float4 v_pp_desaturate_tint__weight : register(c5);
float4 v_pp_desaturate_tint_masked : register(c6);
sampler2D s_clr : register(s0);
sampler2D s_glow : register(s1);
sampler2D s_blur : register(s2);
sampler2D s_overlay : register(s3);
sampler2D s_crv : register(s4);

#include "./composite.hlsli"
