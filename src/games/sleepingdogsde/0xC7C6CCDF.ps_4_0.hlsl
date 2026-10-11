// Copies the hair specular (drawn by 0x4D449B45 into the scene buffer as additive scratch space) into the light buffers.
// Vanilla's 8-bit scratch clamped the sum of the overlapping hair layers at 1; the float16 upgrade does not, so the
// input is saturated to keep hair highlights at the vanilla level.
SamplerState _texDiffuse_s : register(s0);
SamplerState _texDiffuse2_s : register(s1);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texDiffuse2 : register(t1);

void main(
    float4 v0: SV_Position0,
    float4 v1: TEXCOORD0,
    float3 v2: TEXCOORD1,
    float3 v3: TEXCOORD2,
    float3 v4: TEXCOORD3,
    float3 v5: TEXCOORD4,
    out float4 o0: SV_Target0) {
  if (texDiffuse.SampleBias(_texDiffuse_s, v1.zw, -2).w + -0.5 < 0) discard;

  o0.xyz = saturate(texDiffuse2.Sample(_texDiffuse2_s, v1.xy).xyz);
  o0.w = 1;
}
