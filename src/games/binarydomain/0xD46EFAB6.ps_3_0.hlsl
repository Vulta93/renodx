#include "./shared.h"

// Binary Domain tone map and composite (ps_3_0 0xD46EFAB6, drawn twice per frame: once with the full resolution particle
// layer, once with the half resolution one, with a constant write 0x76589376 between them).
// Vanilla: scene (R16G16B16A16F, linear, tiny units) * c66.x (CPU auto exposure) -> sqrt(1 - exp(-x)) (gamma 2 encoded,
// per channel, no LUT) -> (curve + bloom) * layer.a + layer.rgb, with bloom and layer already gamma 2 encoded, written to
// an 8-bit target. The display decodes that with its own EOTF (~2.2), so the light on screen is the encoded value ^ 2.2.
// This replacement works in that display-linear domain. Tone Mapper None / Roll-off: below a knee it reproduces vanilla
// exactly, above it the curve continues linearly instead of closing at white. Tone Mapper PsychoV: PsychoV17 anchored to
// the vanilla curve (midgray). The result stays in the game's 2.2 encoding, which is the vanilla encoded value in the SDR
// range, so the fog sprites and the AA pass that follow blend as they always did. Grading, the Game / UI brightness split
// and the roll-off (Roll-off and PsychoV tone mappers) happen later, in 0x19BC57FB, on the finished scene.

float4 c_location[3] : register(c64);  // c65 = (1 / size, half texel), c66.x = exposure
float4 c_ps_user : register(c192);     // c192.x = layer sampling offset scale
sampler2D scene_texture : register(s0);
sampler2D layer_texture : register(s1);
sampler2D bloom_texture : register(s2);

static const float TONE_MAP_TYPE_PSYCHOV = 5.f;  // addon-only value (renodx::draw uses 0-3, Roll-off is 4)

// Vanilla display light (1 - exp(-x)) ^ 1.1 is kept up to x = 1.7 (0.801, 0.904 encoded), then continues with the slope
// it has there (0.1969 per unit of exposed scene), so highlights keep their gradient instead of closing at white.
static const float KNEE_SCENE = 1.7f;
static const float KNEE_LIGHT = 0.800994f;
static const float KNEE_SLOPE = 0.196939f;

// PsychoV anchors on the vanilla curve (output midgray, analytic slope): the vanilla display light (1 - e^-x)^1.1 reaches
// midgray 0.18 at an exposed scene value of 0.236185, where cone = x * V'(x) / V(x) = 0.97521.
static const float PSYCHOV_ANCHOR_IN = 0.236185f;
static const float PSYCHOV_ANCHOR_OUT = 0.18f;
static const float PSYCHOV_CONE_RESPONSE = 0.97521f;

float4 main(float2 vpos : VPOS) : COLOR {
  const float4 c65 = c_location[1];
  const float2 uv = vpos * c65.xy + c65.zw;
  const float3 scene = max(0.f, tex2D(scene_texture, uv).rgb * c_location[2].x);
  const float3 bloom = tex2D(bloom_texture, uv).rgb;
  const float4 layer = tex2D(layer_texture, c65.xy * vpos + c65.zw * c_ps_user.x);

  const float3 vanilla_light = pow(1.f - exp2(-1.44269502f * scene), 1.1f);
  float3 light = vanilla_light;
  [branch]
  if (RENODX_TONE_MAP_TYPE == TONE_MAP_TYPE_PSYCHOV) {
    // Arguments follow the declaration of psychotm_test17 (src/shaders/tonemap/psychov/test17.hlsl). Exposure, highlights,
    // shadows, contrast and purity stay neutral because the grading sliders run later on the finished scene. Bleaching is
    // off (an observer effect that desaturates bright colors, so not neutral); the unused clip point, adaptation contrast
    // and white curve mode keep the function defaults. Negative lobes are clamped like the 8-bit target did.
    light = max(0.f, renodx::tonemap::psychov::psychotm_test17(
                         scene,
                         RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS,
                         1.f,
                         1.f,
                         1.f,
                         1.f,
                         1.f,
                         0.f,
                         100.f,
                         1.f,
                         1.f,
                         0,
                         PSYCHOV_CONE_RESPONSE,
                         PSYCHOV_ANCHOR_IN.xxx,
                         PSYCHOV_ANCHOR_OUT.xxx,
                         1.f,
                         1));
  } else if (RENODX_TONE_MAP_TYPE != renodx::draw::TONE_MAP_TYPE_VANILLA) {
    light = lerp(vanilla_light, KNEE_LIGHT + KNEE_SLOPE * (scene - KNEE_SCENE), step(KNEE_SCENE, scene));
  }

  // Vanilla blends bloom and the particle layer in the encoded domain (what the 8-bit target held; its alpha was clamped).
  float3 encoded = (renodx::color::gamma::EncodeSafe(light, 2.2f) + bloom) * saturate(layer.a) + layer.rgb;
  [branch]
  if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_VANILLA) {
    encoded = saturate(encoded);
  }
  return float4(encoded, 1.f);
}
