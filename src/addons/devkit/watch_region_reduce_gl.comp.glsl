#version 430
// Reduces the measured slots of a devkit_watch_region atlas into one row of the stats image per slot (OpenGL).
// Keep in sync with watch_region_reduce.cs_5_x.hlsl: same statistics, same constants, same stats word layout.
//
// The atlas is a grid of region-sized cells, `slot` = cell index (row-major, ATLAS_COLUMNS per row). One dispatch covers
// every slot: gl_WorkGroupID.z is the slot. The stats image holds STATS_STRIDE uints per slot, starts at 0 and is merged
// with atomics, so a minimum is stored as the bitwise inverse of the float bits and merged with a maximum.
//
// The host inserts `#define` lines right after the #version line (see the defaults below, which also make this file
// compile on its own for offline validation): REGION_WIDTH, REGION_HEIGHT, ATLAS_COLUMNS, SIGNAL (0 raw, 1 PQ, 2 scRGB),
// SRGB_DECODE (1 when the watched texture is an sRGB format), ATLAS_FORMAT (image format qualifier of the atlas),
// ATLAS_UNIT and STATS_UNIT (image units).

#ifndef REGION_WIDTH
#define REGION_WIDTH 16
#endif
#ifndef REGION_HEIGHT
#define REGION_HEIGHT 16
#endif
#ifndef ATLAS_COLUMNS
#define ATLAS_COLUMNS 1
#endif
#ifndef SIGNAL
#define SIGNAL 0
#endif
#ifndef SRGB_DECODE
#define SRGB_DECODE 0
#endif
#ifndef ATLAS_FORMAT
#define ATLAS_FORMAT rgba8
#endif
#ifndef ATLAS_UNIT
#define ATLAS_UNIT 0
#endif
#ifndef STATS_UNIT
#define STATS_UNIT 1
#endif

#define GROUP_SIZE 16
#define GROUP_THREADS (GROUP_SIZE * GROUP_SIZE)

#define STATS_NON_FINITE_COUNT 0
#define STATS_MAX_LUMINANCE_BITS 1
#define STATS_MIN_LUMINANCE_INVERTED_BITS 2
#define STATS_MAX_CHANNEL_BITS 3
#define STATS_LUMINANCE_SUM_LOW 4
#define STATS_LUMINANCE_SUM_HIGH 5
#define STATS_NEGATIVE_COUNT 6
#define STATS_ABOVE_ONE_COUNT 7
#define STATS_ABOVE_203_COUNT 8
#define STATS_ABOVE_400_COUNT 9
#define STATS_ABOVE_1000_COUNT 10
#define STATS_ABOVE_4000_COUNT 11
#define STATS_FINITE_COUNT 12
#define STATS_BLACK_COUNT 13
#define STATS_MAX_ALPHA_BITS 14
#define STATS_ALPHA_ABOVE_ONE_COUNT 15

#define SIGNAL_RAW 0
#define SIGNAL_PQ 1
#define SIGNAL_SCRGB 2

// The luminance sum is reduced in float inside a group, then added to a 64-bit fixed-point total (low and high words,
// value * 2^20). Rounding once per group, not per pixel, keeps the mean exact for dark 8-bit content. Per-pixel values
// are clamped to the float16 maximum so the total stays far below 2^64 units.
#define LUMINANCE_SUM_SCALE 1048576.0
#define LUMINANCE_SUM_CLAMP 65504.0
#define TWO_POW_32 4294967296.0
// Largest float below 2^32: converting a float of 2^32 or more to uint is undefined in GLSL (HLSL saturates).
#define MAX_FLOAT_BELOW_TWO_POW_32 4294967040.0

// Counter slots in the group-shared reduction.
#define COUNT_NON_FINITE 0
#define COUNT_NEGATIVE 1
#define COUNT_ABOVE_ONE 2
#define COUNT_ABOVE_203 3
#define COUNT_ABOVE_400 4
#define COUNT_ABOVE_1000 5
#define COUNT_ABOVE_4000 6
#define COUNT_FINITE 7
#define COUNT_BLACK 8
#define COUNT_ALPHA_ABOVE_ONE 9
#define COUNT_KINDS 10

layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) in;

layout(binding = ATLAS_UNIT, ATLAS_FORMAT) uniform readonly image2D atlas;
layout(binding = STATS_UNIT, r32ui) uniform coherent uimage2D stats;

shared uint shared_max_luminance[GROUP_THREADS];
shared uint shared_min_luminance_inverted[GROUP_THREADS];
shared uint shared_max_channel[GROUP_THREADS];
shared uint shared_max_alpha[GROUP_THREADS];
shared float shared_luminance_sum[GROUP_THREADS];
shared uint shared_counts[COUNT_KINDS][GROUP_THREADS];

bool IsNonFinite(float value) {
  return (floatBitsToUint(value) & 0x7F800000u) == 0x7F800000u;
}

float DecodePq(float encoded) {
  float encoded_power = pow(clamp(encoded, 0.0, 1.0), 1.0 / 78.84375);
  float linear_ratio = max(encoded_power - 0.8359375, 0.0) / (18.8515625 - (18.6875 * encoded_power));
  return 10000.0 * pow(linear_ratio, 1.0 / 0.1593017578125);
}

// The sRGB transfer function of an sRGB texture, applied to what D3D11 would get from an sRGB shader resource view.
float DecodeSrgb(float encoded) {
  return (encoded <= 0.04045) ? (encoded / 12.92) : pow((encoded + 0.055) / 1.055, 2.4);
}

void main() {
  uint group_index = gl_LocalInvocationIndex;
  uint slot = gl_WorkGroupID.z;

  uint max_luminance_bits = 0u;
  uint min_luminance_inverted_bits = 0u;
  uint max_channel_bits = 0u;
  uint max_alpha_bits = 0u;
  float luminance_sum = 0.0;
  uint counts[COUNT_KINDS] = uint[COUNT_KINDS](0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);

  if (gl_GlobalInvocationID.x < uint(REGION_WIDTH) && gl_GlobalInvocationID.y < uint(REGION_HEIGHT)) {
    ivec2 cell_origin = ivec2(int(slot % uint(ATLAS_COLUMNS)) * REGION_WIDTH, int(slot / uint(ATLAS_COLUMNS)) * REGION_HEIGHT);
    vec4 rgba = imageLoad(atlas, cell_origin + ivec2(gl_GlobalInvocationID.xy));
    vec3 rgb = rgba.rgb;
#if SRGB_DECODE
    rgb = vec3(DecodeSrgb(rgb.r), DecodeSrgb(rgb.g), DecodeSrgb(rgb.b));
#endif

    if (IsNonFinite(rgb.r) || IsNonFinite(rgb.g) || IsNonFinite(rgb.b)) {
      counts[COUNT_NON_FINITE] = 1u;
    } else {
      vec3 display = rgb;
      float luminance = 0.0;
#if SIGNAL == SIGNAL_PQ
      display = vec3(DecodePq(rgb.r), DecodePq(rgb.g), DecodePq(rgb.b));
      luminance = dot(display, vec3(0.2627002, 0.6779981, 0.0593017));
#elif SIGNAL == SIGNAL_SCRGB
      display = 80.0 * rgb;
      luminance = dot(display, vec3(0.2126, 0.7152, 0.0722));
#else
      luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
#endif
      // Float bits only order correctly for non-negative values; negatives are counted separately.
      // Compare instead of max() so a -0.0 result can never contribute its sign bit.
      float non_negative_luminance = (luminance > 0.0) ? luminance : 0.0;
      float brightest_channel = max(max(display.r, display.g), display.b);
      float max_channel = (brightest_channel > 0.0) ? brightest_channel : 0.0;

      max_luminance_bits = floatBitsToUint(non_negative_luminance);
      min_luminance_inverted_bits = ~floatBitsToUint(non_negative_luminance);
      max_channel_bits = floatBitsToUint(max_channel);
      luminance_sum = min(non_negative_luminance, LUMINANCE_SUM_CLAMP);

      counts[COUNT_FINITE] = 1u;
      counts[COUNT_NEGATIVE] = (rgb.r < 0.0 || rgb.g < 0.0 || rgb.b < 0.0) ? 1u : 0u;
      counts[COUNT_ABOVE_ONE] = (rgb.r > 1.0 || rgb.g > 1.0 || rgb.b > 1.0) ? 1u : 0u;
      counts[COUNT_BLACK] = (rgb.r == 0.0 && rgb.g == 0.0 && rgb.b == 0.0) ? 1u : 0u;
      // Alpha as stored (formats without alpha read 1); a non-finite alpha contributes nothing.
      if (!IsNonFinite(rgba.a)) {
        max_alpha_bits = floatBitsToUint((rgba.a > 0.0) ? rgba.a : 0.0);
        counts[COUNT_ALPHA_ABOVE_ONE] = (rgba.a > 1.0) ? 1u : 0u;
      }
#if SIGNAL != SIGNAL_RAW
      counts[COUNT_ABOVE_203] = luminance > 203.0 ? 1u : 0u;
      counts[COUNT_ABOVE_400] = luminance > 400.0 ? 1u : 0u;
      counts[COUNT_ABOVE_1000] = luminance > 1000.0 ? 1u : 0u;
      counts[COUNT_ABOVE_4000] = luminance > 4000.0 ? 1u : 0u;
#endif
    }
  }

  shared_max_luminance[group_index] = max_luminance_bits;
  shared_min_luminance_inverted[group_index] = min_luminance_inverted_bits;
  shared_max_channel[group_index] = max_channel_bits;
  shared_max_alpha[group_index] = max_alpha_bits;
  shared_luminance_sum[group_index] = luminance_sum;
  for (uint count_index = 0u; count_index < uint(COUNT_KINDS); ++count_index) {
    shared_counts[count_index][group_index] = counts[count_index];
  }
  memoryBarrierShared();
  barrier();

  // log2(GROUP_THREADS) halving steps.
  for (uint reduction_step = 1u; reduction_step <= 8u; ++reduction_step) {
    uint stride = uint(GROUP_THREADS) >> reduction_step;
    if (group_index < stride) {
      shared_max_luminance[group_index] = max(shared_max_luminance[group_index], shared_max_luminance[group_index + stride]);
      shared_min_luminance_inverted[group_index] = max(shared_min_luminance_inverted[group_index], shared_min_luminance_inverted[group_index + stride]);
      shared_max_channel[group_index] = max(shared_max_channel[group_index], shared_max_channel[group_index + stride]);
      shared_max_alpha[group_index] = max(shared_max_alpha[group_index], shared_max_alpha[group_index + stride]);
      shared_luminance_sum[group_index] += shared_luminance_sum[group_index + stride];
      for (uint count_index = 0u; count_index < uint(COUNT_KINDS); ++count_index) {
        shared_counts[count_index][group_index] += shared_counts[count_index][group_index + stride];
      }
    }
    memoryBarrierShared();
    barrier();
  }

  if (group_index != 0u) return;

  // Every group touches the slot's non-finite count only when it has something to add, to keep atomic traffic down.
  if (shared_counts[COUNT_NON_FINITE][0] != 0u) {
    imageAtomicAdd(stats, ivec2(STATS_NON_FINITE_COUNT, int(slot)), shared_counts[COUNT_NON_FINITE][0]);
  }
  if (shared_counts[COUNT_FINITE][0] == 0u) return;

  imageAtomicMax(stats, ivec2(STATS_MAX_LUMINANCE_BITS, int(slot)), shared_max_luminance[0]);
  imageAtomicMax(stats, ivec2(STATS_MIN_LUMINANCE_INVERTED_BITS, int(slot)), shared_min_luminance_inverted[0]);
  imageAtomicMax(stats, ivec2(STATS_MAX_CHANNEL_BITS, int(slot)), shared_max_channel[0]);
  imageAtomicMax(stats, ivec2(STATS_MAX_ALPHA_BITS, int(slot)), shared_max_alpha[0]);

  // 64-bit fixed-point sum: split the scaled group sum into high and low words, add the low word and carry into the high
  // word when it wraps.
  float scaled_group_sum = shared_luminance_sum[0] * LUMINANCE_SUM_SCALE;
  uint scaled_high = uint(floor(scaled_group_sum / TWO_POW_32));
  uint scaled_low = uint(clamp(scaled_group_sum - (float(scaled_high) * TWO_POW_32) + 0.5, 0.0, MAX_FLOAT_BELOW_TWO_POW_32));
  uint previous_low = imageAtomicAdd(stats, ivec2(STATS_LUMINANCE_SUM_LOW, int(slot)), scaled_low);
  uint carry = (previous_low + scaled_low < previous_low) ? 1u : 0u;
  if (scaled_high + carry != 0u) {
    imageAtomicAdd(stats, ivec2(STATS_LUMINANCE_SUM_HIGH, int(slot)), scaled_high + carry);
  }

  imageAtomicAdd(stats, ivec2(STATS_FINITE_COUNT, int(slot)), shared_counts[COUNT_FINITE][0]);
  imageAtomicAdd(stats, ivec2(STATS_NEGATIVE_COUNT, int(slot)), shared_counts[COUNT_NEGATIVE][0]);
  imageAtomicAdd(stats, ivec2(STATS_ABOVE_ONE_COUNT, int(slot)), shared_counts[COUNT_ABOVE_ONE][0]);
  imageAtomicAdd(stats, ivec2(STATS_BLACK_COUNT, int(slot)), shared_counts[COUNT_BLACK][0]);
  imageAtomicAdd(stats, ivec2(STATS_ABOVE_203_COUNT, int(slot)), shared_counts[COUNT_ABOVE_203][0]);
  imageAtomicAdd(stats, ivec2(STATS_ABOVE_400_COUNT, int(slot)), shared_counts[COUNT_ABOVE_400][0]);
  imageAtomicAdd(stats, ivec2(STATS_ABOVE_1000_COUNT, int(slot)), shared_counts[COUNT_ABOVE_1000][0]);
  imageAtomicAdd(stats, ivec2(STATS_ABOVE_4000_COUNT, int(slot)), shared_counts[COUNT_ABOVE_4000][0]);
  imageAtomicAdd(stats, ivec2(STATS_ALPHA_ABOVE_ONE_COUNT, int(slot)), shared_counts[COUNT_ALPHA_ABOVE_ONE][0]);
}
