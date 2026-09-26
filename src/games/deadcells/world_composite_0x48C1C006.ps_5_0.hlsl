#include "./common.hlsl"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float4 position : SV_POSITION,
    float4 pixelColor : pixelColor,
    float2 calculatedUV : calculatedUV,
    float displaceBias : displaceBias,
    out float4 o0 : SV_TARGET0)
{
  // Original game math (verified against the dumped shader)
  float4 color = t0.Sample(s0_s, calculatedUV);
  color *= pixelColor;
  color *= (1.0f - displaceBias);

  // RenoDX: gamma -> linear, tone map + color grade, apply Game Brightness
  color.rgb = renodx::color::srgb::DecodeSafe(color.rgb);
  color.rgb = renodx::draw::ToneMapPass(color.rgb);
  color.rgb = renodx::draw::RenderIntermediatePass(color.rgb);

  o0 = color;
}