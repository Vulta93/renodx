#ifndef SRC_SLEEPINGDOGSDE_SHARED_H_
#define SRC_SLEEPINGDOGSDE_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
struct ShaderInjectData {
  float toneMapType;
  float toneMapPeakNits;
  float toneMapGameNits;
  float toneMapUINits;
  float toneMapHueCorrection;
  float colorGradeExposure;
  float colorGradeHighlights;
  float colorGradeShadows;
  float colorGradeContrast;
  float colorGradeSaturation;
};

#ifndef __cplusplus
cbuffer cb13 : register(b13) {
  ShaderInjectData injectedData : packoffset(c0);
}

// Map the existing sliders onto the standard RenoDX draw config, so
// renodx::draw::SwapChainPass() works (must be defined before renodx.hlsl).
#define RENODX_PEAK_WHITE_NITS     injectedData.toneMapPeakNits
#define RENODX_DIFFUSE_WHITE_NITS  injectedData.toneMapGameNits
#define RENODX_GRAPHICS_WHITE_NITS injectedData.toneMapUINits
// Keep the mod's existing pure gamma 2.2 behaviour: no sRGB->2.2 gamma correction
// (RenoDX default would be 2.2 correction, which darkens shadows vs. what we tested)
// and a gamma 2.2 intermediate between RenderIntermediatePass and SwapChainPass.
#define RENODX_GAMMA_CORRECTION      0.f  // GAMMA_CORRECTION_NONE
#define RENODX_INTERMEDIATE_ENCODING 2.f  // ENCODING_GAMMA_2_2

#include "../../shaders/renodx.hlsl"
#endif

#endif  // SRC_SLEEPINGDOGSDE_SHARED_H_
