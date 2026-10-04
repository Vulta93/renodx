#include "./shared.h"

// Fable III composite permutation 0x794A5A08: screen-space displacement + depth of field, no dust planes (castle garden looking at the sky,
// 2026-10-04 capture). Same inputs as 0x5A6CF82E without the dust planes, so the constants sit at lower registers.
#define FABLE3_HAS_DOF
#define FABLE3_HAS_DISPLACEMENT

float4 g_PerspectiveConstants : register(c25);
float4 g_AmbientNormalMapDarkeningColour : register(c31);  // CTAB name; only .w is read (scale of the blurred DoF colour)
float4 g_DisplacementScale : register(c56);
float4 g_DepthOfFieldPlanes : register(c57);
float4 g_DepthOfFieldUnitMaxBlurNearFar : register(c58);
float4 g_BloomFactor : register(c59);
float4 g_RecipMaxLuminance : register(c60);
float4 g_ColourSensitivityThreshold : register(c61);
float4 g_SaturationBrightnessBaseAndOffset : register(c62);
float4 g_GlobalGammaAdjustment : register(c63);
float4 g_InputResolution : register(c69);
// Bool constants are left to the compiler (b0, b1 in declaration order, as in the original); an explicit register(bN) is rejected with /Gec.
bool g_EnableScreenspaceAA;
bool g_UseOldDoFCalculation;

sampler g_HDRSampler : register(s0);
sampler g_DepthSampler : register(s1);
sampler g_ToneMapSampler : register(s2);
sampler g_BloomSampler : register(s3);
sampler g_DepthOfFieldBlurSampler : register(s4);
sampler g_DepthOfFieldFactorSampler : register(s5);
sampler g_DisplacementSampler : register(s6);

#include "./composite.hlsli"
