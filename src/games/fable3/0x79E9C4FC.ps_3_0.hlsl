#include "./shared.h"

// Fable III composite permutation 0x79E9C4FC: no depth buffer / depth of field, no dust, no displacement (dialogue scene,
// 2026-10-03 capture).

float4 g_BloomFactor : register(c56);
float4 g_RecipMaxLuminance : register(c57);
float4 g_ColourSensitivityThreshold : register(c58);
float4 g_SaturationBrightnessBaseAndOffset : register(c59);
float4 g_GlobalGammaAdjustment : register(c60);
float4 g_InputResolution : register(c61);
// Bool constant is left to the compiler (b0, as in the original); an explicit register(bN) is rejected with /Gec.
bool g_EnableScreenspaceAA;

sampler g_HDRSampler : register(s0);
sampler g_ToneMapSampler : register(s1);
sampler g_BloomSampler : register(s2);

#include "./composite.hlsli"
