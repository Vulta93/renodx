#include "./shared.h"

// Fable III composite permutation 0x1902BEBB: dust planes + depth of field, no displacement (in-game cutscene, 2026-10-03 capture).
#define FABLE3_HAS_DOF
#define FABLE3_HAS_DUST

float4 g_PerspectiveConstants : register(c25);
float4 g_AmbientNormalMapDarkeningColour : register(c31);  // CTAB name; only .w is read (scale of the blurred DoF colour)
float4 g_DustPlanes[8] : register(c56);
float4 g_DepthOfFieldPlanes : register(c69);
float4 g_DepthOfFieldUnitMaxBlurNearFar : register(c70);
float4 g_DustPlanesZ : register(c71);
float4 g_DustPlanesAlpha : register(c77);
float4 g_BloomFactor : register(c78);
float4 g_RecipMaxLuminance : register(c79);
float4 g_ColourSensitivityThreshold : register(c128);
float4 g_SaturationBrightnessBaseAndOffset : register(c129);
float4 g_GlobalGammaAdjustment : register(c130);
float4 g_InputResolution : register(c131);
// Bool constants are left to the compiler (b0, b1 in declaration order, as in the original); an explicit register(bN) is rejected with /Gec.
bool g_EnableScreenspaceAA;
bool g_UseOldDoFCalculation;

sampler g_DustSampler : register(s0);
sampler g_HDRSampler : register(s1);
sampler g_DepthSampler : register(s2);
sampler g_ToneMapSampler : register(s3);
sampler g_BloomSampler : register(s4);
sampler g_DepthOfFieldBlurSampler : register(s5);
sampler g_DepthOfFieldFactorSampler : register(s6);

#include "./composite.hlsli"
