// Hair specular (two Kajiya-Kay lobes), drawn additively into the scene buffer as scratch space and then copied into the
// float16 light buffers. Its sqrt(1 - d^2) and pow produce NaN at grazing angles; vanilla's 8-bit target wrote them as 0.
// The output is saturated to emulate that clamp, otherwise the NaN reaches the light buffers through the upgraded target.

cbuffer sbHairLook : register(b0) {
  struct
  {
    float4 SpecColor1;
    float4 SpecColor2;
    float4 SpecParams1;
    float4 SpecParams2;
    float4 LightDir1_AmbientOcclusion;
    float4 LightDir2;
  } sbHairLook : packoffset(c0);
}

cbuffer cbShaderParams : register(b1) {
  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);
}

SamplerState _texDiffuse_s : register(s0);
SamplerState _texSpecular_s : register(s1);
SamplerState _texCollector_s : register(s2);
SamplerState _texNormal_s : register(s3);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texSpecular : register(t1);
Texture2D<float4> texCollector : register(t2);
Texture2D<float4> texNormal : register(t3);

void main(
    float4 v0: SV_Position0,
    float4 v1: TEXCOORD0,
    float3 v2: TEXCOORD1,
    float3 v3: TEXCOORD2,
    float3 v4: TEXCOORD3,
    float3 v5: TEXCOORD4,
    out float4 o0: SV_Target0) {
  float4 r0, r1, r2, r3, r4, r5;

  r0.xyzw = texDiffuse.SampleBias(_texDiffuse_s, v1.zw, -2).xyzw;
  r0.x = r0.w + -0.5;
  if (r0.x < 0) discard;

  r0.x = dot(sbHairLook.LightDir2.xyz, sbHairLook.LightDir2.xyz);
  r0.x = rsqrt(r0.x);
  r0.xyz = sbHairLook.LightDir2.xyz * r0.xxx;
  r1.xyz = texNormal.Sample(_texNormal_s, v1.xy).xyz;
  r1.xyz = r1.xyz * float3(2, 2, 2) + float3(-1, -1, -1);
  r0.w = dot(r1.xyz, r1.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyz = r1.xyz * r0.www;
  r2.xyzw = texSpecular.SampleBias(_texSpecular_s, v1.zw, -2).xyzw;
  r0.w = -0.5 + r2.y;
  r1.w = r0.w * 2 + sbHairLook.SpecParams2.y;
  r0.w = r0.w * 2 + sbHairLook.SpecParams1.y;
  r0.w = sbHairLook.SpecParams1.x * r0.w;
  r3.xyz = r0.www * r1.xyz + v5.xyz;
  r0.w = sbHairLook.SpecParams2.x * r1.w;
  r1.xyz = r0.www * r1.xyz + v5.xyz;
  r0.w = dot(r1.xyz, r1.xyz);
  r0.w = rsqrt(r0.w);
  r1.xyz = r1.xyz * r0.www;
  r0.x = dot(r1.xyz, r0.xyz);
  r0.z = dot(v2.xyz, v2.xyz);
  r0.z = rsqrt(r0.z);
  r4.xyz = v2.xyz * r0.zzz;
  r0.y = dot(r1.xyz, -r4.xyz);
  r0.zw = -r0.xy * r0.xy + float2(1, 1);
  r0.x = r0.y * r0.x;
  r0.yz = sqrt(r0.zw);
  r0.x = r0.y * r0.z + r0.x;
  r0.x = log2(r0.x);
  r0.x = sbHairLook.SpecParams2.w * r0.x;
  r0.x = exp2(r0.x);
  r0.yzw = sbHairLook.SpecColor2.xyz * r2.zzz;
  r0.yzw = r0.yzw * r2.xxx;
  r1.xyz = sbHairLook.SpecColor1.xyz * r2.xxx;
  r2.xyzw = texCollector.Sample(_texCollector_s, v1.xy).xyzw;
  r1.w = max(0.35, r2.w);
  r2.xyz = float3(1.4, 1.4, 1.4) + -cbShaderParams.Value2.xyz;
  r2.xyz = r1.www * r2.xyz + cbShaderParams.Value2.xyz;
  r5.xyz = float3(0.08, 0.08, 0.08) * r2.xyz;
  r0.yzw = r5.xyz * r0.yzw;
  r1.xyz = r5.xyz * r1.xyz;
  r5.xyz = r2.xyz * float3(0.08, 0.08, 0.08) + -r0.yzw;
  r0.yzw = sbHairLook.SpecParams2.zzz * r5.xyz + r0.yzw;
  r0.xyz = r0.yzw * r0.xxx;
  r2.xyz = r2.xyz * float3(0.08, 0.08, 0.08) + -r1.xyz;
  r1.xyz = sbHairLook.SpecParams1.zzz * r2.xyz + r1.xyz;
  r0.w = dot(r3.xyz, r3.xyz);
  r0.w = rsqrt(r0.w);
  r2.xyz = r3.xyz * r0.www;
  r3.y = dot(r2.xyz, -r4.xyz);
  r0.w = dot(sbHairLook.LightDir1_AmbientOcclusion.xyz, sbHairLook.LightDir1_AmbientOcclusion.xyz);
  r0.w = rsqrt(r0.w);
  r4.xyz = sbHairLook.LightDir1_AmbientOcclusion.xyz * r0.www;
  r3.x = dot(r2.xyz, r4.xyz);
  r0.w = r3.y * r3.x;
  r2.xy = -r3.xy * r3.xy + float2(1, 1);
  r2.xy = sqrt(r2.xy);
  r0.w = r2.x * r2.y + r0.w;
  r0.w = log2(r0.w);
  r0.w = sbHairLook.SpecParams1.w * r0.w;
  r0.w = exp2(r0.w);
  r0.xyz = r1.xyz * r0.www + r0.xyz;
  r0.w = rsqrt(v1.w);
  r0.w = 1 / r0.w;
  o0.xyz = r0.www * r0.xyz;
  o0.w = 0;

  o0.xyz = saturate(o0.xyz);
}
