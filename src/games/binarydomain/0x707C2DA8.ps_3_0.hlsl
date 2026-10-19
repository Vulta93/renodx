// Binary Domain HUD sprite (ps_3_0 0x707C2DA8, drawn ~17 times per frame into the HUD buffer after the scene copy).
// Vanilla: rgb = vertex color * texture + vertex color1, a = texture.a * vertex alpha, written to an 8-bit target, which
// clamps the shader output to [0, 1] before blending. The HUD buffer is float16 now, so the clamp is done here: without it
// the heartbeat pulse (offset term) reached ~2.7x white and showed up at Peak Brightness.
sampler2D s0 : register(s0);

float4 main(float4 color : COLOR0, float3 color_offset : COLOR1, float2 uv : TEXCOORD0) : COLOR {
  const float4 texel = tex2D(s0, uv);
  return saturate(float4(color.rgb * texel.rgb + color_offset, texel.a * color.a));
}
