// Reduces one measured region on Direct3D 9, which has no compute shaders or UAVs (devkit_watch_region).
// Every output pixel reduces an 8x8 block of its input; the passes repeat until one pixel is left. The statistics are the
// ones of watch_region_reduce.cs_5_x.hlsl, as 16 floats in four groups (keep in sync with D3D9_STATS_* in
// mcp/watch_region.hpp):
//   group 0: max luminance, min luminance, max channel, luminance sum
//   group 1: finite, non-finite, negative and above-1.0 pixel counts
//   group 2: black, above 203, above 400 and above 1000 nits pixel counts
//   group 3: above 4000 nits pixel count, max alpha, alpha-above-1.0 pixel count
// Intermediate passes write the four groups to four render targets (MRT). The last pass writes one row of the stats
// target instead: four pixels (x = group) at y = the measurement's slot.
// Counts are floats, exact up to 2^24 pixels, the largest region watched on Direct3D 9. The luminance sum is a float sum
// over the blocks (no 64-bit fixed point without integer atomics); its relative error stays far below verify's tolerance.

#define BLOCK_SIZE 8
#define SIGNAL_PQ 1
#define SIGNAL_SCRGB 2
#define FLOAT_MAX 3.402823e38f
#define LUMINANCE_SUM_CLAMP 65504.f

sampler2D input0 : register(s0);
sampler2D input1 : register(s1);
sampler2D input2 : register(s2);
sampler2D input3 : register(s3);

// xy: input texel of the first block (the slot's cell in the atlas for the first pass, 0 for a merge pass); zw: input
// texture size.
float4 input_origin_and_size : register(c0);
// xy: input pixels that belong to the region; z: 1 for the first pass (reads the region's pixels), 0 for a merge pass
// (reads the four groups of the previous level); w: signal.
float4 input_extent_pass_signal : register(c1);
// x: 1 for the last pass, which writes the stats row.
float4 output_mode : register(c2);

struct Output {
  float4 target0 : COLOR0;
  float4 target1 : COLOR1;
  float4 target2 : COLOR2;
  float4 target3 : COLOR3;
};

// Written as range checks: SM3 has no bit casts, and NaN fails both comparisons.
bool IsFinite(float value) {
  return value >= -FLOAT_MAX && value <= FLOAT_MAX;
}

float DecodePq(float encoded) {
  const float encoded_power = pow(clamp(encoded, 0.f, 1.f), 1.f / 78.84375f);
  const float linear_ratio = max(encoded_power - 0.8359375f, 0.f) / (18.8515625f - (18.6875f * encoded_power));
  return 10000.f * pow(linear_ratio, 1.f / 0.1593017578125f);
}

Output main(float2 position : VPOS) {
  const float2 pixel = floor(position);
  const bool last_pass = output_mode.x != 0.f;
  const bool first_pass = input_extent_pass_signal.z != 0.f;
  const float signal = input_extent_pass_signal.w;
  // The last pass covers four pixels of one stats row, which all reduce the single remaining block.
  const float2 block = last_pass ? float2(0.f, 0.f) : pixel;

  float4 group0 = float4(0.f, FLOAT_MAX, 0.f, 0.f);
  float4 group1 = 0.f;
  float4 group2 = 0.f;
  float4 group3 = 0.f;

  [loop]
  for (int y = 0; y < BLOCK_SIZE; ++y) {
    [loop]
    for (int x = 0; x < BLOCK_SIZE; ++x) {
      const float2 texel = (block * BLOCK_SIZE) + float2(x, y);
      if (texel.x < input_extent_pass_signal.x && texel.y < input_extent_pass_signal.y) {
        const float4 uv = float4((input_origin_and_size.xy + texel + 0.5f) / input_origin_and_size.zw, 0.f, 0.f);
        if (first_pass) {
          const float4 rgba = tex2Dlod(input0, uv);
          const float3 rgb = rgba.rgb;
          if (!IsFinite(rgb.r) || !IsFinite(rgb.g) || !IsFinite(rgb.b)) {
            group1.y += 1.f;
          } else {
            float3 display = rgb;
            float luminance = 0.f;
            if (signal == SIGNAL_PQ) {
              display = float3(DecodePq(rgb.r), DecodePq(rgb.g), DecodePq(rgb.b));
              luminance = dot(display, float3(0.2627002f, 0.6779981f, 0.0593017f));
            } else if (signal == SIGNAL_SCRGB) {
              display = 80.f * rgb;
              luminance = dot(display, float3(0.2126f, 0.7152f, 0.0722f));
            } else {
              luminance = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
            }
            const float non_negative_luminance = max(luminance, 0.f);
            group0.x = max(group0.x, non_negative_luminance);
            group0.y = min(group0.y, non_negative_luminance);
            group0.z = max(group0.z, max(max(display.r, display.g), display.b));
            group0.w += min(non_negative_luminance, LUMINANCE_SUM_CLAMP);

            group1.x += 1.f;
            group1.z += (rgb.r < 0.f || rgb.g < 0.f || rgb.b < 0.f) ? 1.f : 0.f;
            group1.w += (rgb.r > 1.f || rgb.g > 1.f || rgb.b > 1.f) ? 1.f : 0.f;
            group2.x += (rgb.r == 0.f && rgb.g == 0.f && rgb.b == 0.f) ? 1.f : 0.f;
            // Alpha as stored (formats without alpha read 1); a non-finite alpha contributes nothing.
            if (IsFinite(rgba.a)) {
              group3.y = max(group3.y, max(rgba.a, 0.f));
              group3.z += rgba.a > 1.f ? 1.f : 0.f;
            }
            if (signal == SIGNAL_PQ || signal == SIGNAL_SCRGB) {
              group2.y += luminance > 203.f ? 1.f : 0.f;
              group2.z += luminance > 400.f ? 1.f : 0.f;
              group2.w += luminance > 1000.f ? 1.f : 0.f;
              group3.x += luminance > 4000.f ? 1.f : 0.f;
            }
          }
        } else {
          const float4 merged0 = tex2Dlod(input0, uv);
          group0.x = max(group0.x, merged0.x);
          group0.y = min(group0.y, merged0.y);
          group0.z = max(group0.z, merged0.z);
          group0.w += merged0.w;
          group1 += tex2Dlod(input1, uv);
          group2 += tex2Dlod(input2, uv);
          const float4 merged3 = tex2Dlod(input3, uv);
          group3.x += merged3.x;
          group3.y = max(group3.y, merged3.y);
          group3.z += merged3.z;
        }
      }
    }
  }

  Output output;
  output.target0 = group0;
  output.target1 = group1;
  output.target2 = group2;
  output.target3 = group3;
  if (last_pass) {
    if (pixel.x >= 3.f) {
      output.target0 = group3;
    } else if (pixel.x >= 2.f) {
      output.target0 = group2;
    } else if (pixel.x >= 1.f) {
      output.target0 = group1;
    }
  }
  return output;
}
