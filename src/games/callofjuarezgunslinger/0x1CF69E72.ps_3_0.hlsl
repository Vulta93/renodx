// Damage "tear" HUD effect, step 1: fills the composite target (#2) with white (rgb 1, a 0).
// #2 carries the HDR-encoded image (see common.hlsli); a plain SDR 1.0 would decode to ~4000x
// white in the final pass, so write the encoding of 1.0 instead.
#include "./common.hlsli"

float4 main() : COLOR {
  float3 color = float3(1.f, 1.f, 1.f);
  if (RENODX_TONE_MAP_TYPE > 0.f) color = CoJEncodeHDR(color);
  return float4(color, 0.f);
}
