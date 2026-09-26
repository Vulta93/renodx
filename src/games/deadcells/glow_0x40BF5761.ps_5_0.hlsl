#include "./common.hlsl"

Texture2D<float4> t0 : register(t0);
SamplerState s0_s : register(s0);

void main(
    float2 v1 : calculatedUV,
    out float4 o0 : SV_TARGET0)
{
  o0 = t0.Sample(s0_s, v1.xy);

  o0.rgb = renodx::color::gamma::DecodeSafe(o0.rgb, 2.2f);
  o0.rgb *= RENODX_DIFFUSE_WHITE_NITS / renodx::color::srgb::REFERENCE_WHITE;
  o0.rgb = renodx::color::gamma::EncodeSafe(o0.rgb, 2.2f);
}
