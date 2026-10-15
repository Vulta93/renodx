// Quad covering the viewport for watch_region_reduce_dx9.ps_3_0.hlsl (devkit_watch_region on Direct3D 9). The pixel
// shader addresses pixels through VPOS, so no texture coordinates are passed.

float4 main(float2 position : POSITION) : POSITION {
  return float4(position, 0.f, 1.f);
}
