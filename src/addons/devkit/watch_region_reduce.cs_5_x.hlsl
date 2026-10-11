// Reduces one measured region into a slot of the stats texture (devkit_watch_region).
// The source is a scratch copy of exactly the watched region, so (0, 0) is the region origin.
// Output: one row of STATS_STRIDE uints per slot. All fields start at 0 (the texture is cleared with 0)
// and are combined with atomics, so a minimum is stored as the bitwise inverse of the float bits and merged with Max.

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

#define SIGNAL_RAW 0
#define SIGNAL_PQ 1
#define SIGNAL_SCRGB 2

// Luminance sum is accumulated as fixed point (value * 256); a per-pixel luminance is clamped so a group sum fits in 32 bits.
#define LUMINANCE_SUM_SCALE 256.0f
#define LUMINANCE_SUM_CLAMP 32768.0f

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
#define COUNT_KINDS 9

Texture2D<float4> source : register(t0);
RWTexture2D<uint> stats : register(u0);

cbuffer Constants : register(b0) {
  uint region_width;
  uint region_height;
  uint signal;
  uint slot;
};

groupshared uint shared_max_luminance[GROUP_THREADS];
groupshared uint shared_min_luminance_inverted[GROUP_THREADS];
groupshared uint shared_max_channel[GROUP_THREADS];
groupshared uint shared_luminance_sum[GROUP_THREADS];
groupshared uint shared_counts[COUNT_KINDS][GROUP_THREADS];

bool IsNonFinite(float value) {
  return (asuint(value) & 0x7F800000u) == 0x7F800000u;
}

float DecodePq(float encoded) {
  const float encoded_power = pow(clamp(encoded, 0.f, 1.f), 1.f / 78.84375f);
  const float linear_ratio = max(encoded_power - 0.8359375f, 0.f) / (18.8515625f - (18.6875f * encoded_power));
  return 10000.f * pow(linear_ratio, 1.f / 0.1593017578125f);
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
  uint max_luminance_bits = 0u;
  uint min_luminance_inverted_bits = 0u;
  uint max_channel_bits = 0u;
  uint luminance_sum = 0u;
  uint counts[COUNT_KINDS] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

  if (dispatch_thread_id.x < region_width && dispatch_thread_id.y < region_height) {
    const float3 rgb = source.Load(int3(dispatch_thread_id.xy, 0)).rgb;

    if (IsNonFinite(rgb.r) || IsNonFinite(rgb.g) || IsNonFinite(rgb.b)) {
      counts[COUNT_NON_FINITE] = 1u;
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
      // Float bits only order correctly for non-negative values; negatives are counted separately.
      // Compare instead of max() so a -0.0 result can never contribute its sign bit.
      const float non_negative_luminance = (luminance > 0.f) ? luminance : 0.f;
      const float brightest_channel = max(max(display.r, display.g), display.b);
      const float max_channel = (brightest_channel > 0.f) ? brightest_channel : 0.f;

      max_luminance_bits = asuint(non_negative_luminance);
      min_luminance_inverted_bits = ~asuint(non_negative_luminance);
      max_channel_bits = asuint(max_channel);
      luminance_sum = (uint)(min(non_negative_luminance, LUMINANCE_SUM_CLAMP) * LUMINANCE_SUM_SCALE + 0.5f);

      counts[COUNT_FINITE] = 1u;
      counts[COUNT_NEGATIVE] = (rgb.r < 0.f || rgb.g < 0.f || rgb.b < 0.f) ? 1u : 0u;
      counts[COUNT_ABOVE_ONE] = (rgb.r > 1.f || rgb.g > 1.f || rgb.b > 1.f) ? 1u : 0u;
      counts[COUNT_BLACK] = (rgb.r == 0.f && rgb.g == 0.f && rgb.b == 0.f) ? 1u : 0u;
      if (signal != SIGNAL_RAW) {
        counts[COUNT_ABOVE_203] = luminance > 203.f ? 1u : 0u;
        counts[COUNT_ABOVE_400] = luminance > 400.f ? 1u : 0u;
        counts[COUNT_ABOVE_1000] = luminance > 1000.f ? 1u : 0u;
        counts[COUNT_ABOVE_4000] = luminance > 4000.f ? 1u : 0u;
      }
    }
  }

  shared_max_luminance[group_index] = max_luminance_bits;
  shared_min_luminance_inverted[group_index] = min_luminance_inverted_bits;
  shared_max_channel[group_index] = max_channel_bits;
  shared_luminance_sum[group_index] = luminance_sum;
  [unroll]
  for (uint count_index = 0u; count_index < COUNT_KINDS; ++count_index) {
    shared_counts[count_index][group_index] = counts[count_index];
  }
  GroupMemoryBarrierWithGroupSync();

  // log2(GROUP_THREADS) halving steps; a fixed trip count keeps the loop unrollable for FXC.
  [unroll]
  for (uint reduction_step = 1u; reduction_step <= 8u; ++reduction_step) {
    const uint stride = GROUP_THREADS >> reduction_step;
    if (group_index < stride) {
      shared_max_luminance[group_index] = max(shared_max_luminance[group_index], shared_max_luminance[group_index + stride]);
      shared_min_luminance_inverted[group_index] = max(shared_min_luminance_inverted[group_index], shared_min_luminance_inverted[group_index + stride]);
      shared_max_channel[group_index] = max(shared_max_channel[group_index], shared_max_channel[group_index + stride]);
      shared_luminance_sum[group_index] += shared_luminance_sum[group_index + stride];
      [unroll]
      for (uint count_index = 0u; count_index < COUNT_KINDS; ++count_index) {
        shared_counts[count_index][group_index] += shared_counts[count_index][group_index + stride];
      }
    }
    GroupMemoryBarrierWithGroupSync();
  }

  if (group_index != 0u) return;

  // Every group touches the slot's non-finite count only when it has something to add, to keep atomic traffic down.
  if (shared_counts[COUNT_NON_FINITE][0] != 0u) {
    InterlockedAdd(stats[uint2(STATS_NON_FINITE_COUNT, slot)], shared_counts[COUNT_NON_FINITE][0]);
  }
  if (shared_counts[COUNT_FINITE][0] == 0u) return;

  InterlockedMax(stats[uint2(STATS_MAX_LUMINANCE_BITS, slot)], shared_max_luminance[0]);
  InterlockedMax(stats[uint2(STATS_MIN_LUMINANCE_INVERTED_BITS, slot)], shared_min_luminance_inverted[0]);
  InterlockedMax(stats[uint2(STATS_MAX_CHANNEL_BITS, slot)], shared_max_channel[0]);

  // 64-bit sum: add into the low word and carry into the high word when the low word wraps.
  const uint group_sum = shared_luminance_sum[0];
  uint previous_low = 0u;
  InterlockedAdd(stats[uint2(STATS_LUMINANCE_SUM_LOW, slot)], group_sum, previous_low);
  if (previous_low + group_sum < previous_low) {
    InterlockedAdd(stats[uint2(STATS_LUMINANCE_SUM_HIGH, slot)], 1u);
  }

  InterlockedAdd(stats[uint2(STATS_FINITE_COUNT, slot)], shared_counts[COUNT_FINITE][0]);
  InterlockedAdd(stats[uint2(STATS_NEGATIVE_COUNT, slot)], shared_counts[COUNT_NEGATIVE][0]);
  InterlockedAdd(stats[uint2(STATS_ABOVE_ONE_COUNT, slot)], shared_counts[COUNT_ABOVE_ONE][0]);
  InterlockedAdd(stats[uint2(STATS_BLACK_COUNT, slot)], shared_counts[COUNT_BLACK][0]);
  InterlockedAdd(stats[uint2(STATS_ABOVE_203_COUNT, slot)], shared_counts[COUNT_ABOVE_203][0]);
  InterlockedAdd(stats[uint2(STATS_ABOVE_400_COUNT, slot)], shared_counts[COUNT_ABOVE_400][0]);
  InterlockedAdd(stats[uint2(STATS_ABOVE_1000_COUNT, slot)], shared_counts[COUNT_ABOVE_1000][0]);
  InterlockedAdd(stats[uint2(STATS_ABOVE_4000_COUNT, slot)], shared_counts[COUNT_ABOVE_4000][0]);
}
