/*
 * Copyright (C) 2026 Vulta93
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <include/reshade.hpp>

#include "../../../utils/bitwise.hpp"
#include "../../../utils/data.hpp"
#include "../../../utils/descriptor.hpp"
#include "../../../utils/gpu_fence.hpp"
#include "../../../utils/mcp/arguments.hpp"
#include "../../../utils/mcp/types.hpp"
#include "../../../utils/pipeline_layout.hpp"
#include "../../../utils/render.hpp"
#include "../../../utils/state.hpp"
#include "../../../utils/swapchain.hpp"
#include "../formatting.hpp"
#include "./frame_overview.hpp"
#include "./resource_analysis.hpp"
#include "./resource_handle.hpp"
#include "./shader_hash.hpp"
#include "./watch_region_opengl.hpp"

// devkit_watch_region: GPU region statistics around every command that writes one texture.
//
// The state of the watched texture after command N equals its state before the next command that writes it, so the
// tool only measures *before* each writer and once more at the end of the frame. The end of a frame is ReShade's
// reshade_present event, which runs after every add-on's present handler (for example a mod's swap chain proxy pass),
// so the final measurement does not depend on add-on load order.
//
// D3D11 measures in place on the immediate context: the region is copied into a scratch texture and reduced right away
// by a compute shader into one row of an R32_UINT stats texture.
//
// D3D12 and Vulkan never change the game's pipeline, descriptor or constant bindings: at a measurement point the
// region is only copied into a pooled scratch texture (with a per-subresource barrier from the exactly known state of
// the watched texture to copy source and back). The copies are ordered by submission (execute_command_list), and all
// reductions run at the end of the frame on ReShade's immediate command list of the presenting queue.
//
// D3D9 has no compute shaders or UAVs. A measurement point only copies the region (StretchRect) into its slot of one
// atlas texture, without touching the game's state. At the end of the frame a pixel shader reduces every used slot in a
// few passes (8x8 pixels per output pixel) into one row of a float stats target. Needs D3D9Ex for the readback fence.
//
// OpenGL records in execution order on one context like D3D11, but has no cheap state-free in-place reduction either: a
// measurement point copies the region (ReShade's copy_texture_region, glCopyImageSubData) into its cell of a grid atlas,
// and one compute dispatch at the end of the frame reduces every cell into one row of an r32ui stats image (the same word
// layout as D3D11/D3D12/Vulkan). The writers are found by asking GL for the draw framebuffer and image units, because
// ReShade raises no event for glDrawBuffers or DSA attachment changes. The readback fence is a polled GL sync object
// (OpenGLSyncTimeline). Only events on the context that presents the swap chain are measured.
//
// The stats texture is copied to a small readback ring at the end of every frame and decoded two frames later, so a
// long watch does not stall the GPU every frame.
namespace renodx::addons::devkit::mcp::watch_region {

using json = renodx::utils::mcp::json;
using ToolResult = renodx::utils::mcp::ToolResult;
using Signal = resource_analysis::Signal;
namespace mcp_arguments = renodx::utils::mcp::arguments;

constexpr std::uint32_t STATS_STRIDE = 16u;
constexpr std::uint32_t MAX_SLOTS = 1024u;
constexpr std::uint32_t READBACK_RING_SIZE = 3u;
constexpr std::uint32_t GROUP_SIZE = 16u;
constexpr std::uint32_t MAX_FRAMES = 3600u;
// UAV slots scanned for writers: compute UAVs, and D3D11 pixel-shader UAVs (the D3D11.0 limit; D3D11.1 slots 8-63
// are not scanned).
constexpr std::uint32_t MAX_UAV_SLOTS = 8u;
// D3D12/Vulkan: default and maximum VRAM for the region copies taken inside the game's command lists.
constexpr std::uint32_t DEFAULT_SCRATCH_BUDGET_MB = 512u;
constexpr std::uint32_t MAX_SCRATCH_BUDGET_MB = 8192u;
constexpr std::uint32_t MAX_SCRATCH_ENTRIES = 4096u;
// D3D12/Vulkan scratch textures created when a watch starts; the pool then grows at frame ends as needed.
constexpr std::uint32_t INITIAL_SCRATCH_ENTRIES = 16u;
// D3D12/Vulkan: extra frames a watch may run to measure the state after the last writer of its last frame.
constexpr std::uint32_t MAX_CLOSING_FRAMES = 8u;
// D3D12: while cross-queue points still wait for the game queue's lifetime fence (the GPU can run several frames behind
// the CPU), closing goes on past MAX_CLOSING_FRAMES until they are reduced or this much time passed; the rest are
// reported as cross_queue_not_ready. This only bounds the watch's duration; fence values never depend on time.
constexpr auto CROSS_QUEUE_CLOSING_TIMEOUT = std::chrono::seconds(1);
constexpr auto LOCK_WAIT_REPORT_TIME = std::chrono::seconds(2);
// D3D12/Vulkan: every CPU wait for the GPU in the watch (lifetime fences at the end of a watch and when a queue or the
// device is destroyed, the stats readback fence) and the wait for a frame end's signals in flight on a destroyed queue.
constexpr auto LIFETIME_FENCE_WAIT_TIMEOUT = std::chrono::seconds(2);
// Descriptor table ranges are scanned up to this many descriptors when looking for the watched texture as a UAV.
constexpr std::uint32_t MAX_SCANNED_TABLE_DESCRIPTORS = 256u;
constexpr std::size_t MAX_RENDER_PASS_MEMBERS = 64u;
constexpr std::size_t MAX_REPORTED_FRAMES = 100u;

// Word offsets inside one stats row. Keep in sync with watch_region_reduce.cs_5_x.hlsl.
constexpr std::uint32_t STATS_NON_FINITE_COUNT = 0u;
constexpr std::uint32_t STATS_MAX_LUMINANCE_BITS = 1u;
constexpr std::uint32_t STATS_MIN_LUMINANCE_INVERTED_BITS = 2u;
constexpr std::uint32_t STATS_MAX_CHANNEL_BITS = 3u;
constexpr std::uint32_t STATS_LUMINANCE_SUM_LOW = 4u;
constexpr std::uint32_t STATS_LUMINANCE_SUM_HIGH = 5u;
constexpr std::uint32_t STATS_NEGATIVE_COUNT = 6u;
constexpr std::uint32_t STATS_ABOVE_ONE_COUNT = 7u;
constexpr std::uint32_t STATS_ABOVE_203_COUNT = 8u;
constexpr std::uint32_t STATS_ABOVE_400_COUNT = 9u;
constexpr std::uint32_t STATS_ABOVE_1000_COUNT = 10u;
constexpr std::uint32_t STATS_ABOVE_4000_COUNT = 11u;
constexpr std::uint32_t STATS_FINITE_COUNT = 12u;
constexpr std::uint32_t STATS_BLACK_COUNT = 13u;
constexpr std::uint32_t STATS_MAX_ALPHA_BITS = 14u;
constexpr std::uint32_t STATS_ALPHA_ABOVE_ONE_COUNT = 15u;
// D3D9: float positions of the same statistics in one row of the stats target (4 pixels of 4 floats). Keep in sync with
// watch_region_reduce_dx9.ps_3_0.hlsl. The row is converted to the word layout above after the readback.
constexpr std::uint32_t D3D9_STATS_MAX_LUMINANCE = 0u;
constexpr std::uint32_t D3D9_STATS_MIN_LUMINANCE = 1u;
constexpr std::uint32_t D3D9_STATS_MAX_CHANNEL = 2u;
constexpr std::uint32_t D3D9_STATS_LUMINANCE_SUM = 3u;
constexpr std::uint32_t D3D9_STATS_FINITE_COUNT = 4u;
constexpr std::uint32_t D3D9_STATS_NON_FINITE_COUNT = 5u;
constexpr std::uint32_t D3D9_STATS_NEGATIVE_COUNT = 6u;
constexpr std::uint32_t D3D9_STATS_ABOVE_ONE_COUNT = 7u;
constexpr std::uint32_t D3D9_STATS_BLACK_COUNT = 8u;
constexpr std::uint32_t D3D9_STATS_ABOVE_203_COUNT = 9u;
constexpr std::uint32_t D3D9_STATS_ABOVE_400_COUNT = 10u;
constexpr std::uint32_t D3D9_STATS_ABOVE_1000_COUNT = 11u;
constexpr std::uint32_t D3D9_STATS_ABOVE_4000_COUNT = 12u;
constexpr std::uint32_t D3D9_STATS_MAX_ALPHA = 13u;
constexpr std::uint32_t D3D9_STATS_ALPHA_ABOVE_ONE_COUNT = 14u;
// D3D9: the four float4 groups of a row, written to four render targets by the intermediate reduction passes.
constexpr std::uint32_t D3D9_STATS_GROUPS = 4u;
constexpr std::uint32_t D3D9_REDUCTION_BLOCK_SIZE = 8u;
// D3D9: pixel counts are floats in the reduction, exact up to 2^24.
constexpr std::uint64_t D3D9_MAX_REGION_PIXELS = 1u << 24u;
constexpr double LUMINANCE_SUM_SCALE = 1048576.0;
// Per-pixel luminance cap applied to the sum (float16 maximum), mirrored by the CPU verification.
constexpr double LUMINANCE_SUM_CLAMP = 65504.0;
// verify: GPU float math against CPU double math on the same pixels.
constexpr double VERIFY_RELATIVE_TOLERANCE = 1e-4;
constexpr double VERIFY_ABSOLUTE_TOLERANCE = 1e-7;

// Relative change below which two peaks count as equal.
constexpr double PEAK_TOLERANCE = 1e-4;
// Peaks closer than this share a bucket when looking for a repeated ceiling value (about 0.1%).
constexpr double CEILING_BUCKETS_PER_DECADE = 2000.0;

enum class WriterKind : std::uint8_t {
  DRAW = 0,
  DISPATCH = 1,
  COPY = 2,
  CLEAR = 3,
  RESOLVE = 4,
  UPDATE = 5,
  // D3D12 render passes and Vulkan render pass instances (one per subpass): the draws inside are its members.
  RENDER_PASS = 6,
  // A D3D12 bundle executed while the watched texture is a render target.
  BUNDLE = 7,
};

// Why the state before a writer (or after it) was not measured. Every case is reported in the tool output.
enum class UnmeasuredReason : std::uint8_t {
  NONE = 0,
  SLOT_LIMIT = 1,
  SCRATCH_BUDGET = 2,
  STATE_UNKNOWN = 3,
  INSIDE_RENDER_PASS = 4,
  OTHER_QUEUE = 5,
  RESUBMITTED = 6,
  UNTRACKED_COMMAND_LIST = 7,
  BUNDLE = 8,
  NO_LATER_MEASUREMENT = 9,
  SCRATCH_POOL_EMPTY = 10,
  // A dispatch that can reach the texture only through a bound descriptor table: bindless engines keep most
  // textures in one table, so this does not prove a write, and the texture's state is unknown.
  DESCRIPTOR_TABLE = 11,
  // D3D12: a cross-queue point whose copy the GPU had not finished on the game's queue (lifetime fence not passed) when
  // the watch ended, or one taken while closing that no writer needed.
  CROSS_QUEUE_NOT_READY = 12,
};

struct Request {
  reshade::api::resource_view resource_view = {0u};
  bool prefer_clone = false;
  std::optional<reshade::api::subresource_box> region = std::nullopt;
  Signal signal = Signal::RAW;
  std::uint32_t frames = 1u;
  std::optional<std::uint32_t> shader_hash = std::nullopt;
  bool include_unchanged = false;
  bool verify = false;
  std::uint32_t scratch_budget_mb = DEFAULT_SCRATCH_BUDGET_MB;
  // withSnapshot: a snapshot is captured in the watched frame and each traced writer is linked to its snapshot command.
  bool with_snapshot = false;
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false;
  bool canceled = false;
  bool completed = false;
  std::optional<ToolResult> result = std::nullopt;
  // Why the writers could not be linked to a snapshot (empty when they were).
  std::string snapshot_link_error;
  // The snapshot begin_linked_snapshot started for this watch.
  std::optional<std::uint64_t> linked_snapshot_id = std::nullopt;
};

struct ToolContext {
  std::function<std::uint32_t(const json& arguments)> resolve_device_index;
  std::function<bool(std::uint32_t device_index, const std::shared_ptr<Request>& request)> enqueue_request;
  std::function<void(std::uint32_t device_index, const std::shared_ptr<Request>& request)> cancel_request;
  // The snapshot's commands as devkit_frame_overview sees them (withSnapshot).
  std::function<frame_overview::Input(std::uint32_t device_index)> collect_snapshot_commands;
};

// A snapshot command by the key the snapshot gives it while recording: the command list, its recording and the command
// number in it.
// The snapshot's draw index is only known once the snapshot frame ended and its list was sorted.
struct SnapshotCommandKey {
  std::uint64_t cmd_list_handle = 0u;
  std::uint32_t cmd_list_draw_index = 0u;
  // Which recording of the command list: D3D12/Vulkan lists restart their numbers when reset within one frame.
  std::uint64_t cmd_list_recording = 0u;
};

struct DrawMember {
  std::uint32_t command_index = 0u;
  std::uint32_t pixel_shader_hash = 0u;
  std::uint32_t vertex_shader_hash = 0u;
  std::optional<SnapshotCommandKey> snapshot_command = std::nullopt;
};

struct WriterInfo {
  WriterKind kind = WriterKind::DRAW;
  // D3D11: ordinal in the frame. D3D12/Vulkan: ordinal in its command list until committed, then in the frame.
  std::uint32_t command_index = 0u;
  std::uint32_t pixel_shader_hash = 0u;
  std::uint32_t vertex_shader_hash = 0u;
  std::uint32_t compute_shader_hash = 0u;
  std::vector<DrawMember> members = {};
  std::uint32_t member_count = 0u;
  std::optional<SnapshotCommandKey> snapshot_command = std::nullopt;
};

// One command that writes the watched subresource, or a pure measurement point (no writer), in command order.
struct Record {
  std::optional<WriterInfo> writer = std::nullopt;
  // D3D11: stats slot of the measurement taken before the command. D3D12/Vulkan: scratch entry holding the copy.
  std::optional<std::uint32_t> measurement = std::nullopt;
  // Why no measurement was taken before this writer.
  UnmeasuredReason reason = UnmeasuredReason::NONE;
  // D3D12/Vulkan: the watched image the record is about (one of the swap chain's images when the watch follows it).
  std::uint64_t image = 0u;
};

// What a copy, clear, resolve or update writes. `subresource` is nullopt when every subresource is written
// (copy_resource). Clears name the view and the state the view must be in for the clear.
struct WrittenTarget {
  reshade::api::resource resource = {0u};
  std::optional<std::uint32_t> subresource = std::nullopt;
  reshade::api::resource_view view = {0u};
  std::optional<reshade::api::resource_usage> state = std::nullopt;
};

struct RegionStats {
  std::array<std::uint32_t, STATS_STRIDE> raw = {};
  std::uint32_t finite_count = 0u;
  std::uint32_t non_finite_count = 0u;
  std::uint32_t negative_count = 0u;
  std::uint32_t above_one_count = 0u;
  std::uint32_t black_count = 0u;
  std::array<std::uint32_t, 4> above_nits_counts = {};
  // Alpha as stored, of the finite pixels (formats without alpha read 1).
  std::uint32_t alpha_above_one_count = 0u;
  double max_luminance = 0.0;
  double min_luminance = 0.0;
  double mean_luminance = 0.0;
  double max_channel = 0.0;
  double max_alpha = 0.0;
};

// A committed measurement, identified by its position in commit (submission) order.
struct Point {
  std::uint32_t frame_index = 0u;
  std::optional<std::uint32_t> slot = std::nullopt;
  std::optional<std::uint32_t> scratch_entry = std::nullopt;
  UnmeasuredReason refused = UnmeasuredReason::NONE;
  std::optional<RegionStats> stats = std::nullopt;
  bool decoded = false;
  std::uint64_t image = 0u;
  // D3D12: copied by a game command list on another direct queue than the presenting one. Such a point is decoded only
  // through InFlightFrame::late_points, never through its frame's range.
  bool cross_queue = false;
  // Waiting in Session::deferred_points for the game queue's lifetime fence before it can be reduced.
  bool deferred = false;
};

// A writer and the measurements around it: the state before it and before the next writer (or the end of the frame).
struct Event {
  WriterInfo writer;
  std::uint32_t frame_index = 0u;
  std::uint64_t image = 0u;
  std::optional<std::uint64_t> before_point = std::nullopt;
  UnmeasuredReason before_reason = UnmeasuredReason::NONE;
  bool after_resolved = false;
  std::optional<std::uint64_t> after_point = std::nullopt;
  UnmeasuredReason after_reason = UnmeasuredReason::NONE;
  // Executed on another queue than the presenting one: ordered against the presenting queue's writers by CPU submission
  // order only (see DEVKIT_MCP.md).
  bool cross_queue = false;
};

struct InFlightFrame {
  std::uint32_t ring_index = 0u;
  std::uint32_t frame_index = 0u;
  std::uint64_t first_point = 0u;
  std::uint64_t point_count = 0u;
  // False for the D3D12/Vulkan warm-up and closing frames, which are not part of the requested frames.
  bool counted = true;
  bool truncated = false;
  // Value of the session's stats fence covering this frame's readback. Max: no readback. 0 on D3D12/Vulkan: not
  // signalled yet (Vulkan, see Session::stats_signal_pending) or no stats fence; unused on D3D11.
  std::uint64_t fence_value = 0u;
  // Cross-queue points of earlier frames (or this one) reduced at this frame end.
  std::vector<std::uint64_t> late_points;
};

struct RunningRange {
  double minimum = std::numeric_limits<double>::infinity();
  double maximum = -std::numeric_limits<double>::infinity();
  double sum = 0.0;
  std::uint64_t count = 0u;

  void Add(double value) {
    minimum = std::min(minimum, value);
    maximum = std::max(maximum, value);
    sum += value;
    ++count;
  }
};

struct WriterAggregate {
  WriterKind kind = WriterKind::DRAW;
  std::uint32_t pixel_shader_hash = 0u;
  std::uint32_t vertex_shader_hash = 0u;
  std::uint32_t compute_shader_hash = 0u;
  std::uint64_t executions = 0u;
  std::uint64_t changed_executions = 0u;
  std::uint64_t peak_lowered_executions = 0u;
  std::uint64_t peak_raised_executions = 0u;
  std::uint64_t dropped_above_one_executions = 0u;
  // Executions after which some pixel's alpha was above 1.0 when none was before (a blend or upgrade side effect).
  std::uint64_t raised_alpha_above_one_executions = 0u;
  std::uint64_t unmeasured_executions = 0u;
  std::uint64_t cross_queue_executions = 0u;
  std::map<UnmeasuredReason, std::uint64_t> unmeasured_reasons = {};
  std::uint32_t frames_seen = 0u;
  std::uint32_t last_frame_index = (std::numeric_limits<std::uint32_t>::max)();
  RunningRange before_peak;
  RunningRange after_peak;
  RunningRange before_mean;
  RunningRange after_mean;
  RunningRange after_max_alpha;
  std::map<std::int64_t, std::uint64_t> after_peak_buckets;
};

// The CPU view of a gpu_fence::Fence's progress. The fence's completion callbacks only count here (see FenceTimeline);
// they take nothing but this mutex, so it is a leaf lock and may be read under the watcher lock.
struct FenceCompletion {
  std::shared_mutex mutex;
  std::condition_variable_any condition;
  std::uint64_t completed = 0u;
};

// D3D12 queue lifetime fences and the D3D12/Vulkan stats readback fence, on utils::gpu_fence. gpu_fence numbers the
// values of one fence 1, 2, 3, ... (a failed signal uses none) and delivers completion callbacks in order, so with a
// callback on every signal the n-th callback means value n completed. That holds only under three rules, kept by
// SignalTimeline: every signal carries a callback (never the callback-less QueueSignal); a timeline is signalled on
// one queue only (gpu_fence orders every signal after the previous one with a GPU wait, so a second queue would be made
// to wait on the first); at most one thread signals it at a time (`signalling`, asserted). Frame ends read `completion`
// instead of querying the GPU, and every CPU wait for a timeline is a bounded wait on its condition variable. The fence
// is destroyed only outside the watcher lock and outside `completion->mutex` (its destructor joins the callback worker),
// never from a callback. gpu_fence passes the backend's completion value through, including UINT64_MAX after a device
// removal: pending callbacks then run as if completed, so the count advances and bounded waits return. A callback is a
// threshold, not proof that the GPU work succeeded; after a removal the watch is expected to fail at its next signal or
// readback map.
struct FenceTimeline {
  std::unique_ptr<renodx::utils::gpu_fence::Fence> fence;
  // OpenGL: polled sync objects on the presenting thread instead of a gpu_fence::Fence (see OpenGLSyncTimeline).
  std::unique_ptr<OpenGLSyncTimeline> opengl;
  std::shared_ptr<FenceCompletion> completion = std::make_shared<FenceCompletion>();
  std::atomic<bool> signalling = false;
};

// D3D12: one execution of a game command list that copies into a scratch texture. D3D12 lets the game reset a command
// list while the GPU still runs it, so the reset does not end the texture's use: it ends when the GPU passes
// `fence_value` on the lifetime fence of `queue`. ReShade raises execute_command_list under the queue's lock, releases
// the lock and only then calls the real ExecuteCommandLists, so a fence signalled from inside the event could land
// before the game's work. A submission whose event came before a frame end gets the value queued at that frame end
// instead (stated assumption, DEVKIT_MCP.md: the game's submit call has returned by then).
struct GpuSubmission {
  // Null once the queue was destroyed: its destroy event then waits for the queue and removes the submissions marked
  // with its `destroyed_queue_wait`; any left after that wait failed are kept until the device is destroyed.
  reshade::api::command_queue* queue = nullptr;
  // Watcher::frame_end_index when the event was recorded: below the current index once a frame end has passed it.
  std::uint64_t frame_end_index = 0u;
  std::optional<std::uint64_t> fence_value = std::nullopt;
  // Set by a frame end that is signalling the queue's lifetime fence for this submission (the value is only known once
  // the signal returns); 0 otherwise. See ProcessLifetimes.
  std::uint64_t pending_signal = 0u;
  std::uint64_t destroyed_queue_wait = 0u;
};

// D3D12/Vulkan: a region-sized texture that receives one measurement copy inside a game command list. It belongs to
// that command list until the list is reset or destroyed, because a list may be executed again without being
// re-recorded and would copy into it again. On D3D12 it is reused only once the GPU also passed every submission.
struct ScratchEntry {
  reshade::api::resource texture = {0u};
  std::uint64_t owner_list = 0u;
  bool in_use = false;
  bool awaiting_reduction = false;
  bool release_after_reduction = false;
  std::vector<GpuSubmission> submissions;
};

// D3D12/Vulkan: what the watch knows about one command list.
struct ListState {
  std::vector<Record> records;
  std::vector<std::uint32_t> scratch_entries;
  std::uint32_t command_count = 0u;
  // Reset or created after the watch started; earlier lists have an unknown render pass state and are not measured.
  bool tracked = false;
  bool direct = true;
  bool bundle = false;
  bool in_render_pass = false;
  std::optional<std::size_t> render_pass_record = std::nullopt;
  // Set once the list was executed (or inserted as a secondary list) since its last reset.
  std::optional<std::uint32_t> executed_frame = std::nullopt;
  std::vector<std::uint64_t> committed_points;
};

// Minimal Vulkan ABI for one image memory barrier; the Devkit does not build against the Vulkan SDK.
struct VulkanImageSubresourceRange {
  std::uint32_t aspect_mask = 0u;
  std::uint32_t base_mip_level = 0u;
  std::uint32_t level_count = 0u;
  std::uint32_t base_array_layer = 0u;
  std::uint32_t layer_count = 0u;
};
struct VulkanImageMemoryBarrier {
  std::uint32_t s_type = 45u;  // VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
  const void* p_next = nullptr;
  std::uint32_t src_access_mask = 0u;
  std::uint32_t dst_access_mask = 0u;
  std::uint32_t old_layout = 0u;
  std::uint32_t new_layout = 0u;
  std::uint32_t src_queue_family_index = 0xFFFFFFFFu;  // VK_QUEUE_FAMILY_IGNORED
  std::uint32_t dst_queue_family_index = 0xFFFFFFFFu;
  std::uint64_t image = 0u;
  VulkanImageSubresourceRange subresource_range = {};
};
// VkImage is a 64-bit handle on both targets; only pNext changes size (x86 builds: 64 bytes).
static_assert(sizeof(VulkanImageMemoryBarrier) == (sizeof(void*) == 8u ? 72u : 64u));
using VulkanCmdPipelineBarrier = void(__stdcall*)(
    void* command_buffer,
    std::uint32_t src_stage_mask,
    std::uint32_t dst_stage_mask,
    std::uint32_t dependency_flags,
    std::uint32_t memory_barrier_count,
    const void* memory_barriers,
    std::uint32_t buffer_barrier_count,
    const void* buffer_barriers,
    std::uint32_t image_barrier_count,
    const VulkanImageMemoryBarrier* image_barriers);
using VulkanGetDeviceProcAddr = void*(__stdcall*)(void* device, const char* name);

// D3D9: a native object owned by the watch, released when the watch's objects are destroyed.
struct D3D9Release {
  void operator()(IUnknown* object) const { object->Release(); }
};
template <typename T>
using D3D9Pointer = std::unique_ptr<T, D3D9Release>;

// D3D9: one intermediate level of the reduction: the four stats groups of every 8x8 block of the level below.
struct D3D9ReductionLevel {
  std::array<reshade::api::resource, D3D9_STATS_GROUPS> targets = {};
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
};

// D3D9: the objects of the frame-end reduction. The atlas is Session::scratch (a grid of region-sized slots,
// `atlas_columns` per row) and the stats target is Session::stats_texture (D3D9_STATS_GROUPS x MAX_SLOTS, one row per
// slot). The state block restores the game's state after the passes.
struct D3D9Reduction {
  D3D9Pointer<IDirect3DStateBlock9> state_block;
  D3D9Pointer<IDirect3DVertexDeclaration9> vertex_declaration;
  D3D9Pointer<IDirect3DVertexShader9> vertex_shader;
  D3D9Pointer<IDirect3DPixelShader9> pixel_shader;
  std::vector<D3D9ReductionLevel> levels;
  std::uint32_t atlas_columns = 0u;
  std::uint32_t atlas_width = 0u;
  std::uint32_t atlas_height = 0u;
};

struct Session {
  std::shared_ptr<Request> request;
  std::string failure;
  // The effect runtime that started the watch: only its frame ends measure (ProcessAtFrameEnd). Null once ReShade
  // reset or destroyed it (OnEffectRuntimeDestroyed), which fails the watch.
  reshade::api::effect_runtime* runtime = nullptr;

  reshade::api::resource watched_resource = {0u};
  // With preferClone, copies/clears/resolves/updates may name the original texture while the game's draws write the
  // clone; non-draw writers naming either one are measured.
  reshade::api::resource original_resource = {0u};
  // D3D12/Vulkan watch on a swap-chain image: every image of that swap chain, so the watch follows the current back
  // buffer (the game may render each frame into any of them, in any order). Empty otherwise.
  std::vector<std::uint64_t> swap_chain_images;
  // Device-level update events carry no command list; on D3D11 they always come from the immediate context.
  reshade::api::command_list* immediate_cmd_list = nullptr;
  std::uint32_t subresource = 0u;
  std::uint32_t mip_level = 0u;
  std::uint32_t array_layer = 0u;
  bool single_subresource = true;
  reshade::api::subresource_box region = {};
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  Signal signal = Signal::RAW;
  std::uint32_t frames = 1u;
  std::optional<std::uint32_t> shader_hash = std::nullopt;
  bool include_unchanged = false;
  bool with_snapshot = false;
  bool used_clone = false;
  reshade::api::format scratch_format = reshade::api::format::unknown;
  std::string resource_format_name;

  // D3D12/Vulkan: measurements are copies in the game's command lists, reduced at the end of the frame.
  bool explicit_api = false;
  reshade::api::device_api device_api = reshade::api::device_api::d3d11;
  // The command queue of `runtime`. Every frame-end operation of the watch (reductions, stats fence, readback maps,
  // the final waits) runs on it, inside a present on it, where ReShade holds this queue's lock.
  reshade::api::command_queue* queue = nullptr;
  VulkanCmdPipelineBarrier vulkan_cmd_pipeline_barrier = nullptr;
  std::vector<ScratchEntry> scratch_entries;
  std::uint32_t max_scratch_entries = 0u;
  // Hook reservations that found no free scratch texture since the last frame end.
  std::uint32_t scratch_pool_misses = 0u;
  // Reservations asked for since the last frame end (found or not, hooks and frame end): the frame end grows the pool
  // so that the next frame starts with this many free textures, within the budget.
  std::uint32_t scratch_frame_demand = 0u;
  // Largest pool size a frame end wanted (the pool plus its shortfall), even beyond the budget.
  std::uint32_t scratch_entries_needed = 0u;
  std::uint32_t scratch_budget_mb = DEFAULT_SCRATCH_BUDGET_MB;
  std::unordered_map<std::uint64_t, ListState> lists;
  // D3D12/Vulkan stats readback fence, signalled only on `queue` by the presenting thread. Null without one (Vulkan
  // without timeline semaphores): a readback is then read after waiting for the queue to go idle.
  std::shared_ptr<FenceTimeline> stats_timeline;
  // Vulkan: the newest in-flight frame's signal waits for finish_present, after ReShade submitted the frame's immediate
  // command list with the present's wait semaphores; signalling inside reshade_present would submit it early, without
  // them. Its fence_value stays 0 until then.
  bool stats_signal_pending = false;
  bool stats_signal_failed = false;

  // verify: CPU readback of the region at one measurement point, compared with the GPU stats of the same point.
  bool verify = false;
  reshade::api::resource verify_readback = {0u};
  std::optional<std::uint64_t> verify_point = std::nullopt;
  bool verify_copied = false;
  std::optional<RegionStats> verify_gpu_stats = std::nullopt;
  // The measurement taken at the end of the last frame (D3D11: always; D3D12/Vulkan: the presented back buffer only),
  // reported as `endOfWatch`: it includes every write made during present, by the game or by any add-on.
  std::optional<std::uint64_t> end_point = std::nullopt;
  std::optional<RegionStats> end_stats = std::nullopt;

  // D3D11: the region copy that is reduced. D3D12/Vulkan: a scratch entry is copied here before it is reduced. D3D9 and
  // OpenGL: the atlas of measurement slots.
  reshade::api::resource scratch = {0u};
  reshade::api::resource_view scratch_srv = {0u};
  reshade::api::resource stats_texture = {0u};
  reshade::api::resource_view stats_uav = {0u};
  std::array<reshade::api::resource, READBACK_RING_SIZE> readbacks = {};
  renodx::utils::render::RenderPass pass;
  // width, height, signal, slot as raw bits; the compute pass reads this through push constants.
  std::array<float, 4> constants = {};
  D3D9Reduction d3d9;
  OpenGLReduction opengl;

  // State of the frame being recorded.
  std::uint32_t slots_used = 0u;
  // D3D11: MAX_SLOTS. D3D9 and OpenGL: the atlas slots that fit the scratch budget and the device's texture size.
  std::uint32_t slot_capacity = MAX_SLOTS;
  std::uint32_t command_count = 0u;
  bool stats_cleared = false;
  bool truncated = false;
  std::uint64_t frame_first_point = 0u;
  std::uint32_t frame_command_base = 0u;

  // Commit state, in submission order.
  std::deque<Point> points;
  std::uint64_t points_base = 0u;
  std::deque<Event> events;
  std::uint64_t events_base = 0u;
  std::optional<std::uint64_t> open_event = std::nullopt;
  // The latest point with no writer committed after it: the state any later unmeasured writer starts from.
  std::optional<std::uint64_t> last_point = std::nullopt;
  // D3D12/Vulkan: the first frame only establishes which command lists are tracked (lists reset before the watch
  // started have an unknown render pass state); no writer is reported for it.
  bool warming_up = false;
  bool closing = false;
  std::uint32_t closing_frames = 0u;
  std::chrono::steady_clock::time_point closing_started_at = {};
  // D3D12: cross-queue points not reduced yet, in commit order. A frame end reduces those whose copy the GPU has
  // finished, judged on the CPU from the game queue's lifetime fence (Watcher::completed_fence_values), never by making
  // the presenting queue wait on the GPU: a fence that never arrives must not freeze present.
  std::vector<std::uint64_t> deferred_points;

  std::uint32_t frames_recorded = 0u;
  std::uint32_t frames_decoded = 0u;
  std::uint32_t truncated_frames = 0u;
  std::uint64_t readback_pushes = 0u;
  std::deque<InFlightFrame> in_flight;

  std::map<std::string, WriterAggregate> aggregates;
  json trace = json::array();
  std::uint64_t events_decoded = 0u;
  std::uint64_t events_changed = 0u;
  std::uint64_t events_unmeasured = 0u;
  std::uint64_t events_cross_queue = 0u;
  std::map<UnmeasuredReason, std::uint64_t> unmeasured_reasons = {};
  std::vector<std::uint32_t> frames_with_unmeasured_writers;
};

// A D3D12/Vulkan scratch texture of a finished watch that a command list may still copy into (`owner_list`, 0 once
// that list was reset or destroyed) or that the GPU may still be writing (D3D12 `submissions`). It is destroyed when
// both are over (Vulkan: at the reset, which the game may only do once the GPU is done) or with the device.
struct RetiredScratch {
  reshade::api::resource texture = {0u};
  std::uint64_t owner_list = 0u;
  std::vector<GpuSubmission> submissions;
};

// D3D12: the lifetime fence of one game queue. gpu_fence signals it with the native ID3D12CommandQueue::Signal, not
// ReShade's command_queue::signal, which also flushes that queue's immediate command list without the queue's lock and
// would race with the game thread submitting to it. Fences are created only at frame ends, on the presenting thread.
struct QueueLifetimeFence {
  std::shared_ptr<FenceTimeline> timeline;
  // Signals issued after releasing the watcher lock and not attached yet; other frame ends skip the queue meanwhile, and
  // its destroy event and the device's wait for them before the queue goes away.
  std::uint32_t signals_in_flight = 0u;
};

// D3D12/Vulkan: a region copy reserved under the watcher lock and recorded after releasing it, because recording calls
// into the driver, which can call back into the game (for example its allocation callbacks), and the game may then
// wait for one of its threads that is waiting for the watcher lock. The scratch texture already exists and stays alive
// until the command list is reset, even if the watch ends meanwhile (the frame end then retires it to that list); the
// session reference keeps the fixed watch settings the copy reads.
struct PendingCopy {
  std::shared_ptr<Session> session;
  reshade::api::resource texture = {0u};
  std::uint64_t image = 0u;
  reshade::api::resource_usage state = reshade::api::resource_usage::undefined;
};

// The watcher is owned by the per-device data. `watched_resource_handle` is a lock-free gate (0 = no watch running), so
// draws pay nothing while idle, and lets a destroyed watched texture stop the watch without taking the lock.
struct Watcher {
  std::shared_mutex mutex;
  std::atomic<std::uint64_t> watched_resource_handle = 0u;
  std::atomic<bool> has_retired_scratch = false;
  // Set while a session is installed, also after the watched texture was destroyed (the gate above is 0 then): its
  // D3D12 scratch textures still need their submissions recorded.
  std::atomic<bool> has_session = false;
  // OpenGL: the context of the latest watch started (null for other APIs), so that hooks on other contexts return before
  // taking the lock. Stored before `watched_resource_handle` and only read behind a non-zero one, so it is not cleared.
  std::atomic<void*> opengl_context = nullptr;
  // Shared with hooks that record a region copy after releasing the lock, so the session outlives them.
  std::shared_ptr<Session> session;
  std::deque<std::shared_ptr<Request>> pending_requests;
  // Set by the Devkit for withSnapshot. begin_linked_snapshot starts a snapshot of the device's next frame and returns its id (nullopt when
  // another snapshot is queued or running); called without the watcher lock. peek_snapshot_command returns the key the
  // running snapshot gives the command a hook is about to see (nullopt when no snapshot records `cmd_list`); the watch
  // hooks run before the snapshot's, so it is called under the lock and must not take other locks.
  std::function<std::optional<std::uint64_t>()> begin_linked_snapshot;
  std::function<std::optional<SnapshotCommandKey>(reshade::api::command_list* cmd_list)> peek_snapshot_command;
  std::vector<RetiredScratch> retired_scratch;
  // D3D12: lifetime fences by queue handle, and the number of frame ends so far (see GpuSubmission).
  std::unordered_map<std::uint64_t, QueueLifetimeFence> queue_fences;
  // Completed value of each queue's lifetime fence, read by the last ProcessLifetimes (by queue handle).
  std::unordered_map<std::uint64_t, std::uint64_t> completed_fence_values;
  std::uint64_t frame_end_index = 0u;
  std::uint64_t destroyed_queue_waits = 0u;
  std::uint64_t planned_signal_ids = 0u;
  // Fences of destroyed queues (and of a watch on one) whose final wait failed, destroyed with the device: the GPU may
  // still signal them.
  std::vector<std::shared_ptr<FenceTimeline>> orphan_timelines;
  // Lock diagnostics: the thread holding `mutex` and what it is doing (see WatcherLock).
  std::atomic<std::uint32_t> owner_thread_id = 0u;
  std::atomic<const char*> owner_step = nullptr;
};

// Holds `watcher->mutex` and records the owner's thread and step. A wait longer than LOCK_WAIT_REPORT_TIME is logged once
// with the owner's thread and step (the waiting thread itself as owner means the lock was re-entered), then the wait
// continues.
class WatcherLock {
 public:
  WatcherLock(Watcher* watcher, const char* step) : watcher_(watcher) {
    if (!watcher->mutex.try_lock()) {
      const auto start = std::chrono::steady_clock::now();
      bool reported = false;
      while (!watcher->mutex.try_lock()) {
        if (!reported && std::chrono::steady_clock::now() - start > LOCK_WAIT_REPORT_TIME) {
          reported = true;
          const auto owner_thread_id = watcher->owner_thread_id.load(std::memory_order_relaxed);
          const auto* owner_step = watcher->owner_step.load(std::memory_order_relaxed);
          reshade::log::message(
              reshade::log::level::warning,
              std::format(
                  "devkit::watch_region(lock wait over 2 s: thread {} in {} waits for thread {} in {}{})",
                  GetCurrentThreadId(),
                  step,
                  owner_thread_id,
                  (owner_step != nullptr ? owner_step : "unknown"),
                  (owner_thread_id == GetCurrentThreadId() ? ", re-entered on the same thread" : ""))
                  .c_str());
        }
        std::this_thread::yield();
      }
    }
    SetStep(step);
  }
  ~WatcherLock() { Unlock(); }
  WatcherLock(const WatcherLock&) = delete;
  WatcherLock& operator=(const WatcherLock&) = delete;

  void SetStep(const char* step) {
    watcher_->owner_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
    watcher_->owner_step.store(step, std::memory_order_relaxed);
  }

  // Releases the lock before the end of the scope, so the caller can call into ReShade or the driver.
  void Unlock() {
    if (watcher_ == nullptr) return;
    watcher_->owner_thread_id.store(0u, std::memory_order_relaxed);
    watcher_->owner_step.store(nullptr, std::memory_order_relaxed);
    watcher_->mutex.unlock();
    watcher_ = nullptr;
  }

 private:
  Watcher* watcher_;
};

namespace internal {

// Set while the watch issues its own native Vulkan barrier: ReShade reports it as a barrier event on this thread while
// the watcher lock is held.
static thread_local bool issuing_native_commands = false;

// D3D12: calls `visit` on the submissions of every scratch texture the watcher knows: the running watch's and the
// retired ones. Under the watcher lock.
template <typename Visitor>
inline void ForEachSubmissionList(Watcher* watcher, Visitor&& visit) {
  if (watcher->session != nullptr) {
    for (auto& entry : watcher->session->scratch_entries) {
      visit(entry.submissions);
    }
  }
  for (auto& retired : watcher->retired_scratch) {
    visit(retired.submissions);
  }
}

// D3D12: adds one execution of a command list to a texture's submissions. A newer execution on the same queue replaces
// an older one that has no fence value yet: the value attached later also covers the older work on that queue.
inline void AddSubmission(std::vector<GpuSubmission>* submissions, const GpuSubmission& submission) {
  for (auto& existing : *submissions) {
    if (existing.queue == submission.queue && !existing.fence_value.has_value()) {
      existing = submission;
      return;
    }
  }
  submissions->push_back(submission);
}

// D3D12: whether the GPU passed every submission, judged from completed fence values read without the lock (by queue
// handle). A submission without a fence value, or of a destroyed queue, is not complete.
[[nodiscard]] inline bool AreSubmissionsComplete(
    const std::vector<GpuSubmission>& submissions,
    const std::unordered_map<std::uint64_t, std::uint64_t>& completed_values) {
  return std::ranges::all_of(submissions, [&](const GpuSubmission& submission) {
    if (submission.queue == nullptr || !submission.fence_value.has_value()) return false;
    const auto pair = completed_values.find(reinterpret_cast<std::uint64_t>(submission.queue));
    return pair != completed_values.end() && pair->second >= submission.fence_value.value();
  });
}

// Creates a fence timeline, or returns null (logged) when the device cannot create one, for example a Vulkan device
// without timeline semaphores. OpenGL: a polled sync timeline bound to the current context, so it must be created, signalled,
// polled and waited for on the presenting thread.
[[nodiscard]] inline std::shared_ptr<FenceTimeline> CreateFenceTimeline(reshade::api::device* device) {
  try {
    auto timeline = std::make_shared<FenceTimeline>();
    if (device->get_api() == reshade::api::device_api::opengl) {
      const auto* functions = GetOpenGLFunctions();
      if (functions == nullptr || functions->get_current_context() == nullptr) {
        reshade::log::message(reshade::log::level::warning, "devkit::watch_region(fence creation failed: no OpenGL context is current)");
        return nullptr;
      }
      timeline->opengl = std::make_unique<OpenGLSyncTimeline>();
      timeline->opengl->functions = functions;
      timeline->opengl->context = functions->get_current_context();
      return timeline;
    }
    timeline->fence = std::make_unique<renodx::utils::gpu_fence::Fence>(device);
    return timeline;
  } catch (const std::exception& exception) {
    reshade::log::message(
        reshade::log::level::warning,
        std::format("devkit::watch_region(fence creation failed: {})", exception.what()).c_str());
    return nullptr;
  }
}

// Signals the next value of `timeline` on `queue` and returns it; 0 means the signal failed and queued nothing. On D3D12
// the native signal does not flush ReShade's immediate command list; on Vulkan ReShade's signal does. Never under the
// watcher lock (it calls the driver). gpu_fence returns 0 for a failed backend signal but throws for a failed setup step
// (native query, callback worker start); that is logged and also returns 0. gpu_fence withdraws the callback before it
// throws, so the callback count stays right.
[[nodiscard]] inline std::uint64_t SignalTimeline(FenceTimeline* timeline, reshade::api::command_queue* queue) {
  [[maybe_unused]] const bool already_signalling = timeline->signalling.exchange(true, std::memory_order_acq_rel);
  assert(!already_signalling);
  struct SignallingReset {
    std::atomic<bool>* signalling;
    ~SignallingReset() { signalling->store(false, std::memory_order_release); }
  } signalling_reset{.signalling = &timeline->signalling};
  if (timeline->opengl != nullptr) {
    // The sync object completes behind every command of the frame; the flush after it starts the GPU on them. It is
    // ReShade's own (unhooked) glFlush: the exported one raises execute_command_list to every add-on.
    auto& opengl_timeline = *timeline->opengl;
    if (opengl_timeline.functions->get_current_context() != opengl_timeline.context) {
      reshade::log::message(reshade::log::level::error, "devkit::watch_region(fence signal failed: the OpenGL context is not the one the fence belongs to)");
      return 0u;
    }
    auto* const sync = opengl_timeline.functions->fence_sync(gl_enum::SYNC_GPU_COMMANDS_COMPLETE, 0u);
    if (sync == nullptr) return 0u;
    opengl_timeline.pending.push_back(OpenGLSyncTimeline::PendingSync{.value = ++opengl_timeline.last_signalled, .sync = sync});
    queue->flush_immediate_command_list();
    return opengl_timeline.last_signalled;
  }
  try {
    return renodx::utils::gpu_fence::QueueSignal(
        queue,
        timeline->fence.get(),
        [completion = timeline->completion]() {
          {
            std::unique_lock completion_lock(completion->mutex);
            ++completion->completed;
          }
          completion->condition.notify_all();
        });
  } catch (const std::exception& exception) {
    reshade::log::message(
        reshade::log::level::error,
        std::format("devkit::watch_region(fence signal threw: {})", exception.what()).c_str());
    return 0u;
  }
}

// OpenGL: polls the sync objects in order on the current context and publishes the ones that completed.
[[nodiscard]] inline std::uint64_t GetCompletedValue(FenceTimeline* timeline) {
  if (timeline->opengl != nullptr) {
    auto& opengl_timeline = *timeline->opengl;
    if (opengl_timeline.functions->get_current_context() == opengl_timeline.context) {
      while (!opengl_timeline.pending.empty()) {
        const auto [value, sync] = opengl_timeline.pending.front();
        const auto status = opengl_timeline.functions->client_wait_sync(sync, 0u, 0u);
        if (status != gl_enum::ALREADY_SIGNALED && status != gl_enum::CONDITION_SATISFIED) break;
        opengl_timeline.functions->delete_sync(sync);
        opengl_timeline.pending.pop_front();
        std::unique_lock completion_lock(timeline->completion->mutex);
        timeline->completion->completed = value;
      }
    }
  }
  std::shared_lock completion_lock(timeline->completion->mutex);
  return timeline->completion->completed;
}

// Waits up to LIFETIME_FENCE_WAIT_TIMEOUT until the GPU passed `value` on `timeline`. Never under the watcher lock.
// OpenGL: waits on the oldest sync object (flushing the context's commands) until `value` completed; false at once when the
// current context is not the fence's.
[[nodiscard]] inline bool WaitTimeline(FenceTimeline* timeline, std::uint64_t value) {
  if (timeline->opengl != nullptr) {
    auto& opengl_timeline = *timeline->opengl;
    if (opengl_timeline.functions->get_current_context() != opengl_timeline.context) return false;
    const auto deadline = std::chrono::steady_clock::now() + LIFETIME_FENCE_WAIT_TIMEOUT;
    while (GetCompletedValue(timeline) < value) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (opengl_timeline.pending.empty() || remaining <= std::chrono::steady_clock::duration::zero()) return false;
      const auto status = opengl_timeline.functions->client_wait_sync(
          opengl_timeline.pending.front().sync,
          gl_enum::SYNC_FLUSH_COMMANDS_BIT,
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()));
      if (status == gl_enum::SYNC_WAIT_FAILED) return false;
    }
    return true;
  }
  auto& completion = *timeline->completion;
  std::unique_lock completion_lock(completion.mutex);
  return completion.condition.wait_for(completion_lock, LIFETIME_FENCE_WAIT_TIMEOUT, [&completion, value]() {
    return completion.completed >= value;
  });
}

[[nodiscard]] inline std::string_view GetWriterKindName(WriterKind kind) {
  switch (kind) {
    case WriterKind::DRAW:
      return "draw";
    case WriterKind::DISPATCH:
      return "dispatch";
    case WriterKind::COPY:
      return "copy";
    case WriterKind::CLEAR:
      return "clear";
    case WriterKind::RESOLVE:
      return "resolve";
    case WriterKind::UPDATE:
      return "update";
    case WriterKind::RENDER_PASS:
      return "render_pass";
    case WriterKind::BUNDLE:
      return "bundle";
  }
  return "unknown";
}

// The command lists whose recording order is their execution order: the D3D11 immediate context, the D3D9 device and
// OpenGL contexts. OpenGL has one such list per context, so the hooks also compare the current context with the session's.
[[nodiscard]] inline bool IsImmediateCommandList(reshade::api::command_list* cmd_list) {
  switch (cmd_list->get_device()->get_api()) {
    case reshade::api::device_api::d3d9:
    case reshade::api::device_api::opengl:
      return true;
    case reshade::api::device_api::d3d11: {
      auto* native_context = reinterpret_cast<ID3D11DeviceContext*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
      return native_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    }
    default:
      return false;
  }
}

// D3D9: the surface of a ReShade resource handle (a native IDirect3DSurface9 or IDirect3DTexture9; `level` for a
// texture), or null for another resource type.
[[nodiscard]] inline D3D9Pointer<IDirect3DSurface9> GetD3D9Surface(reshade::api::resource resource, std::uint32_t level) {
  auto* native_resource = reinterpret_cast<IDirect3DResource9*>(static_cast<std::uintptr_t>(resource.handle));
  IDirect3DSurface9* surface = nullptr;
  switch (native_resource->GetType()) {
    case D3DRTYPE_SURFACE:
      surface = static_cast<IDirect3DSurface9*>(native_resource);
      surface->AddRef();
      break;
    case D3DRTYPE_TEXTURE:
      if (FAILED(static_cast<IDirect3DTexture9*>(native_resource)->GetSurfaceLevel(level, &surface))) {
        surface = nullptr;
      }
      break;
    default:
      break;
  }
  return D3D9Pointer<IDirect3DSurface9>(surface);
}

// OpenGL: whether the calling thread's context is not the one the running watch belongs to (always false for other APIs).
// Lock-free: loader threads and helper contexts must not take the watcher lock on every draw and upload.
[[nodiscard]] inline bool IsOtherOpenGLContext(const Watcher& watcher) {
  const auto context = watcher.opengl_context.load(std::memory_order_acquire);
  if (context == nullptr) return false;
  const auto* functions = GetOpenGLFunctions();
  return functions == nullptr || functions->get_current_context() != context;
}

[[nodiscard]] inline bool IsExplicitApi(reshade::api::device_api api) {
  return api == reshade::api::device_api::d3d12 || api == reshade::api::device_api::vulkan;
}

// The watched image a resource stands for, or 0: the watched texture, any image of the followed swap chain, or the
// original of a watched clone (which stands for the clone).
[[nodiscard]] inline std::uint64_t GetWatchedImage(const Session& session, std::uint64_t handle) {
  if (handle == 0u) return 0u;
  if (handle == session.watched_resource.handle || handle == session.original_resource.handle) return session.watched_resource.handle;
  return (std::ranges::find(session.swap_chain_images, handle) != session.swap_chain_images.end() ? handle : 0u);
}

// The watched image a view (render target, UAV, attachment) writes, when it includes the watched mip level and array
// layer; 0 otherwise.
[[nodiscard]] inline std::uint64_t GetWatchedImageOfView(
    const Session& session,
    reshade::api::device* device,
    reshade::api::resource_view view) {
  if (view.handle == 0u) return 0u;
  const auto image = GetWatchedImage(session, device->get_resource_from_view(view).handle);
  if (image == 0u) return 0u;
  const auto view_desc = device->get_resource_view_desc(view);
  const auto& texture = view_desc.texture;
  // ReShade's D3D11/D3D12 view descriptions leave the count at 0 for non-array dimensions (a D3D12 Texture2D RTV,
  // DSV or UAV has layer_count 0): such a view covers exactly one level and one layer.
  const auto level_count = (texture.level_count == 0u ? 1u : texture.level_count);
  const auto layer_count = (texture.layer_count == 0u ? 1u : texture.layer_count);
  const auto covers_level = session.mip_level >= texture.first_level
                            && (level_count == UINT32_MAX || session.mip_level < texture.first_level + level_count);
  const auto covers_layer = session.array_layer >= texture.first_layer
                            && (layer_count == UINT32_MAX || session.array_layer < texture.first_layer + layer_count);
  return (covers_level && covers_layer ? image : 0u);
}

// withSnapshot: the key of the snapshot command the hook is about to see. Only draws, dispatches and copies are snapshot
// commands, so clears, resolves and updates never ask.
[[nodiscard]] inline std::optional<SnapshotCommandKey> PeekSnapshotCommand(
    const Watcher& watcher, const Session& session, reshade::api::command_list* cmd_list) {
  if (!session.with_snapshot || cmd_list == nullptr || !watcher.peek_snapshot_command) return std::nullopt;
  return watcher.peek_snapshot_command(cmd_list);
}

[[nodiscard]] inline bool MatchesFilter(const Session& session, const WriterInfo& writer) {
  if (!session.shader_hash.has_value()) return true;
  const auto hash = session.shader_hash.value();
  if (writer.pixel_shader_hash == hash || writer.vertex_shader_hash == hash || writer.compute_shader_hash == hash) return true;
  return std::ranges::any_of(writer.members, [hash](const DrawMember& member) {
    return member.pixel_shader_hash == hash || member.vertex_shader_hash == hash;
  });
}

// D3D12/Vulkan, under the watcher lock: reserves a free scratch entry for `owner_list` (0 = owned by the frame end).
// Hooks only take entries that already have a texture: creating an image inside the game's own command recording
// calls into the driver and the game's allocation callbacks from a context the game does not expect (a deadlock
// inside DOOM 2016's allocator). The frame end (`allow_new`) may add an entry without a texture and create it on the
// presenting thread. Returns the entry, or nullopt with SCRATCH_BUDGET (pool at the budget) or SCRATCH_POOL_EMPTY
// (all textures in use; the pool grows at the next frame end).
[[nodiscard]] inline std::pair<std::optional<std::uint32_t>, UnmeasuredReason> ReserveScratchEntry(
    Session* session,
    std::uint64_t owner_list,
    bool allow_new) {
  ++session->scratch_frame_demand;
  std::optional<std::uint32_t> entry_index = std::nullopt;
  for (std::uint32_t index = 0u; index < session->scratch_entries.size(); ++index) {
    const auto& candidate = session->scratch_entries[index];
    if (!candidate.in_use && (allow_new || candidate.texture.handle != 0u)) {
      entry_index = index;
      break;
    }
  }
  if (!entry_index.has_value()) {
    if (session->scratch_entries.size() >= session->max_scratch_entries) return {std::nullopt, UnmeasuredReason::SCRATCH_BUDGET};
    if (!allow_new) {
      ++session->scratch_pool_misses;
      return {std::nullopt, UnmeasuredReason::SCRATCH_POOL_EMPTY};
    }
    session->scratch_entries.emplace_back();
    entry_index = static_cast<std::uint32_t>(session->scratch_entries.size() - 1u);
  }
  auto& entry = session->scratch_entries[entry_index.value()];
  entry.in_use = true;
  entry.owner_list = owner_list;
  entry.awaiting_reduction = false;
  entry.release_after_reduction = false;
  return {entry_index, UnmeasuredReason::NONE};
}

// D3D12/Vulkan, outside the watcher lock and only on the presenting thread: a scratch texture for one region copy ({0}
// on failure).
[[nodiscard]] inline reshade::api::resource CreateScratchTexture(reshade::api::device* device, const Session& session) {
  reshade::api::resource texture = {0u};
  if (!device->create_resource(
          reshade::api::resource_desc(session.width, session.height, 1, 1, session.scratch_format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::copy_dest | reshade::api::resource_usage::copy_source),
          nullptr,
          reshade::api::resource_usage::copy_dest,
          &texture)) {
    return {0u};
  }
  return texture;
}

// D3D12/Vulkan, outside the watcher lock: records the copy of the region of `image`, which is in `state` right now,
// into `texture`. Reads only session fields that are fixed for the whole watch.
inline void RecordRegionCopy(
    const Session& session,
    reshade::api::command_list* cmd_list,
    std::uint64_t image,
    reshade::api::resource texture,
    reshade::api::resource_usage state) {
  // Transitions only the watched subresource. ReShade's barrier() transitions every subresource, which would corrupt
  // the state of the other slices or mips of an array texture, so textures with more than one subresource use the
  // native barrier on D3D12 and Vulkan.
  const auto transition_watched_subresource = [&](reshade::api::resource_usage old_state, reshade::api::resource_usage new_state) {
    if (session.single_subresource) {
      cmd_list->barrier(reshade::api::resource{image}, old_state, new_state);
      return;
    }
    if (session.device_api == reshade::api::device_api::d3d12) {
      // Same conversion as ReShade's convert_usage_to_resource_states for the colour states used here.
      const auto to_d3d12_state = [](reshade::api::resource_usage usage) {
        if (usage == reshade::api::resource_usage::present) return D3D12_RESOURCE_STATE_PRESENT;
        if (usage == reshade::api::resource_usage::general) return D3D12_RESOURCE_STATE_COMMON;
        return static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(usage) & 0xFFFFFFFu);
      };
      const D3D12_RESOURCE_BARRIER barrier = {
          .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition = {
              .pResource = reinterpret_cast<ID3D12Resource*>(static_cast<std::uintptr_t>(image)),
              .Subresource = session.subresource,
              .StateBefore = to_d3d12_state(old_state),
              .StateAfter = to_d3d12_state(new_state),
          },
      };
      auto* native_cmd_list = reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
      native_cmd_list->ResourceBarrier(1u, &barrier);
      return;
    }

    // Same conversion as ReShade's convert_usage_to_image_layout for the colour states used here.
    const auto to_vulkan_layout = [](reshade::api::resource_usage usage) -> std::uint32_t {
      switch (usage) {
        case reshade::api::resource_usage::render_target:
          return 2u;  // VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        case reshade::api::resource_usage::shader_resource:
          return 5u;  // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        case reshade::api::resource_usage::copy_source:
          return 6u;  // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
        case reshade::api::resource_usage::copy_dest:
          return 7u;  // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
        case reshade::api::resource_usage::present:
          return 1000001002u;  // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        default:
          return 1u;  // VK_IMAGE_LAYOUT_GENERAL (unordered access)
      }
    };
    constexpr std::uint32_t ALL_COMMANDS_STAGE = 0x00010000u;
    constexpr std::uint32_t MEMORY_READ_AND_WRITE_ACCESS = 0x00008000u | 0x00010000u;
    const VulkanImageMemoryBarrier barrier = {
        .src_access_mask = MEMORY_READ_AND_WRITE_ACCESS,
        .dst_access_mask = MEMORY_READ_AND_WRITE_ACCESS,
        .old_layout = to_vulkan_layout(old_state),
        .new_layout = to_vulkan_layout(new_state),
        .image = image,
        .subresource_range = {
            .aspect_mask = 1u,  // VK_IMAGE_ASPECT_COLOR_BIT
            .base_mip_level = session.mip_level,
            .level_count = 1u,
            .base_array_layer = session.array_layer,
            .layer_count = 1u,
        },
    };
    issuing_native_commands = true;
    session.vulkan_cmd_pipeline_barrier(
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(cmd_list->get_native())),
        ALL_COMMANDS_STAGE,
        ALL_COMMANDS_STAGE,
        0u,
        0u,
        nullptr,
        0u,
        nullptr,
        1u,
        &barrier);
    issuing_native_commands = false;
  };

  transition_watched_subresource(state, reshade::api::resource_usage::copy_source);
  cmd_list->copy_texture_region(reshade::api::resource{image}, session.subresource, &session.region, texture, 0u, nullptr);
  transition_watched_subresource(reshade::api::resource_usage::copy_source, state);
}

[[nodiscard]] inline Point* FindPoint(Session* session, std::uint64_t point_id) {
  if (point_id < session->points_base || point_id - session->points_base >= session->points.size()) return nullptr;
  return &session->points[static_cast<std::size_t>(point_id - session->points_base)];
}

// Commits one record in submission order: a measurement closes the open event (it is the state after that writer) and
// a writer that matches the filter opens a new event. `refused` drops the record's measurement (for example a command
// list executed on another queue); its writer is still accounted for. Before and after are only taken from the same
// image (a followed swap chain has several).
// `cross_queue` (D3D12): the record's list ran on another direct queue than the presenting one; its point is reduced
// once the GPU finished the copy there (Session::deferred_points).
inline void Commit(Session* session, const Record& record, UnmeasuredReason refused, bool cross_queue = false) {
  std::optional<std::uint64_t> point_id = std::nullopt;
  auto unmeasured_reason = record.reason;
  if (record.measurement.has_value() && refused == UnmeasuredReason::NONE) {
    point_id = session->points_base + session->points.size();
    auto point = Point{.frame_index = session->frames_recorded, .image = record.image, .cross_queue = cross_queue};
    if (session->explicit_api) {
      point.scratch_entry = record.measurement;
      session->scratch_entries[record.measurement.value()].awaiting_reduction = true;
    } else {
      point.slot = record.measurement;
    }
    const auto becomes_verify_point = (session->closing && session->verify && !session->verify_point.has_value());
    if (cross_queue) {
      // While closing, only a point that ends the open writer or becomes the verify point is worth waiting for.
      if (!session->closing || session->open_event.has_value() || becomes_verify_point) {
        point.deferred = true;
        session->deferred_points.push_back(point_id.value());
      } else {
        point.refused = UnmeasuredReason::CROSS_QUEUE_NOT_READY;
        point.decoded = true;
      }
    }
    session->points.push_back(point);
    if (becomes_verify_point && point.refused == UnmeasuredReason::NONE) {
      session->verify_point = point_id;
    }
  } else if (refused != UnmeasuredReason::NONE) {
    unmeasured_reason = refused;
  }
  if (unmeasured_reason == UnmeasuredReason::SLOT_LIMIT || unmeasured_reason == UnmeasuredReason::SCRATCH_BUDGET) {
    session->truncated = true;
  }

  if (session->open_event.has_value() && (point_id.has_value() || record.writer.has_value())) {
    auto& open_event = session->events[static_cast<std::size_t>(session->open_event.value() - session->events_base)];
    const auto same_image = (open_event.image == record.image);
    open_event.after_resolved = true;
    open_event.after_point = (same_image ? point_id : std::nullopt);
    if (!same_image) {
      open_event.after_reason = UnmeasuredReason::NO_LATER_MEASUREMENT;
    } else {
      open_event.after_reason = (point_id.has_value() ? UnmeasuredReason::NONE : unmeasured_reason);
    }
    session->open_event.reset();
  }

  if (!record.writer.has_value()) {
    if (point_id.has_value()) {
      session->last_point = point_id;
    }
    return;
  }

  auto writer = record.writer.value();
  writer.command_index += session->frame_command_base;
  for (auto& member : writer.members) {
    member.command_index += session->frame_command_base;
  }
  auto before_point = point_id;
  if (!before_point.has_value() && session->last_point.has_value()) {
    if (const auto* last = FindPoint(session, session->last_point.value()); last != nullptr && last->image == record.image) {
      before_point = session->last_point;
    }
  }
  session->last_point.reset();
  if (session->warming_up || session->closing || !MatchesFilter(*session, writer)) return;

  session->events.push_back(Event{
      .writer = std::move(writer),
      .frame_index = session->frames_recorded,
      .image = record.image,
      .before_point = before_point,
      .before_reason = (before_point.has_value() ? UnmeasuredReason::NONE : unmeasured_reason),
      .cross_queue = cross_queue,
  });
  session->open_event = session->events_base + session->events.size() - 1u;
}

inline void DestroySessionObjects(Session* session, reshade::api::device* device) {
  if (session->device_api == reshade::api::device_api::opengl) {
    // Some OpenGL games delete the context before ReShade destroys the runtime (see CanDestroyResourcesForDevice), and a
    // helper context may be current instead. GL names belong to the share group of the session's context: deleting from
    // any other context could hit the game's own objects, so they are leaked and left to the context teardown.
    auto& opengl = session->opengl;
    if (opengl.functions == nullptr || opengl.functions->get_current_context() != opengl.context) {
      if (opengl.functions != nullptr) {
        reshade::log::message(reshade::log::level::warning, "devkit::watch_region(the OpenGL context of the watch is not current; its GL objects are not deleted)");
      }
      session->scratch = {0u};
      session->stats_texture = {0u};
      session->readbacks = {};
      session->verify_readback = {0u};
      opengl.pipeline = {0u};
      opengl.atlas_view = {0u};
      opengl.stats_view = {0u};
      session->stats_timeline.reset();
      return;
    }
    if (opengl.pipeline.handle != 0u) {
      device->destroy_pipeline(opengl.pipeline);
      opengl.pipeline = {0u};
    }
    if (opengl.atlas_view.handle != 0u) {
      device->destroy_resource_view(opengl.atlas_view);
      opengl.atlas_view = {0u};
    }
    if (opengl.stats_view.handle != 0u) {
      device->destroy_resource_view(opengl.stats_view);
      opengl.stats_view = {0u};
    }
  }
  session->pass.DestroyAll(device);
  if (session->scratch_srv.handle != 0u) {
    device->destroy_resource_view(session->scratch_srv);
    session->scratch_srv = {0u};
  }
  if (session->stats_uav.handle != 0u) {
    device->destroy_resource_view(session->stats_uav);
    session->stats_uav = {0u};
  }
  if (session->scratch.handle != 0u) {
    device->destroy_resource(session->scratch);
    session->scratch = {0u};
  }
  if (session->stats_texture.handle != 0u) {
    device->destroy_resource(session->stats_texture);
    session->stats_texture = {0u};
  }
  for (auto& readback : session->readbacks) {
    if (readback.handle == 0u) continue;
    device->destroy_resource(readback);
    readback = {0u};
  }
  if (session->verify_readback.handle != 0u) {
    device->destroy_resource(session->verify_readback);
    session->verify_readback = {0u};
  }
  for (const auto& level : session->d3d9.levels) {
    for (const auto target : level.targets) {
      if (target.handle != 0u) {
        device->destroy_resource(target);
      }
    }
  }
  session->d3d9.levels.clear();
  session->d3d9.state_block.reset();
  session->d3d9.vertex_declaration.reset();
  session->d3d9.vertex_shader.reset();
  session->d3d9.pixel_shader.reset();
  session->stats_timeline.reset();
}

// Under the watcher lock: removes the running session from the watcher, so no hook reaches it any more. D3D12/Vulkan
// scratch textures still owned by a command list may be copied into again if the game executes that list later, and
// D3D12 ones with submissions may still be written by the GPU: they move to the watcher's retired list and are
// destroyed when both are over. The other scratch textures go to `released_textures`, to be destroyed after releasing
// the lock.
[[nodiscard]] inline std::shared_ptr<Session> DetachSession(
    Watcher* watcher,
    std::vector<reshade::api::resource>* released_textures) {
  auto session = std::move(watcher->session);
  watcher->watched_resource_handle.store(0u, std::memory_order_release);
  watcher->has_session.store(false, std::memory_order_release);
  for (auto& entry : session->scratch_entries) {
    if (entry.texture.handle == 0u) continue;
    if (entry.in_use && (entry.owner_list != 0u || !entry.submissions.empty())) {
      watcher->retired_scratch.push_back(RetiredScratch{
          .texture = entry.texture,
          .owner_list = entry.owner_list,
          .submissions = std::move(entry.submissions),
      });
    } else {
      released_textures->push_back(entry.texture);
    }
    entry.texture = {0u};
  }
  watcher->has_retired_scratch.store(!watcher->retired_scratch.empty(), std::memory_order_release);
  return session;
}

// Completes a request that cannot finish (its queue or device went away) and wakes the waiting tool call.
inline void FailRequest(const std::shared_ptr<Request>& request, const std::string& message) {
  {
    std::scoped_lock request_lock(request->mutex);
    request->result = ToolResult{
        .text = message,
        .structured_content = json{{"error", message}},
        .is_error = true,
    };
    request->completed = true;
  }
  request->cv.notify_all();
}

// Vulkan: signals the stats fence for the in-flight frame that waits for it (Session::stats_signal_pending). On the
// presenting thread, inside a present on the session's queue.
inline void SignalPendingStatsFrame(Session* session, reshade::api::command_queue* queue) {
  if (!session->stats_signal_pending) return;
  session->stats_signal_pending = false;
  const auto value = SignalTimeline(session->stats_timeline.get(), queue);
  if (value == 0u) {
    session->stats_signal_failed = true;
    return;
  }
  for (auto& frame : session->in_flight) {
    if (frame.fence_value == 0u) {
      frame.fence_value = value;
    }
  }
}

// D3D11: reduces the watched region into the next stats slot and returns that slot. D3D9 and OpenGL: copy the region into
// that slot of the atlas; the frame end reduces it. Must run with the session locked.
[[nodiscard]] inline std::optional<std::uint32_t> MeasureRegion(Session* session, reshade::api::command_list* cmd_list) {
  if (session->slots_used >= session->slot_capacity) {
    session->truncated = true;
    return std::nullopt;
  }

  if (session->device_api == reshade::api::device_api::d3d9) {
    // A native StretchRect raises no ReShade event and changes no device state, and its result tells whether this
    // source can be copied at all.
    const auto source = internal::GetD3D9Surface(session->watched_resource, session->mip_level);
    const auto atlas = internal::GetD3D9Surface(session->scratch, 0u);
    if (source == nullptr || atlas == nullptr) {
      session->failure = "The watched texture or the measurement atlas has no Direct3D 9 surface.";
      return std::nullopt;
    }
    const auto slot = session->slots_used++;
    const auto& region = session->region;
    const RECT source_rect = {
        .left = static_cast<LONG>(region.left),
        .top = static_cast<LONG>(region.top),
        .right = static_cast<LONG>(region.right),
        .bottom = static_cast<LONG>(region.bottom),
    };
    const auto cell_left = static_cast<LONG>((slot % session->d3d9.atlas_columns) * session->width);
    const auto cell_top = static_cast<LONG>((slot / session->d3d9.atlas_columns) * session->height);
    const RECT cell_rect = {
        .left = cell_left,
        .top = cell_top,
        .right = cell_left + static_cast<LONG>(session->width),
        .bottom = cell_top + static_cast<LONG>(session->height),
    };
    auto* native_device = reinterpret_cast<IDirect3DDevice9*>(static_cast<std::uintptr_t>(cmd_list->get_device()->get_native()));
    const auto result = native_device->StretchRect(source.get(), &source_rect, atlas.get(), &cell_rect, D3DTEXF_POINT);
    if (FAILED(result)) {
      session->failure = std::format(
          "StretchRect cannot copy the watched {} texture into the measurement atlas (HRESULT 0x{:08X}): this source is"
          " not supported on Direct3D 9.",
          session->resource_format_name,
          static_cast<std::uint32_t>(result));
      return std::nullopt;
    }
    return slot;
  }

  if (session->device_api == reshade::api::device_api::opengl) {
    // ReShade copies through glCopyImageSubData, which changes no GL state; a copy from the default framebuffer is a blit
    // that leaves the default framebuffer's read buffer at GL_BACK.
    auto& opengl = session->opengl;
    const auto& functions = *opengl.functions;
    if (!opengl.first_copy_checked) {
      // Errors the game left pending are drained once, so that an error after the first copy is the copy's own.
      DrainOpenGLErrors(functions);
    }
    const auto slot = session->slots_used++;
    const auto cell_left = (slot % opengl.atlas_columns) * session->width;
    const auto cell_top = (slot / opengl.atlas_columns) * session->height;
    const auto cell = reshade::api::subresource_box{
        .left = cell_left,
        .top = cell_top,
        .front = 0u,
        .right = cell_left + session->width,
        .bottom = cell_top + session->height,
        .back = 1u,
    };
    cmd_list->copy_texture_region(session->watched_resource, session->subresource, &session->region, session->scratch, 0u, &cell);
    unsigned int copy_error = 0u;
    if (!opengl.first_copy_checked) {
      opengl.first_copy_checked = true;
      copy_error = functions.get_error();
    }
    RestoreDefaultReadBuffer(opengl);
    if (copy_error != 0u) {
      session->failure = std::format(
          "ReShade cannot copy the watched {} texture into the measurement atlas (OpenGL error 0x{:04X}): this source is"
          " not supported on OpenGL.",
          session->resource_format_name,
          copy_error);
      return std::nullopt;
    }
    return slot;
  }

  if (!session->stats_cleared) {
    const std::array<std::uint32_t, 4> zeros = {};
    cmd_list->clear_unordered_access_view_uint(session->stats_uav, zeros.data());
    session->stats_cleared = true;
  }

  cmd_list->copy_texture_region(session->watched_resource, session->subresource, &session->region, session->scratch, 0u, nullptr);

  const auto slot = session->slots_used++;
  session->constants = {
      std::bit_cast<float>(session->width),
      std::bit_cast<float>(session->height),
      std::bit_cast<float>(static_cast<std::uint32_t>(session->signal)),
      std::bit_cast<float>(slot),
  };

  // RenderPass can only restore state through ReShade's snapshot tracking, which the Devkit does not enable,
  // so the compute-stage slots this pass overwrites are saved and restored directly.
  auto* native_context = reinterpret_cast<ID3D11DeviceContext*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
  ID3D11ComputeShader* previous_shader = nullptr;
  ID3D11Buffer* previous_constant_buffer = nullptr;
  ID3D11ShaderResourceView* previous_srv = nullptr;
  ID3D11UnorderedAccessView* previous_uav = nullptr;
  native_context->CSGetShader(&previous_shader, nullptr, nullptr);
  native_context->CSGetConstantBuffers(0u, 1u, &previous_constant_buffer);
  native_context->CSGetShaderResources(0u, 1u, &previous_srv);
  native_context->CSGetUnorderedAccessViews(0u, 1u, &previous_uav);

  const auto rendered = session->pass.Render(cmd_list);

  const UINT keep_append_counter = 0xFFFFFFFFu;
  native_context->CSSetShader(previous_shader, nullptr, 0u);
  native_context->CSSetConstantBuffers(0u, 1u, &previous_constant_buffer);
  native_context->CSSetShaderResources(0u, 1u, &previous_srv);
  native_context->CSSetUnorderedAccessViews(0u, 1u, &previous_uav, &keep_append_counter);
  if (previous_shader != nullptr) {
    previous_shader->Release();
  }
  if (previous_constant_buffer != nullptr) {
    previous_constant_buffer->Release();
  }
  if (previous_srv != nullptr) {
    previous_srv->Release();
  }
  if (previous_uav != nullptr) {
    previous_uav->Release();
  }

  if (!rendered) {
    session->failure = "The region reduction pass failed to render.";
    return std::nullopt;
  }
  return slot;
}

// D3D11 immediate context and D3D9: recording order is execution order, so the record is committed at once. Writers
// outside the filter are only measured to close an open event.
inline void RecordImmediateWriter(Session* session, reshade::api::command_list* cmd_list, const WriterInfo& writer, bool matches_filter) {
  if (!matches_filter && !session->open_event.has_value()) return;

  auto record = Record{.writer = writer};
  // The last slot stays free for the measurement taken at the end of the frame.
  if (session->slots_used + 1u >= session->slot_capacity) {
    session->truncated = true;
    record.reason = UnmeasuredReason::SLOT_LIMIT;
  } else {
    record.measurement = MeasureRegion(session, cmd_list);
    if (!record.measurement.has_value()) return;
  }
  Commit(session, record, UnmeasuredReason::NONE);
}

// D3D12/Vulkan: the per-list state, created on first use. A list seen for the first time without a reset after the
// watch started was already recording, so its render pass state is unknown and nothing in it is measured.
[[nodiscard]] inline ListState* GetListState(Session* session, reshade::api::command_list* cmd_list) {
  const auto handle = reinterpret_cast<std::uint64_t>(cmd_list);
  auto [iterator, inserted] = session->lists.try_emplace(handle);
  if (inserted && session->device_api == reshade::api::device_api::d3d12) {
    auto* native_cmd_list = reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
    const auto type = native_cmd_list->GetType();
    iterator->second.direct = (type == D3D12_COMMAND_LIST_TYPE_DIRECT);
    iterator->second.bundle = (type == D3D12_COMMAND_LIST_TYPE_BUNDLE);
  }
  return &iterator->second;
}

// D3D12/Vulkan: records a writer (or, without `writer`, a pure measurement point) of the watched `image` in its command
// list. `state` is the exactly known state of the watched subresource at this point, or nullopt with the reason it is
// not known.
// Writers are always measured when their state is known; outside the shaderHash filter only when the measurement can
// close a matching writer (the first record of a list, or right after a writer of the same list). Runs under the
// watcher lock and only reserves the measurement; the caller records it with RecordRegionCopy after unlocking.
[[nodiscard]] inline std::optional<PendingCopy> RecordExplicit(
    const std::shared_ptr<Session>& session_owner,
    reshade::api::command_list* cmd_list,
    std::uint64_t image,
    std::optional<WriterInfo> writer,
    std::optional<reshade::api::resource_usage> state,
    UnmeasuredReason unknown_state_reason) {
  auto* session = session_owner.get();
  auto* list = GetListState(session, cmd_list);
  if (list->bundle) return std::nullopt;

  auto record = Record{.writer = std::move(writer), .image = image};

  auto reason = UnmeasuredReason::NONE;
  if (!list->tracked) {
    reason = UnmeasuredReason::UNTRACKED_COMMAND_LIST;
  } else if (!list->direct) {
    reason = UnmeasuredReason::OTHER_QUEUE;
  } else if (list->in_render_pass) {
    reason = UnmeasuredReason::INSIDE_RENDER_PASS;
  } else if (!state.has_value()) {
    reason = unknown_state_reason;
  }

  const auto previous_is_writer = (!list->records.empty() && list->records.back().writer.has_value());
  const auto previous_is_matching_writer = previous_is_writer && MatchesFilter(*session, list->records.back().writer.value());
  bool wanted = false;
  if (record.writer.has_value()) {
    wanted = list->records.empty() || previous_is_matching_writer || MatchesFilter(*session, record.writer.value());
  } else {
    // A pure point only adds information after a writer of this list, or as the first record (it may close a writer
    // of an earlier list and is the "before" of a later render pass).
    wanted = list->records.empty() || previous_is_writer;
  }

  std::optional<PendingCopy> pending = std::nullopt;
  if (reason == UnmeasuredReason::NONE && wanted) {
    const auto list_handle = reinterpret_cast<std::uint64_t>(cmd_list);
    const auto [entry_index, reserve_reason] = ReserveScratchEntry(session, list_handle, false);
    if (entry_index.has_value()) {
      record.measurement = entry_index;
      list->scratch_entries.push_back(entry_index.value());
      pending = PendingCopy{
          .session = session_owner,
          .texture = session->scratch_entries[entry_index.value()].texture,
          .image = image,
          .state = state.value(),
      };
    } else {
      record.reason = reserve_reason;
    }
  } else {
    record.reason = reason;
  }

  if (!record.writer.has_value() && !record.measurement.has_value()) return std::nullopt;
  list->records.push_back(std::move(record));
  return pending;
}


}  // namespace internal

// Draw/dispatch hook. Counts the command and measures when it writes the watched texture through a render target
// (draws) or a compute UAV (dispatches).
inline void OnDrawOrDispatch(Watcher* watcher, reshade::api::command_list* cmd_list, bool is_dispatch) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  const auto device_api = cmd_list->get_device()->get_api();
  const auto explicit_api = internal::IsExplicitApi(device_api);
  if (!explicit_api && !internal::IsImmediateCommandList(cmd_list)) return;
  if (internal::IsOtherOpenGLContext(*watcher)) return;

  WatcherLock lock(watcher, "OnDrawOrDispatch");
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;
  // Re-checked under the lock: the texture may have been destroyed while this thread waited.
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;

  auto* device = cmd_list->get_device();

  if (explicit_api) {
    auto* list = internal::GetListState(session, cmd_list);
    if (list->bundle) return;
    const auto command_index = list->command_count++;

    if (is_dispatch) {
      // The dispatch writes the watched subresource when it is bound as a compute UAV, through root/push descriptors or
      // through a descriptor table range visible to the compute stage.
      std::uint64_t written_image = 0u;
      for (std::uint32_t slot = 0u; slot < MAX_UAV_SLOTS && written_image == 0u; ++slot) {
        const auto view = renodx::utils::state::GetBoundResourceView(
            cmd_list, reshade::api::shader_stage::compute, reshade::api::descriptor_type::unordered_access_view, slot);
        written_image = internal::GetWatchedImageOfView(*session, device, view);
      }
      const auto bound_as_uav_slot = (written_image != 0u);
      std::optional<renodx::utils::state::BoundDescriptorTables> bound_tables = std::nullopt;
      if (written_image == 0u) {
        bound_tables = renodx::utils::state::CopyBoundDescriptorTables(cmd_list, renodx::utils::state::PipelineBindPoint::COMPUTE);
      }
      auto* descriptor_data = renodx::utils::data::Get<renodx::utils::descriptor::DeviceData>(device);
      if (bound_tables.has_value() && bound_tables->layout.handle != 0u && descriptor_data != nullptr) {
        renodx::utils::pipeline_layout::GetPipelineLayoutData(bound_tables->layout, [&](const auto* layout_data) {
          for (const auto& [param_index, table] : bound_tables->tables.entries()) {
            if (written_image != 0u || table.handle == 0u || param_index >= layout_data->params.size()) continue;
            const auto scan_range = [&](const reshade::api::descriptor_range& range) {
              if (written_image != 0u) return;
              if (range.type != reshade::api::descriptor_type::texture_unordered_access_view
                  && range.type != reshade::api::descriptor_type::unordered_access_view) return;
              if (!renodx::utils::bitwise::HasFlag(range.visibility, reshade::api::shader_stage::compute)) return;

              reshade::api::descriptor_heap heap = {0u};
              std::uint32_t base_offset = 0u;
              if (!renodx::utils::descriptor::TryGetDescriptorHeapOffset(device, table, range.binding, 0u, &heap, &base_offset)) return;
              const std::shared_lock descriptor_lock(descriptor_data->mutex);
              const auto heap_pair = descriptor_data->heaps.find(heap.handle);
              if (heap_pair == descriptor_data->heaps.end() || base_offset >= heap_pair->second.size()) return;
              const auto& heap_slots = heap_pair->second;
              const auto available = static_cast<std::uint32_t>(heap_slots.size() - base_offset);
              const auto scanned = std::min({(range.count == UINT32_MAX ? available : range.count), available, MAX_SCANNED_TABLE_DESCRIPTORS});
              for (std::uint32_t index = 0u; index < scanned && written_image == 0u; ++index) {
                const auto& descriptor = heap_slots[base_offset + index];
                if (!descriptor.HasResourceView()) continue;
                written_image = internal::GetWatchedImageOfView(*session, device, descriptor.resource_view);
              }
            };
            const auto& param = layout_data->params[param_index];
            switch (param.type) {
              case reshade::api::pipeline_layout_param_type::descriptor_table:
                for (const auto& range : std::span(param.descriptor_table.ranges, param.descriptor_table.count)) {
                  scan_range(range);
                }
                break;
              case reshade::api::pipeline_layout_param_type::descriptor_table_with_static_samplers:
                for (const auto& range : std::span(param.descriptor_table_with_static_samplers.ranges, param.descriptor_table_with_static_samplers.count)) {
                  scan_range(range);
                }
                break;
              default:
                break;
            }
          }
        });
      }
      if (written_image == 0u) return;
      // Only a UAV slot binding puts the texture in UNORDERED_ACCESS state for this dispatch. A descriptor table hit is
      // reported as a possible writer with no measurement, because a barrier from a guessed state is invalid.
      std::optional<reshade::api::resource_usage> dispatch_state = std::nullopt;
      if (bound_as_uav_slot) {
        dispatch_state = reshade::api::resource_usage::unordered_access;
      }
      auto pending = internal::RecordExplicit(
          watcher->session,
          cmd_list,
          written_image,
          WriterInfo{
              .kind = WriterKind::DISPATCH,
              .command_index = command_index,
              .compute_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::compute_shader),
              .snapshot_command = internal::PeekSnapshotCommand(*watcher, *session, cmd_list),
          },
          dispatch_state,
          UnmeasuredReason::DESCRIPTOR_TABLE);
      lock.Unlock();
      if (pending.has_value()) {
        internal::RecordRegionCopy(*pending->session, cmd_list, pending->image, pending->texture, pending->state);
      }
      return;
    }

    // Inside a render pass that has the watched texture as attachment, the draw is a member of that render pass.
    if (list->in_render_pass) {
      if (!list->render_pass_record.has_value()) return;
      auto& render_pass = list->records[list->render_pass_record.value()].writer.value();
      ++render_pass.member_count;
      if (render_pass.members.size() < MAX_RENDER_PASS_MEMBERS) {
        render_pass.members.push_back(DrawMember{
            .command_index = command_index,
            .pixel_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader),
            .vertex_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader),
            .snapshot_command = internal::PeekSnapshotCommand(*watcher, *session, cmd_list),
        });
      }
      return;
    }

    std::uint64_t written_image = 0u;
    for (const auto& view : renodx::utils::swapchain::GetRenderTargets(cmd_list)) {
      written_image = internal::GetWatchedImageOfView(*session, device, view);
      if (written_image != 0u) break;
    }
    if (written_image == 0u) return;
    // D3D12 draws outside a render pass need their render targets in RENDER_TARGET state. Vulkan draws are always
    // inside a render pass; one seen here comes from a list whose render pass state is not tracked.
    std::optional<reshade::api::resource_usage> draw_state = std::nullopt;
    if (session->device_api == reshade::api::device_api::d3d12) {
      draw_state = reshade::api::resource_usage::render_target;
    }
    auto pending = internal::RecordExplicit(
        watcher->session,
        cmd_list,
        written_image,
        WriterInfo{
            .kind = WriterKind::DRAW,
            .command_index = command_index,
            .pixel_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader),
            .vertex_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader),
            .snapshot_command = internal::PeekSnapshotCommand(*watcher, *session, cmd_list),
        },
        draw_state,
        UnmeasuredReason::STATE_UNKNOWN);
    lock.Unlock();
    if (pending.has_value()) {
      internal::RecordRegionCopy(*pending->session, cmd_list, pending->image, pending->texture, pending->state);
    }
    return;
  }

  // OpenGL has one command list per context; only the context that presents the swap chain is measured.
  if (session->device_api == reshade::api::device_api::opengl
      && session->opengl.functions->get_current_context() != session->opengl.context) return;

  const auto command_index = session->command_count++;

  const auto make_writer = [&]() {
    return WriterInfo{
        .kind = (is_dispatch ? WriterKind::DISPATCH : WriterKind::DRAW),
        .command_index = command_index,
        .pixel_shader_hash = (is_dispatch ? 0u : renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader)),
        .vertex_shader_hash = (is_dispatch ? 0u : renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader)),
        .compute_shader_hash = (is_dispatch ? renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::compute_shader) : 0u),
        .snapshot_command = internal::PeekSnapshotCommand(*watcher, *session, cmd_list),
    };
  };

  bool writes_watched_resource = false;
  if (session->device_api == reshade::api::device_api::opengl) {
    // ReShade raises no event for glDrawBuffers or DSA attachment changes, so utils::state would miss writers: GL itself
    // says what the draw framebuffer and the image units are. Each query can synchronise with a threaded driver, so draws
    // that could not be measured anyway (outside the shader filter, no event open) are skipped first.
    if (session->shader_hash.has_value() && !session->open_event.has_value() && !internal::MatchesFilter(*session, make_writer())) return;

    const auto& opengl = session->opengl;
    const auto& functions = *opengl.functions;
    const auto is_watched_object = [&](int object_type, int name) {
      const auto matches = [&](std::uint64_t handle) {
        const auto target = static_cast<unsigned int>(handle >> 40u);
        const auto is_renderbuffer = (target == gl_enum::RENDERBUFFER);
        if (is_renderbuffer != (object_type == static_cast<int>(gl_enum::RENDERBUFFER))) return false;
        return target != gl_enum::FRAMEBUFFER_DEFAULT && static_cast<int>(handle & 0xFFFFFFFFu) == name;
      };
      return name != 0 && (matches(session->watched_resource.handle) || matches(session->original_resource.handle));
    };
    if (is_dispatch) {
      for (std::uint32_t unit = 0u; unit < opengl.max_image_units && !writes_watched_resource; ++unit) {
        int name = 0;
        int access = 0;
        functions.get_integeri_v(gl_enum::IMAGE_BINDING_NAME, unit, &name);
        functions.get_integeri_v(gl_enum::IMAGE_BINDING_ACCESS, unit, &access);
        if (name == 0 || access == static_cast<int>(gl_enum::READ_ONLY) || !is_watched_object(static_cast<int>(gl_enum::TEXTURE), name)) continue;
        int level = 0;
        int layered = 0;
        int layer = 0;
        functions.get_integeri_v(gl_enum::IMAGE_BINDING_LEVEL, unit, &level);
        functions.get_integeri_v(gl_enum::IMAGE_BINDING_LAYERED, unit, &layered);
        functions.get_integeri_v(gl_enum::IMAGE_BINDING_LAYER, unit, &layer);
        writes_watched_resource = (level == static_cast<int>(session->mip_level) && (layered != 0 || layer == static_cast<int>(session->array_layer)));
      }
    } else {
      int draw_framebuffer = 0;
      functions.get_integerv(gl_enum::DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
      if (draw_framebuffer == 0) {
        // Only the back buffers count: a draw into the front buffer does not change the watched image.
        int draw_buffer = 0;
        functions.get_integerv(gl_enum::DRAW_BUFFER0, &draw_buffer);
        switch (static_cast<unsigned int>(draw_buffer)) {
          case gl_enum::BACK:
          case gl_enum::BACK_LEFT:
          case gl_enum::BACK_RIGHT:
          case gl_enum::LEFT:
          case gl_enum::RIGHT:
          case gl_enum::FRONT_AND_BACK:
            writes_watched_resource = opengl.watches_default_framebuffer;
            break;
          default:
            break;
        }
      } else {
        for (std::uint32_t index = 0u; index < opengl.max_draw_buffers && !writes_watched_resource; ++index) {
          int draw_buffer = 0;
          functions.get_integerv(gl_enum::DRAW_BUFFER0 + index, &draw_buffer);
          if (draw_buffer == static_cast<int>(gl_enum::NONE)) continue;
          int object_type = 0;
          int name = 0;
          functions.get_framebuffer_attachment_parameteriv(gl_enum::DRAW_FRAMEBUFFER, static_cast<unsigned int>(draw_buffer), gl_enum::ATTACHMENT_OBJECT_TYPE, &object_type);
          functions.get_framebuffer_attachment_parameteriv(gl_enum::DRAW_FRAMEBUFFER, static_cast<unsigned int>(draw_buffer), gl_enum::ATTACHMENT_OBJECT_NAME, &name);
          if (!is_watched_object(object_type, name)) continue;
          if (object_type == static_cast<int>(gl_enum::RENDERBUFFER)) {
            writes_watched_resource = true;
            continue;
          }
          int level = 0;
          int layer = 0;
          int layered = 0;
          functions.get_framebuffer_attachment_parameteriv(gl_enum::DRAW_FRAMEBUFFER, static_cast<unsigned int>(draw_buffer), gl_enum::ATTACHMENT_TEXTURE_LEVEL, &level);
          functions.get_framebuffer_attachment_parameteriv(gl_enum::DRAW_FRAMEBUFFER, static_cast<unsigned int>(draw_buffer), gl_enum::ATTACHMENT_TEXTURE_LAYER, &layer);
          functions.get_framebuffer_attachment_parameteriv(gl_enum::DRAW_FRAMEBUFFER, static_cast<unsigned int>(draw_buffer), gl_enum::ATTACHMENT_LAYERED, &layered);
          writes_watched_resource = (level == static_cast<int>(session->mip_level) && (layered != 0 || layer == static_cast<int>(session->array_layer)));
        }
      }
    }
  } else if (is_dispatch) {
    for (std::uint32_t slot = 0u; slot < MAX_UAV_SLOTS && !writes_watched_resource; ++slot) {
      const auto view = renodx::utils::state::GetBoundResourceView(
          cmd_list, reshade::api::shader_stage::compute, reshade::api::descriptor_type::unordered_access_view, slot);
      if (view.handle == 0u) continue;
      writes_watched_resource = device->get_resource_from_view(view).handle == session->watched_resource.handle;
    }
  } else {
    for (const auto& view : renodx::utils::state::GetRenderTargets(cmd_list)) {
      if (view.handle == 0u) continue;
      if (device->get_resource_from_view(view).handle == session->watched_resource.handle) {
        writes_watched_resource = true;
        break;
      }
    }
    // D3D11 pixel shaders also write through UAVs bound with OMSetRenderTargetsAndUnorderedAccessViews.
    if (session->device_api == reshade::api::device_api::d3d11) {
      for (std::uint32_t slot = 0u; slot < MAX_UAV_SLOTS && !writes_watched_resource; ++slot) {
        const auto view = renodx::utils::state::GetBoundResourceView(
            cmd_list, reshade::api::shader_stage::pixel, reshade::api::descriptor_type::unordered_access_view, slot);
        if (view.handle == 0u) continue;
        writes_watched_resource = device->get_resource_from_view(view).handle == session->watched_resource.handle;
      }
    }
  }
  if (!writes_watched_resource) return;

  const auto writer = make_writer();
  internal::RecordImmediateWriter(session, cmd_list, writer, internal::MatchesFilter(*session, writer));
}


// Copy, clear, resolve and update hook. Only copies count as numbered commands because those are the ones the
// snapshot draw list also contains. `cmd_list` is nullptr for device-level events (D3D11 immediate-context updates).
inline void OnResourceWrite(Watcher* watcher, reshade::api::command_list* cmd_list, WriterKind kind, const WrittenTarget& target) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  const auto explicit_api = (cmd_list != nullptr && internal::IsExplicitApi(cmd_list->get_device()->get_api()));
  if (cmd_list != nullptr && !explicit_api && !internal::IsImmediateCommandList(cmd_list)) return;
  if (internal::IsOtherOpenGLContext(*watcher)) return;

  WatcherLock lock(watcher, "OnResourceWrite");
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;
  // Re-checked under the lock: the texture may have been destroyed while this thread waited.
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  // D3D12/Vulkan device-level updates have no place in the command stream (documented limit).
  if (cmd_list == nullptr && session->explicit_api) return;
  // OpenGL: events of other contexts (loader threads, helper windows), device-level updates included, are not measured.
  if (session->device_api == reshade::api::device_api::opengl
      && session->opengl.functions->get_current_context() != session->opengl.context) return;

  if (explicit_api) {
    auto* list = internal::GetListState(session, cmd_list);
    if (list->bundle) return;
    const auto command_index = list->command_count;
    if (kind == WriterKind::COPY) {
      ++list->command_count;
    }
    std::uint64_t written_image = 0u;
    if (target.view.handle != 0u) {
      written_image = internal::GetWatchedImageOfView(*session, cmd_list->get_device(), target.view);
    } else if (!target.subresource.has_value() || target.subresource.value() == session->subresource) {
      written_image = internal::GetWatchedImage(*session, target.resource.handle);
    }
    if (written_image == 0u) return;

    // Clears give an exact state: on D3D12 the view must be in that state; on Vulkan ReShade transitions the cleared
    // ranges to COLOR_ATTACHMENT_OPTIMAL around the event but does not report the ranges, so only an image with a
    // single subresource is known to be in it. Copy and resolve destinations are ambiguous (D3D12 promotion from
    // COMMON; Vulkan GENERAL or TRANSFER_DST), so the state before them comes from the previous point.
    auto state = target.state;
    if (session->device_api == reshade::api::device_api::vulkan && !session->single_subresource) {
      state.reset();
    }
    // Only copies are snapshot commands.
    const auto writer = WriterInfo{
        .kind = kind,
        .command_index = command_index,
        .snapshot_command = (kind == WriterKind::COPY ? internal::PeekSnapshotCommand(*watcher, *session, cmd_list) : std::nullopt),
    };
    auto pending = internal::RecordExplicit(watcher->session, cmd_list, written_image, writer, state, UnmeasuredReason::STATE_UNKNOWN);
    lock.Unlock();
    if (pending.has_value()) {
      internal::RecordRegionCopy(*pending->session, cmd_list, pending->image, pending->texture, pending->state);
    }
    return;
  }

  const auto command_index = session->command_count;
  if (kind == WriterKind::COPY) {
    ++session->command_count;
  }
  auto* recording_cmd_list = (cmd_list != nullptr ? cmd_list : session->immediate_cmd_list);
  const auto written_resource = (target.view.handle != 0u ? recording_cmd_list->get_device()->get_resource_from_view(target.view) : target.resource);
  if (written_resource.handle != session->watched_resource.handle
      && written_resource.handle != session->original_resource.handle) return;
  // OpenGL uploads and copies to other mip levels or layers of the watched texture are common and do not change the region.
  if (session->device_api == reshade::api::device_api::opengl && target.view.handle == 0u && target.subresource.has_value()
      && target.subresource.value() != session->subresource) return;

  const auto writer = WriterInfo{
      .kind = kind,
      .command_index = command_index,
      .snapshot_command = (kind == WriterKind::COPY ? internal::PeekSnapshotCommand(*watcher, *session, cmd_list) : std::nullopt),
  };
  // A filter by shader hash never matches a non-shader writer; it is still measured when it closes an open event.
  internal::RecordImmediateWriter(session, recording_cmd_list, writer, !session->shader_hash.has_value());
}

// D3D12/Vulkan begin_render_pass hook. A render pass (Vulkan: each subpass) with the watched texture as attachment is
// one writer; the draws inside are its members, because no copy is allowed inside a render pass. D3D12 render targets
// are in RENDER_TARGET state at BeginRenderPass. On Vulkan the layout here is unknown (ReShade raises this event for
// vkCmdBeginRenderPass, vkCmdNextSubpass and inherited secondary command buffers alike, and does not expose the
// attachment's initial layout), so the state before it is taken from the previous point.
inline void OnBeginRenderPass(
    Watcher* watcher,
    reshade::api::command_list* cmd_list,
    std::span<const reshade::api::render_pass_render_target_desc> render_targets) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsExplicitApi(cmd_list->get_device()->get_api())) return;

  WatcherLock lock(watcher, "OnBeginRenderPass");
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;

  auto* list = internal::GetListState(session, cmd_list);
  if (list->bundle) return;
  list->render_pass_record.reset();
  std::optional<PendingCopy> pending = std::nullopt;
  std::uint64_t written_image = 0u;
  for (const auto& render_target : render_targets) {
    written_image = internal::GetWatchedImageOfView(*session, cmd_list->get_device(), render_target.view);
    if (written_image != 0u) break;
  }
  if (written_image != 0u) {
    std::optional<reshade::api::resource_usage> state = std::nullopt;
    if (session->device_api == reshade::api::device_api::d3d12 && !list->in_render_pass) {
      state = reshade::api::resource_usage::render_target;
    }
    pending = internal::RecordExplicit(watcher->session, cmd_list, written_image, WriterInfo{.kind = WriterKind::RENDER_PASS, .command_index = list->command_count}, state, UnmeasuredReason::STATE_UNKNOWN);
    if (!list->records.empty() && list->records.back().writer.has_value()
        && list->records.back().writer->kind == WriterKind::RENDER_PASS) {
      list->render_pass_record = list->records.size() - 1u;
    }
  }
  list->in_render_pass = true;
  lock.Unlock();
  if (pending.has_value()) {
    internal::RecordRegionCopy(*pending->session, cmd_list, pending->image, pending->texture, pending->state);
  }
}

inline void OnEndRenderPass(Watcher* watcher, reshade::api::command_list* cmd_list) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsExplicitApi(cmd_list->get_device()->get_api())) return;

  WatcherLock lock(watcher, "OnEndRenderPass");
  auto* session = watcher->session.get();
  if (session == nullptr) return;
  auto* list = internal::GetListState(session, cmd_list);
  list->in_render_pass = false;
  list->render_pass_record.reset();
}

// Vulkan barrier hook. ReShade raises it after the real vkCmdPipelineBarrier(2), so the watched image is in the new
// layout. It is used as a measurement point only when that layout is exact: ReShade maps every layout it does not know
// to `general`, and the event carries no subresource range, so only single-subresource images qualify. D3D12 barriers
// are not used (no subresource index in the event, and enhanced barriers report access instead of layout).
inline void OnBarrier(
    Watcher* watcher,
    reshade::api::command_list* cmd_list,
    std::span<const reshade::api::resource> resources,
    std::span<const reshade::api::resource_usage> new_states) {
  if (internal::issuing_native_commands) return;
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::vulkan) return;

  WatcherLock lock(watcher, "OnBarrier");
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty() || !session->single_subresource) return;
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;

  std::vector<PendingCopy> pendings;
  for (std::size_t index = 0u; index < resources.size(); ++index) {
    // Only the watched image itself: the original of a watched clone stands for the clone, but its layout is its own.
    // A VkMemoryBarrier (global barrier) is reported as resource 0; without this check it would match the 0 that
    // GetWatchedImage returns for an unwatched handle, and the region copy would read from image 0.
    const auto barrier_image = resources[index].handle;
    if (barrier_image == 0u || internal::GetWatchedImage(*session, barrier_image) != barrier_image) continue;
    switch (new_states[index]) {
      case reshade::api::resource_usage::render_target:
      case reshade::api::resource_usage::shader_resource:
      case reshade::api::resource_usage::copy_source:
      case reshade::api::resource_usage::copy_dest:
      case reshade::api::resource_usage::present:
        if (auto pending = internal::RecordExplicit(watcher->session, cmd_list, barrier_image, std::nullopt, new_states[index], UnmeasuredReason::STATE_UNKNOWN);
            pending.has_value()) {
          pendings.push_back(std::move(pending.value()));
        }
        break;
      default:
        break;
    }
  }
  lock.Unlock();
  for (const auto& pending : pendings) {
    internal::RecordRegionCopy(*pending.session, cmd_list, pending.image, pending.texture, pending.state);
  }
}

// init_command_list and reset_command_list hook (D3D12 lists start recording when created; Vulkan reports
// vkBeginCommandBuffer as a reset). The list can no longer copy into its scratch entries. Vulkan only allows the reset
// once the GPU is done with the list, so the entries are free at once. D3D12 allows it while the GPU still runs the
// list, so an entry the list executed waits for its submissions (GpuSubmission); one it never executed is free at once.
inline void OnCommandListReset(Watcher* watcher, reshade::api::command_list* cmd_list, bool destroyed) {
  const auto list_handle = reinterpret_cast<std::uint64_t>(cmd_list);
  if (watcher->has_retired_scratch.load(std::memory_order_acquire)) {
    std::vector<reshade::api::resource> released;
    {
      WatcherLock lock(watcher, "OnCommandListReset");
      // A texture the GPU may still be writing (D3D12 submissions) stays until the frame end sees its fence passed.
      std::erase_if(watcher->retired_scratch, [&](RetiredScratch& retired) {
        if (retired.owner_list != list_handle) return false;
        retired.owner_list = 0u;
        if (!retired.submissions.empty()) return false;
        released.push_back(retired.texture);
        return true;
      });
      watcher->has_retired_scratch.store(!watcher->retired_scratch.empty(), std::memory_order_release);
    }
    for (const auto texture : released) {
      cmd_list->get_device()->destroy_resource(texture);
    }
  }

  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsExplicitApi(cmd_list->get_device()->get_api())) return;

  WatcherLock lock(watcher, "OnCommandListReset");
  auto* session = watcher->session.get();
  if (session == nullptr) return;

  if (auto pair = session->lists.find(list_handle); pair != session->lists.end()) {
    // An entry still waiting for its reduction is freed right after it.
    for (const auto entry_index : pair->second.scratch_entries) {
      auto& entry = session->scratch_entries[entry_index];
      if (entry.awaiting_reduction) {
        entry.release_after_reduction = true;
        continue;
      }
      entry.owner_list = 0u;
      if (entry.submissions.empty()) {
        entry.in_use = false;
      }
    }
    if (destroyed) {
      session->lists.erase(pair);
      return;
    }
  }
  if (destroyed) return;

  auto* list = internal::GetListState(session, cmd_list);
  list->records.clear();
  list->scratch_entries.clear();
  list->command_count = 0u;
  list->tracked = true;
  list->in_render_pass = false;
  list->render_pass_record.reset();
  list->executed_frame.reset();
  list->committed_points.clear();
}

// D3D12/Vulkan execute_command_list hook: commits the list's records in submission order. Records of a list executed
// on another queue than the presenting one, or executed again without being reset, are accounted for as unmeasured.
// On D3D12 every execution of a list that copies into scratch textures is also recorded as a submission of those
// textures, whatever the watch does with the records, because the GPU runs the copies anyway; this includes the
// retired textures of a finished watch.
inline void OnExecuteCommandList(Watcher* watcher, reshade::api::command_queue* queue, reshade::api::command_list* cmd_list) {
  const auto has_session = watcher->has_session.load(std::memory_order_acquire);
  if (!has_session && !watcher->has_retired_scratch.load(std::memory_order_acquire)) return;
  const auto device_api = cmd_list->get_device()->get_api();
  if (!internal::IsExplicitApi(device_api)) return;

  const auto list_handle = reinterpret_cast<std::uint64_t>(cmd_list);
  WatcherLock lock(watcher, "OnExecuteCommandList");
  const auto submission = GpuSubmission{
      .queue = queue,
      .frame_end_index = watcher->frame_end_index,
  };
  if (device_api == reshade::api::device_api::d3d12) {
    for (auto& retired : watcher->retired_scratch) {
      if (retired.owner_list == list_handle) {
        internal::AddSubmission(&retired.submissions, submission);
      }
    }
  }
  auto* session = watcher->session.get();
  if (session == nullptr) return;

  const auto pair = session->lists.find(list_handle);
  if (pair == session->lists.end()) return;
  auto& list = pair->second;
  if (device_api == reshade::api::device_api::d3d12) {
    for (const auto entry_index : list.scratch_entries) {
      internal::AddSubmission(&session->scratch_entries[entry_index].submissions, submission);
    }
  }
  // Records are only committed while the watched texture exists (destroy_resource gate) and the watch has not failed.
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u || !session->failure.empty()) return;

  // D3D12 direct lists on another queue are measured late (cross-queue points); other queues and Vulkan are refused.
  const auto on_presenting_queue = (queue == session->queue);
  const auto cross_queue = (!on_presenting_queue && device_api == reshade::api::device_api::d3d12 && list.direct);
  auto refused = UnmeasuredReason::NONE;
  if (!on_presenting_queue && !cross_queue) {
    refused = UnmeasuredReason::OTHER_QUEUE;
  } else if (list.executed_frame.has_value()) {
    refused = UnmeasuredReason::RESUBMITTED;
    // Executed again: this execution overwrites the copies of the earlier one, which are lost if they were not reduced
    // yet (an earlier execution in this frame, or a cross-queue point still waiting).
    for (const auto point_id : list.committed_points) {
      auto* point = internal::FindPoint(session, point_id);
      if (point == nullptr) continue;
      if (list.executed_frame.value() == session->frames_recorded || point->deferred) {
        point->refused = UnmeasuredReason::RESUBMITTED;
        // A later point of the closing frames becomes the verify point instead.
        if (session->verify_point.has_value() && session->verify_point.value() == point_id) {
          session->verify_point.reset();
        }
      }
    }
  }

  for (const auto& record : list.records) {
    const auto next_point = session->points_base + session->points.size();
    internal::Commit(session, record, refused, cross_queue);
    if (session->points_base + session->points.size() != next_point) {
      list.committed_points.push_back(next_point);
    }
  }
  session->frame_command_base += list.command_count;
  list.executed_frame = session->frames_recorded;
}

// D3D12 ExecuteBundle / Vulkan vkCmdExecuteCommands hook. Vulkan secondary records are inserted into the primary at
// this position (render pass members are merged into the primary's render pass); a secondary inserted again without
// being reset is a resubmission. D3D12 bundles cannot contain copies or barriers, so a bundle executed while the
// watched texture is a render target is reported as an unmeasured writer.
inline void OnExecuteSecondaryCommandList(Watcher* watcher, reshade::api::command_list* cmd_list, reshade::api::command_list* secondary_cmd_list) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsExplicitApi(cmd_list->get_device()->get_api())) return;

  WatcherLock lock(watcher, "OnExecuteSecondaryCommandList");
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;

  auto* list = internal::GetListState(session, cmd_list);
  if (session->device_api == reshade::api::device_api::d3d12) {
    std::uint64_t written_image = 0u;
    if (list->render_pass_record.has_value()) {
      written_image = list->records[list->render_pass_record.value()].image;
    } else {
      for (const auto& view : renodx::utils::swapchain::GetRenderTargets(cmd_list)) {
        written_image = internal::GetWatchedImageOfView(*session, cmd_list->get_device(), view);
        if (written_image != 0u) break;
      }
    }
    if (written_image != 0u) {
      // No state is known for a bundle writer, so RecordExplicit reserves no copy.
      static_cast<void>(internal::RecordExplicit(watcher->session, cmd_list, written_image, WriterInfo{.kind = WriterKind::BUNDLE, .command_index = list->command_count}, std::nullopt, UnmeasuredReason::BUNDLE));
    }
    return;
  }

  const auto pair = session->lists.find(reinterpret_cast<std::uint64_t>(secondary_cmd_list));
  if (pair == session->lists.end()) return;
  auto& secondary = pair->second;
  const auto inserted_before = secondary.executed_frame.has_value();
  for (auto record : secondary.records) {
    if (inserted_before && record.measurement.has_value()) {
      record.measurement.reset();
      record.reason = UnmeasuredReason::RESUBMITTED;
    }
    if (!record.writer.has_value()) {
      if (record.measurement.has_value()) {
        list->records.push_back(std::move(record));
      }
      continue;
    }
    record.writer->command_index += list->command_count;
    for (auto& member : record.writer->members) {
      member.command_index += list->command_count;
    }
    if (record.writer->kind == WriterKind::RENDER_PASS && list->render_pass_record.has_value()) {
      auto& render_pass = list->records[list->render_pass_record.value()].writer.value();
      render_pass.member_count += record.writer->member_count;
      for (const auto& member : record.writer->members) {
        if (render_pass.members.size() >= MAX_RENDER_PASS_MEMBERS) break;
        render_pass.members.push_back(member);
      }
      continue;
    }
    list->records.push_back(std::move(record));
  }
  list->command_count += secondary.command_count;
  secondary.executed_frame = session->frames_recorded;
}

// destroy_resource hook. Lock-free on purpose: ReShade can fire it while this module already holds the watcher lock
// (for example when a released reference was the last one). Clearing the gate stops all measuring at once; the next
// frame end reports the watch as failed.
inline void OnResourceDestroyed(Watcher* watcher, reshade::api::resource resource) {
  auto expected_handle = resource.handle;
  if (expected_handle == 0u) return;
  watcher->watched_resource_handle.compare_exchange_strong(expected_handle, 0u, std::memory_order_acq_rel);
}

// finish_present hook (Vulkan): signals the fence of the frame that ReShade just submitted with the present.
inline void OnFinishPresent(Watcher* watcher, reshade::api::command_queue* queue) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;

  std::shared_ptr<Session> session;
  {
    WatcherLock lock(watcher, "OnFinishPresent");
    session = watcher->session;
  }
  // The fence state belongs to the presenting thread (ProcessAtFrameEnd and this hook, both inside present), and the
  // signal runs inside present, where ReShade holds the queue's lock, so the game cannot submit to it concurrently.
  if (session == nullptr || session->queue != queue) return;
  internal::SignalPendingStatsFrame(session.get(), queue);
}

// destroy_effect_runtime hook. ReShade raises it when a runtime is reset (resize, fullscreen switch) or destroyed,
// possibly outside present. The watch that runtime measures fails: its back buffers may be gone, and frames of
// different sizes would be mixed. Only bookkeeping here; the next frame end on the watch's queue finishes it.
inline void OnEffectRuntimeDestroyed(Watcher* watcher, reshade::api::effect_runtime* runtime) {
  if (!watcher->has_session.load(std::memory_order_acquire)) return;
  WatcherLock lock(watcher, "OnEffectRuntimeDestroyed");
  auto* session = watcher->session.get();
  if (session == nullptr || session->runtime != runtime) return;
  session->runtime = nullptr;
  if (session->failure.empty()) {
    session->failure = "The swap chain the watch measured on was reset or destroyed (resize, fullscreen switch or window closed).";
  }
}

// The tool gave up waiting (it set request->canceled first). A queued request is dropped; a running watch stops
// reserving measurements at once, since the hooks check `failure`, and the next frame end on its queue finishes it.
inline void CancelRequest(Watcher* watcher, const std::shared_ptr<Request>& request) {
  WatcherLock lock(watcher, "CancelRequest");
  std::erase(watcher->pending_requests, request);
  auto* session = watcher->session.get();
  if (session != nullptr && session->request == request && session->failure.empty()) {
    session->failure = "The watch was canceled.";
  }
}

// D3D12, on the presenting thread, with or without a running watch: frees the scratch textures the GPU is done with
// (the running watch may reuse them, retired ones are destroyed). At a frame end (`frame_end`) it also counts the frame
// end, creates the lifetime fences of queues that executed region copies, and covers every submission recorded before
// this frame end with one new fence value per queue, signalled on that queue (GpuSubmission). Runs inside present,
// before the real Present, so a game that submits its frame before presenting has submitted it by then. Fences are
// created and signalled without the watcher lock; a queue's destroy event waits for signals in flight on it.
inline void ProcessLifetimes(Watcher* watcher, reshade::api::device* device, bool frame_end) {
  if (device->get_api() != reshade::api::device_api::d3d12) return;
  // Without a session or retired textures there is no submission to track (the frame end count only matters relative
  // to submissions recorded later).
  const auto has_session = watcher->has_session.load(std::memory_order_acquire);
  if (!has_session && !watcher->has_retired_scratch.load(std::memory_order_acquire)) return;

  // Stage 1 (locked): count the frame end and list the queues that need a fence. Submissions recorded from here on
  // carry the new index and wait for the next frame end.
  std::vector<std::uint64_t> queues_without_fence;
  if (frame_end) {
    WatcherLock lock(watcher, "ProcessLifetimes: counting the frame end");
    ++watcher->frame_end_index;
    internal::ForEachSubmissionList(watcher, [&](const std::vector<GpuSubmission>& submissions) {
      for (const auto& submission : submissions) {
        if (submission.queue == nullptr) continue;
        const auto queue_handle = reinterpret_cast<std::uint64_t>(submission.queue);
        if (watcher->queue_fences.contains(queue_handle)) continue;
        if (std::ranges::find(queues_without_fence, queue_handle) != queues_without_fence.end()) continue;
        queues_without_fence.push_back(queue_handle);
      }
    });
  }

  // Stage 2 (unlocked): create the missing fences.
  std::vector<std::pair<std::uint64_t, std::shared_ptr<FenceTimeline>>> created_timelines;
  for (const auto queue_handle : queues_without_fence) {
    auto timeline = internal::CreateFenceTimeline(device);
    if (timeline != nullptr) {
      created_timelines.emplace_back(queue_handle, std::move(timeline));
    }
  }

  // Stage 3 (locked): install the fences, mark what each new signal will cover, free what the GPU is done with.
  struct PlannedSignal {
    reshade::api::command_queue* queue = nullptr;
    std::shared_ptr<FenceTimeline> timeline;
    std::uint64_t signal_id = 0u;
  };
  std::vector<PlannedSignal> planned_signals;
  // Destroyed after the lock is released, like every fence (see FenceTimeline).
  std::vector<std::shared_ptr<FenceTimeline>> unused_timelines;
  std::vector<reshade::api::resource> released_textures;
  {
    WatcherLock lock(watcher, "ProcessLifetimes: planning the signals");
    // Read before this stage plans new signals, so a texture is only freed for values the GPU had already passed.
    // Each timeline's progress is behind its leaf mutex (FenceCompletion): no driver call under the watcher lock.
    std::unordered_map<std::uint64_t, std::uint64_t> completed_values;
    for (const auto& [queue_handle, queue_fence] : watcher->queue_fences) {
      completed_values[queue_handle] = internal::GetCompletedValue(queue_fence.timeline.get());
    }
    watcher->completed_fence_values = completed_values;

    for (auto& [queue_handle, timeline] : created_timelines) {
      // The queue may have been destroyed meanwhile: its destroy event cleared every submission that named it.
      bool queue_has_submissions = false;
      internal::ForEachSubmissionList(watcher, [&](const std::vector<GpuSubmission>& submissions) {
        for (const auto& submission : submissions) {
          if (reinterpret_cast<std::uint64_t>(submission.queue) == queue_handle) {
            queue_has_submissions = true;
          }
        }
      });
      if (!queue_has_submissions || watcher->queue_fences.contains(queue_handle)) {
        unused_timelines.push_back(std::move(timeline));
        continue;
      }
      watcher->queue_fences.emplace(queue_handle, QueueLifetimeFence{.timeline = std::move(timeline)});
    }

    if (frame_end) {
      for (auto& [fence_queue_handle, queue_fence] : watcher->queue_fences) {
        // Frame ends of several runtimes can run at once: a queue with a signal still in flight is left to the next
        // frame end, so at most one thread signals a queue's fence at a time.
        if (queue_fence.signals_in_flight != 0u) continue;
        const auto queue_handle = fence_queue_handle;
        const auto signal_id = ++watcher->planned_signal_ids;
        bool covers_submissions = false;
        internal::ForEachSubmissionList(watcher, [&](std::vector<GpuSubmission>& submissions) {
          for (auto& submission : submissions) {
            if (reinterpret_cast<std::uint64_t>(submission.queue) != queue_handle || submission.fence_value.has_value()
                || submission.pending_signal != 0u || submission.frame_end_index >= watcher->frame_end_index) {
              continue;
            }
            submission.pending_signal = signal_id;
            covers_submissions = true;
          }
        });
        if (!covers_submissions) continue;
        ++queue_fence.signals_in_flight;
        planned_signals.push_back(PlannedSignal{
            .queue = reinterpret_cast<reshade::api::command_queue*>(queue_handle),
            .timeline = queue_fence.timeline,
            .signal_id = signal_id,
        });
      }
    }

    if (watcher->session != nullptr) {
      for (auto& entry : watcher->session->scratch_entries) {
        // An in-use entry no list owns and no reduction waits for is only held by its submissions (empty once a
        // destroyed queue's wait cleared them).
        if (!entry.in_use || entry.owner_list != 0u || entry.awaiting_reduction) continue;
        if (!internal::AreSubmissionsComplete(entry.submissions, completed_values)) continue;
        entry.submissions.clear();
        entry.in_use = false;
      }
    }
    std::erase_if(watcher->retired_scratch, [&](const RetiredScratch& retired) {
      if (retired.owner_list != 0u || !internal::AreSubmissionsComplete(retired.submissions, completed_values)) return false;
      released_textures.push_back(retired.texture);
      return true;
    });
    watcher->has_retired_scratch.store(!watcher->retired_scratch.empty(), std::memory_order_release);
  }

  // Stage 4 (unlocked): signal. gpu_fence picks the value, so it is attached afterwards.
  std::vector<std::uint64_t> signalled_values;
  for (const auto& planned : planned_signals) {
    const auto value = internal::SignalTimeline(planned.timeline.get(), planned.queue);
    if (value == 0u) {
      reshade::log::message(reshade::log::level::warning, "devkit::watch_region(lifetime fence signal failed; those submissions wait for the next frame end's signal)");
    }
    signalled_values.push_back(value);
  }

  // Stage 5 (locked): attach the values. The queue cannot have been destroyed meanwhile: its destroy event waits until
  // `signals_in_flight` drops to 0, which happens here, under the same lock.
  if (!planned_signals.empty()) {
    WatcherLock lock(watcher, "ProcessLifetimes: attaching fence values");
    for (std::size_t index = 0u; index < planned_signals.size(); ++index) {
      const auto& planned = planned_signals[index];
      const auto value = signalled_values[index];
      const auto queue_handle = reinterpret_cast<std::uint64_t>(planned.queue);
      if (auto pair = watcher->queue_fences.find(queue_handle); pair != watcher->queue_fences.end()) {
        --pair->second.signals_in_flight;
      }
      const auto is_covered = [&](const GpuSubmission& submission) {
        return submission.pending_signal == planned.signal_id;
      };
      internal::ForEachSubmissionList(watcher, [&](std::vector<GpuSubmission>& submissions) {
        if (!std::ranges::any_of(submissions, is_covered)) return;
        if (value != 0u) {
          // The new value also covers this queue's earlier fenced submission (signals on one queue complete in order).
          std::erase_if(submissions, [&](const GpuSubmission& submission) {
            return reinterpret_cast<std::uint64_t>(submission.queue) == queue_handle && submission.fence_value.has_value();
          });
        }
        for (auto& submission : submissions) {
          if (!is_covered(submission)) continue;
          submission.pending_signal = 0u;
          if (value != 0u) {
            submission.fence_value = value;
          }
        }
      });
    }
  }
  for (const auto texture : released_textures) {
    device->destroy_resource(texture);
  }
}

// D3D12, without the lock: waits (LIFETIME_FENCE_WAIT_TIMEOUT per fence) until the GPU passed every fence value
// already attached to a submission. Submissions recorded since the last frame end have no value yet and cannot be
// waited for; those textures are freed by a later frame end.
inline void WaitForAttachedFenceValues(Watcher* watcher, reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d12) return;
  std::vector<std::pair<std::shared_ptr<FenceTimeline>, std::uint64_t>> waits;
  {
    WatcherLock lock(watcher, "WaitForAttachedFenceValues");
    for (const auto& [fence_queue_handle, queue_fence] : watcher->queue_fences) {
      const auto queue_handle = fence_queue_handle;
      std::uint64_t highest_value = 0u;
      internal::ForEachSubmissionList(watcher, [&](const std::vector<GpuSubmission>& submissions) {
        for (const auto& submission : submissions) {
          if (reinterpret_cast<std::uint64_t>(submission.queue) == queue_handle && submission.fence_value.has_value()) {
            highest_value = std::max(highest_value, submission.fence_value.value());
          }
        }
      });
      if (highest_value != 0u) {
        waits.emplace_back(queue_fence.timeline, highest_value);
      }
    }
  }
  bool all_reached = true;
  for (const auto& [timeline, value] : waits) {
    if (!internal::WaitTimeline(timeline.get(), value)) {
      all_reached = false;
    }
  }
  if (!all_reached) {
    reshade::log::message(reshade::log::level::warning, "devkit::watch_region(lifetime fence wait timed out at the end of a watch; those scratch textures are freed by a later frame end)");
  }
}

// D3D12 destroy_command_queue hook. ReShade raises it before releasing the native queue, so the queue is still valid
// here, and a game that destroys a queue no longer submits to it: every list it executed was really submitted. The
// queue's fence gets one more signal, waited for without the lock; the textures of its submissions are then freed by the
// next frame end, and the fence is destroyed (gpu_fence keeps no reference to the queue). If the
// queue never got a fence (created only at frame ends, never in hooks) or the wait fails, its textures are kept until
// the device is destroyed, which waits for the device. A watch measuring on this queue fails here: no frame end can
// finish it any more, since they run inside presents on its queue.
inline void OnCommandQueueDestroyed(Watcher* watcher, reshade::api::command_queue* queue) {
  auto* device = queue->get_device();
  if (device->get_api() != reshade::api::device_api::d3d12) return;
  const auto queue_handle = reinterpret_cast<std::uint64_t>(queue);

  std::uint64_t wait_id = 0u;
  // Destroyed at the end of this function, after every lock below is released.
  std::shared_ptr<FenceTimeline> timeline;
  bool has_submissions = false;
  bool signal_in_flight_timed_out = false;
  std::shared_ptr<Session> orphaned_session;
  std::vector<reshade::api::resource> orphaned_textures;
  const auto wait_start = std::chrono::steady_clock::now();
  while (true) {
    WatcherLock lock(watcher, "OnCommandQueueDestroyed");
    const auto pair = watcher->queue_fences.find(queue_handle);
    if (pair != watcher->queue_fences.end() && pair->second.signals_in_flight != 0u) {
      // A frame end is signalling this queue right now; the queue must outlive that call. The wait is bounded: at
      // process exit the signalling thread may already be terminated. The queue is then not used again, and its
      // textures are kept until the device is destroyed.
      if (std::chrono::steady_clock::now() - wait_start < LIFETIME_FENCE_WAIT_TIMEOUT) {
        lock.Unlock();
        std::this_thread::yield();
        continue;
      }
      signal_in_flight_timed_out = true;
    }
    wait_id = ++watcher->destroyed_queue_waits;
    internal::ForEachSubmissionList(watcher, [&](std::vector<GpuSubmission>& submissions) {
      for (auto& submission : submissions) {
        if (submission.queue != queue) continue;
        submission.queue = nullptr;
        submission.fence_value.reset();
        submission.pending_signal = 0u;
        submission.destroyed_queue_wait = wait_id;
        has_submissions = true;
      }
    });
    if (pair != watcher->queue_fences.end()) {
      timeline = pair->second.timeline;
      watcher->queue_fences.erase(pair);
    }
    // After the submissions above were marked, so the session's textures this queue may still use are retired with
    // those marks.
    if (watcher->session != nullptr && watcher->session->queue == queue) {
      orphaned_session = internal::DetachSession(watcher, &orphaned_textures);
    }
    break;
  }
  if (orphaned_session != nullptr) {
    // Its reductions and stats copies were submitted at the presents on this queue, so one more signal on its stats
    // fence covers them before its objects are destroyed. No frame end signals that fence any more.
    bool gpu_finished = false;
    if (orphaned_session->stats_timeline != nullptr) {
      const auto value = internal::SignalTimeline(orphaned_session->stats_timeline.get(), queue);
      gpu_finished = (value != 0u && internal::WaitTimeline(orphaned_session->stats_timeline.get(), value));
    }
    if (gpu_finished) {
      for (const auto texture : orphaned_textures) {
        device->destroy_resource(texture);
      }
      internal::DestroySessionObjects(orphaned_session.get(), device);
    } else {
      reshade::log::message(reshade::log::level::warning, "devkit::watch_region(the GPU did not finish a watch on a destroyed queue within the wait timeout; its textures are not destroyed)");
      if (orphaned_session->stats_timeline != nullptr) {
        WatcherLock lock(watcher, "OnCommandQueueDestroyed: orphaning the stats fence");
        watcher->orphan_timelines.push_back(std::move(orphaned_session->stats_timeline));
      }
    }
    internal::FailRequest(orphaned_session->request, "The command queue the watch measured on was destroyed.");
  }
  if (timeline == nullptr) {
    if (has_submissions) {
      reshade::log::message(reshade::log::level::warning, "devkit::watch_region(a queue was destroyed before its lifetime fence existed; its scratch textures are kept until the device is destroyed)");
    }
    return;
  }

  bool waited = false;
  if (!signal_in_flight_timed_out) {
    const auto value = internal::SignalTimeline(timeline.get(), queue);
    waited = (value != 0u && internal::WaitTimeline(timeline.get(), value));
  }
  if (!waited) {
    if (has_submissions) {
      reshade::log::message(reshade::log::level::warning, "devkit::watch_region(lifetime fence wait failed for a destroyed queue; its scratch textures are kept until the device is destroyed)");
    }
    WatcherLock lock(watcher, "OnCommandQueueDestroyed: orphaning the fence");
    watcher->orphan_timelines.push_back(std::move(timeline));
    return;
  }
  if (has_submissions) {
    WatcherLock lock(watcher, "OnCommandQueueDestroyed: waited");
    internal::ForEachSubmissionList(watcher, [&](std::vector<GpuSubmission>& submissions) {
      std::erase_if(submissions, [&](const GpuSubmission& submission) {
        return submission.destroyed_queue_wait == wait_id;
      });
    });
  }
}

// Called from reshade_present, which ReShade raises after every add-on's present handler and after its own effects
// and overlay, so the end-of-frame measurement includes whatever any add-on wrote during present, independent of load
// order. Finishes the frame that just ended (final measurement, D3D9/D3D12/Vulkan reductions, stats readback), completes
// the watch when enough frames were recorded, and starts the next queued watch. `reduce_shader` is the device API's
// reduction shader (D3D9: the pixel shader); `d3d9_vertex_shader` is only used on D3D9.
inline void ProcessAtFrameEnd(
    Watcher* watcher,
    std::uint32_t device_index,
    reshade::api::effect_runtime* runtime,
    std::span<const std::uint8_t> reduce_shader,
    std::span<const std::uint8_t> d3d9_vertex_shader) {
  auto* device = runtime->get_device();
  auto* queue = runtime->get_command_queue();
  const auto get_unmeasured_reason_name = [](UnmeasuredReason reason) -> std::string_view {
    switch (reason) {
      case UnmeasuredReason::NONE:
        return "none";
      case UnmeasuredReason::SLOT_LIMIT:
        return "slot_limit";
      case UnmeasuredReason::SCRATCH_BUDGET:
        return "scratch_budget";
      case UnmeasuredReason::STATE_UNKNOWN:
        return "state_unknown";
      case UnmeasuredReason::INSIDE_RENDER_PASS:
        return "inside_render_pass";
      case UnmeasuredReason::OTHER_QUEUE:
        return "other_queue";
      case UnmeasuredReason::RESUBMITTED:
        return "resubmitted";
      case UnmeasuredReason::UNTRACKED_COMMAND_LIST:
        return "untracked_command_list";
      case UnmeasuredReason::BUNDLE:
        return "bundle";
      case UnmeasuredReason::NO_LATER_MEASUREMENT:
        return "no_later_measurement";
      case UnmeasuredReason::SCRATCH_POOL_EMPTY:
        return "scratch_pool_empty";
      case UnmeasuredReason::DESCRIPTOR_TABLE:
        return "descriptor_table";
      case UnmeasuredReason::CROSS_QUEUE_NOT_READY:
        return "cross_queue_not_ready";
    }
    return "unknown";
  };

  const auto make_error = [](const std::string& message) {
    return ToolResult{
        .text = message,
        .structured_content = json{{"error", message}},
        .is_error = true,
    };
  };
  const auto complete_request = [](const std::shared_ptr<Request>& request, ToolResult result) {
    {
      std::scoped_lock request_lock(request->mutex);
      request->result = std::move(result);
      request->completed = true;
    }
    request->cv.notify_all();
  };
  // withSnapshot: the snapshot must record the first counted frame, so it starts at the frame end before it (the watch's
  // start on D3D9/D3D11/OpenGL, the end of the warm-up frame on D3D12/Vulkan). Called without the watcher lock.
  const auto begin_linked_snapshot = [&](const std::shared_ptr<Request>& request) {
    // Unset on a device the Devkit takes no snapshots of.
    if (!watcher->begin_linked_snapshot) {
      std::scoped_lock request_lock(request->mutex);
      request->snapshot_link_error = "This device takes no snapshots, so the writers are not linked to a snapshot.";
      return;
    }
    const auto snapshot_id = watcher->begin_linked_snapshot();
    std::scoped_lock request_lock(request->mutex);
    if (snapshot_id.has_value()) {
      request->linked_snapshot_id = snapshot_id;
      return;
    }
    request->snapshot_link_error = "Another snapshot was queued or running when the watched frame began, so the writers are not linked to a snapshot.";
  };

  // D3D12 scratch texture lifetimes, with or without a running watch (see GpuSubmission).
  ProcessLifetimes(watcher, device, true);

  // The frame end runs in stages so that no ReShade or driver call happens under the watcher lock: a driver call can
  // call back into the game (for example its allocation callbacks), and the game may then wait for one of its threads
  // that waits for the lock. Stage 1 (locked) commits the frame-end measurement, plans the reductions from the points
  // committed this frame and advances the frame boundary, so records committed meanwhile land in the next frame.
  // Stage 2 (unlocked) records the copies, reductions and readback and waits for the GPU, working only on the plan and
  // on state that belongs to the presenting thread (in-flight frames, readbacks, fence, reduction pass). Stage 3
  // (locked) merges the results; a finished watch is detached from the watcher there and completed in stage 4
  // (unlocked), where no hook can reach it any more.
  // Queue operations without the lock are safe here: this runs inside present, and ReShade holds the presenting
  // queue's lock for the whole present (Vulkan vkQueuePresentKHR; DXGI Present for D3D12, whose ExecuteCommandLists
  // takes the same lock), so the game cannot submit to that queue meanwhile. D3D11 keeps measuring in stage 1: its hooks
  // only run on the immediate context, on this same thread, so nothing else waits for the lock there.
  // Only the runtime that started the watch measures it, so its frame-end work never runs on two threads at once (two
  // swap chains of one device may present on different threads). A watch that failed or was canceled (also when its
  // runtime was reset or destroyed) is finished by the next frame end of any runtime on its queue: such frame ends are
  // serialized with the starting runtime's by that queue's lock, and every queue operation below stays on that queue.
  // A watch whose queue nobody presents on any more is finished by OnCommandQueueDestroyed or Shutdown.
  std::shared_ptr<Session> session;
  {
    WatcherLock lock(watcher, "ProcessAtFrameEnd: taking the session");
    session = watcher->session;
    if (session != nullptr && session->runtime != runtime && (session->queue != queue || session->failure.empty())) {
      session = nullptr;
    }
  }
  if (session != nullptr) {
    auto* cmd_list = queue->get_immediate_command_list();

    const auto stats_to_json = [&](const RegionStats& stats) {
      auto stats_json = json{
          {"finitePixels", stats.finite_count},
          {"nonFinitePixels", stats.non_finite_count},
          {"negativePixels", stats.negative_count},
          {"pixelsAboveOne", stats.above_one_count},
          {"blackPixels", stats.black_count},
      };
      if (stats.finite_count != 0u) {
        stats_json["maxLuminance"] = stats.max_luminance;
        stats_json["minLuminance"] = stats.min_luminance;
        stats_json["meanLuminance"] = stats.mean_luminance;
        stats_json["maxChannel"] = stats.max_channel;
        stats_json["maxAlpha"] = stats.max_alpha;
        stats_json["pixelsAlphaAboveOne"] = stats.alpha_above_one_count;
      }
      if (session->signal != Signal::RAW) {
        stats_json["pixelsAboveNits"] = json{
            {"203", stats.above_nits_counts[0]},
            {"400", stats.above_nits_counts[1]},
            {"1000", stats.above_nits_counts[2]},
            {"4000", stats.above_nits_counts[3]},
        };
      }
      return stats_json;
    };

    // The stats of one side of an event, or why that side was not measured. nullopt reason = not decoded yet.
    const auto side_stats = [&](std::optional<std::uint64_t> point_id, UnmeasuredReason reason)
        -> std::optional<std::pair<std::optional<RegionStats>, UnmeasuredReason>> {
      if (!point_id.has_value()) return std::pair{std::optional<RegionStats>{}, reason};
      const auto* point = internal::FindPoint(session.get(), point_id.value());
      if (point == nullptr) return std::pair{std::optional<RegionStats>{}, UnmeasuredReason::NO_LATER_MEASUREMENT};
      if (!point->decoded) return std::nullopt;
      if (point->refused != UnmeasuredReason::NONE || !point->stats.has_value()) {
        return std::pair{std::optional<RegionStats>{}, (point->refused != UnmeasuredReason::NONE ? point->refused : UnmeasuredReason::SLOT_LIMIT)};
      }
      return std::pair{point->stats, UnmeasuredReason::NONE};
    };

    // Folds every event whose both sides are decoded into the aggregates (and the trace for one frame), in order.
    const auto process_events = [&]() {
      while (!session->events.empty()) {
        const auto& event = session->events.front();
        if (!event.after_resolved) break;
        const auto before = side_stats(event.before_point, event.before_reason);
        const auto after = side_stats(event.after_point, event.after_reason);
        if (!before.has_value() || !after.has_value()) break;

        const auto& writer = event.writer;
        std::string key = std::format(
            "{} ps={} vs={} cs={}",
            internal::GetWriterKindName(writer.kind),
            formatting::FormatShaderHash(writer.pixel_shader_hash),
            formatting::FormatShaderHash(writer.vertex_shader_hash),
            formatting::FormatShaderHash(writer.compute_shader_hash));
        if (writer.kind == WriterKind::RENDER_PASS) {
          std::vector<std::uint32_t> member_hashes;
          for (const auto& member : writer.members) {
            member_hashes.push_back(member.pixel_shader_hash);
          }
          std::ranges::sort(member_hashes);
          const auto [first_duplicate, last_duplicate] = std::ranges::unique(member_hashes);
          member_hashes.erase(first_duplicate, last_duplicate);
          key = "render_pass ps=[";
          for (std::size_t index = 0u; index < member_hashes.size(); ++index) {
            key += std::format("{}{}", (index == 0u ? "" : ","), formatting::FormatShaderHash(member_hashes[index]));
          }
          key += "]";
        }
        auto& aggregate = session->aggregates.try_emplace(key, WriterAggregate{
                                                                   .kind = writer.kind,
                                                                   .pixel_shader_hash = writer.pixel_shader_hash,
                                                                   .vertex_shader_hash = writer.vertex_shader_hash,
                                                                   .compute_shader_hash = writer.compute_shader_hash,
                                                               }).first->second;
        ++session->events_decoded;
        ++aggregate.executions;
        if (event.cross_queue) {
          ++session->events_cross_queue;
          ++aggregate.cross_queue_executions;
        }
        if (aggregate.last_frame_index != event.frame_index) {
          aggregate.last_frame_index = event.frame_index;
          ++aggregate.frames_seen;
        }

        const auto& before_stats = before->first;
        const auto& after_stats = after->first;
        auto entry = json{
            {"commandIndex", writer.command_index},
            {"writer", std::string(internal::GetWriterKindName(writer.kind))},
        };
        if (event.cross_queue) {
          entry["crossQueue"] = true;
        }
        bool list_in_trace = false;
        if (before_stats.has_value() && after_stats.has_value()) {
          const auto& before_value = before_stats.value();
          const auto& after_value = after_stats.value();
          const auto changed = before_value.raw != after_value.raw;
          if (changed) {
            ++session->events_changed;
            ++aggregate.changed_executions;
          }
          if (after_value.max_luminance < before_value.max_luminance * (1.0 - PEAK_TOLERANCE)) {
            ++aggregate.peak_lowered_executions;
          }
          if (after_value.max_luminance > before_value.max_luminance * (1.0 + PEAK_TOLERANCE)) {
            ++aggregate.peak_raised_executions;
          }
          if (before_value.above_one_count != 0u && after_value.above_one_count == 0u) {
            ++aggregate.dropped_above_one_executions;
          }
          if (before_value.alpha_above_one_count == 0u && after_value.alpha_above_one_count != 0u) {
            ++aggregate.raised_alpha_above_one_executions;
          }
          if (before_value.finite_count != 0u) {
            aggregate.before_peak.Add(before_value.max_luminance);
            aggregate.before_mean.Add(before_value.mean_luminance);
          }
          if (after_value.finite_count != 0u) {
            aggregate.after_peak.Add(after_value.max_luminance);
            aggregate.after_mean.Add(after_value.mean_luminance);
            aggregate.after_max_alpha.Add(after_value.max_alpha);
            const auto bucket = (after_value.max_luminance > 0.0)
                                    ? static_cast<std::int64_t>(std::llround(std::log10(after_value.max_luminance) * CEILING_BUCKETS_PER_DECADE))
                                    : (std::numeric_limits<std::int64_t>::min)();
            ++aggregate.after_peak_buckets[bucket];
          }
          entry["changed"] = changed;
          entry["before"] = stats_to_json(before_value);
          entry["after"] = stats_to_json(after_value);
          if (before_value.finite_count != 0u && after_value.finite_count != 0u) {
            entry["peakDelta"] = after_value.max_luminance - before_value.max_luminance;
          }
          list_in_trace = changed || session->include_unchanged;
        } else {
          // Never dropped silently: counted per reason, per writer and per frame.
          const auto reason = (before_stats.has_value() ? after->second : before->second);
          ++session->events_unmeasured;
          ++aggregate.unmeasured_executions;
          ++aggregate.unmeasured_reasons[reason];
          ++session->unmeasured_reasons[reason];
          if ((session->frames_with_unmeasured_writers.empty() || session->frames_with_unmeasured_writers.back() != event.frame_index)
              && session->frames_with_unmeasured_writers.size() < MAX_REPORTED_FRAMES) {
            session->frames_with_unmeasured_writers.push_back(event.frame_index);
          }
          entry["measured"] = false;
          entry["unmeasuredSide"] = (before_stats.has_value() ? "after" : "before");
          entry["unmeasuredReason"] = std::string(get_unmeasured_reason_name(reason));
          if (before_stats.has_value()) {
            entry["before"] = stats_to_json(before_stats.value());
          }
          if (after_stats.has_value()) {
            entry["after"] = stats_to_json(after_stats.value());
          }
          list_in_trace = true;
        }

        if (session->frames == 1u && list_in_trace) {
          if (writer.pixel_shader_hash != 0u) {
            entry["pixelShaderHash"] = formatting::FormatShaderHash(writer.pixel_shader_hash);
          }
          if (writer.vertex_shader_hash != 0u) {
            entry["vertexShaderHash"] = formatting::FormatShaderHash(writer.vertex_shader_hash);
          }
          if (writer.compute_shader_hash != 0u) {
            entry["computeShaderHash"] = formatting::FormatShaderHash(writer.compute_shader_hash);
          }
          // Replaced by snapshotDraw and the command's textures once the watch completed (see LinkSnapshotCommands).
          const auto snapshot_command_json = [](const SnapshotCommandKey& key) {
            return json{{"commandList", key.cmd_list_handle}, {"recording", key.cmd_list_recording}, {"index", key.cmd_list_draw_index}};
          };
          if (writer.snapshot_command.has_value()) {
            entry["snapshotCommand"] = snapshot_command_json(writer.snapshot_command.value());
          }
          if (writer.kind == WriterKind::RENDER_PASS) {
            auto members = json::array();
            for (const auto& member : writer.members) {
              auto member_json = json{{"commandIndex", member.command_index}};
              if (member.pixel_shader_hash != 0u) {
                member_json["pixelShaderHash"] = formatting::FormatShaderHash(member.pixel_shader_hash);
              }
              if (member.vertex_shader_hash != 0u) {
                member_json["vertexShaderHash"] = formatting::FormatShaderHash(member.vertex_shader_hash);
              }
              if (member.snapshot_command.has_value()) {
                member_json["snapshotCommand"] = snapshot_command_json(member.snapshot_command.value());
              }
              members.push_back(std::move(member_json));
            }
            entry["draws"] = std::move(members);
            entry["drawCount"] = writer.member_count;
          }
          session->trace.push_back(std::move(entry));
        }

        session->events.pop_front();
        ++session->events_base;
      }

      // Points are kept while an event or a later unmeasured writer may still refer to them.
      auto oldest_needed = session->points_base + session->points.size();
      if (!session->events.empty()) {
        const auto& front = session->events.front();
        if (front.before_point.has_value()) {
          oldest_needed = std::min(oldest_needed, front.before_point.value());
        }
        if (front.after_point.has_value()) {
          oldest_needed = std::min(oldest_needed, front.after_point.value());
        }
      }
      if (session->last_point.has_value()) {
        oldest_needed = std::min(oldest_needed, session->last_point.value());
      }
      while (!session->points.empty() && session->points_base < oldest_needed && session->points.front().decoded) {
        session->points.pop_front();
        ++session->points_base;
      }
    };

    // One stats readback, copied out of its mapped texture (stage 2 or 4) and decoded into its points later.
    struct MappedReadback {
      InFlightFrame frame;
      std::vector<std::uint32_t> rows = {};
      std::uint32_t row_count = 0u;
    };
    std::vector<MappedReadback> mapped_readbacks;
    std::string unlocked_failure;

    // Maps the oldest in-flight readback. D3D9/D3D12/Vulkan mapping does not wait for the GPU, so the frame's fence must
    // have passed; with `wait` the CPU waits for it. Never called under the watcher lock.
    const auto map_oldest = [&](bool wait) -> bool {
      const auto frame = session->in_flight.front();
      const auto has_readback = (frame.fence_value != (std::numeric_limits<std::uint64_t>::max)());
      if ((session->explicit_api || session->stats_timeline != nullptr) && has_readback) {
        if (session->stats_timeline != nullptr) {
          if (frame.fence_value == 0u) {
            // Vulkan: finish_present has not signalled this frame yet.
            if (!wait) return false;
            internal::SignalPendingStatsFrame(session.get(), queue);
          }
          if (session->stats_signal_failed) {
            unlocked_failure = "The stats readback fence could not be signalled.";
            return false;
          }
          const auto fence_value = session->in_flight.front().fence_value;
          if (internal::GetCompletedValue(session->stats_timeline.get()) < fence_value) {
            if (!wait) return false;
            // Bounded, so a GPU that never reaches the value (hang without a device removal) fails the watch instead of
            // blocking present forever.
            if (!internal::WaitTimeline(session->stats_timeline.get(), fence_value)) {
              unlocked_failure = "The GPU did not finish the region statistics within the wait timeout.";
              return false;
            }
          }
        } else if (wait) {
          queue->wait_idle();
        } else {
          return false;
        }
      }
      session->in_flight.pop_front();

      auto mapped = MappedReadback{.frame = frame};
      if (has_readback) {
        reshade::api::subresource_data mapped_data = {};
        if (!device->map_texture_region(session->readbacks[frame.ring_index], 0u, nullptr, reshade::api::map_access::read_only, &mapped_data)) {
          unlocked_failure = "Failed to map the stats readback texture.";
          return false;
        }
        mapped.row_count = MAX_SLOTS;
        mapped.rows.resize(static_cast<std::size_t>(MAX_SLOTS) * STATS_STRIDE);
        for (std::uint32_t slot = 0u; slot < MAX_SLOTS; ++slot) {
          std::copy_n(
              reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(mapped_data.data) + (static_cast<std::size_t>(mapped_data.row_pitch) * slot)),
              STATS_STRIDE,
              mapped.rows.begin() + (static_cast<std::ptrdiff_t>(slot) * STATS_STRIDE));
        }
        device->unmap_texture_region(session->readbacks[frame.ring_index], 0u);
        if (session->device_api == reshade::api::device_api::d3d9) {
          // The D3D9 rows hold floats (D3D9_STATS_*); rewritten in the word layout of the compute path, so decoding and
          // the before/after comparison of raw rows are shared.
          for (std::uint32_t slot = 0u; slot < MAX_SLOTS; ++slot) {
            const auto row = std::span(mapped.rows).subspan(static_cast<std::size_t>(slot) * STATS_STRIDE, STATS_STRIDE);
            std::array<float, STATS_STRIDE> values = {};
            std::ranges::transform(row, values.begin(), [](std::uint32_t word) { return std::bit_cast<float>(word); });
            const auto count = [&values](std::uint32_t index) {
              return static_cast<std::uint32_t>(std::lround(values[index]));
            };
            const auto luminance_sum = static_cast<std::uint64_t>(std::llround(
                std::max(static_cast<double>(values[D3D9_STATS_LUMINANCE_SUM]), 0.0) * LUMINANCE_SUM_SCALE));
            std::ranges::fill(row, 0u);
            row[STATS_NON_FINITE_COUNT] = count(D3D9_STATS_NON_FINITE_COUNT);
            row[STATS_MAX_LUMINANCE_BITS] = std::bit_cast<std::uint32_t>(values[D3D9_STATS_MAX_LUMINANCE]);
            row[STATS_MIN_LUMINANCE_INVERTED_BITS] = ~std::bit_cast<std::uint32_t>(values[D3D9_STATS_MIN_LUMINANCE]);
            row[STATS_MAX_CHANNEL_BITS] = std::bit_cast<std::uint32_t>(values[D3D9_STATS_MAX_CHANNEL]);
            row[STATS_LUMINANCE_SUM_LOW] = static_cast<std::uint32_t>(luminance_sum);
            row[STATS_LUMINANCE_SUM_HIGH] = static_cast<std::uint32_t>(luminance_sum >> 32u);
            row[STATS_NEGATIVE_COUNT] = count(D3D9_STATS_NEGATIVE_COUNT);
            row[STATS_ABOVE_ONE_COUNT] = count(D3D9_STATS_ABOVE_ONE_COUNT);
            row[STATS_ABOVE_203_COUNT] = count(D3D9_STATS_ABOVE_203_COUNT);
            row[STATS_ABOVE_400_COUNT] = count(D3D9_STATS_ABOVE_400_COUNT);
            row[STATS_ABOVE_1000_COUNT] = count(D3D9_STATS_ABOVE_1000_COUNT);
            row[STATS_ABOVE_4000_COUNT] = count(D3D9_STATS_ABOVE_4000_COUNT);
            row[STATS_FINITE_COUNT] = count(D3D9_STATS_FINITE_COUNT);
            row[STATS_BLACK_COUNT] = count(D3D9_STATS_BLACK_COUNT);
            row[STATS_MAX_ALPHA_BITS] = std::bit_cast<std::uint32_t>(values[D3D9_STATS_MAX_ALPHA]);
            row[STATS_ALPHA_ABOVE_ONE_COUNT] = count(D3D9_STATS_ALPHA_ABOVE_ONE_COUNT);
          }
        }
      }
      mapped_readbacks.push_back(std::move(mapped));
      return true;
    };

    // Stores the stats of the mapped readbacks in their points, in order. Under the lock, or on a detached session.
    const auto decode_mapped_readbacks = [&]() {
      for (const auto& mapped : mapped_readbacks) {
        const auto& frame = mapped.frame;
        // The frame's own range, except cross-queue points (reduced in whichever frame end found them ready, and
        // listed there in late_points), then this frame end's late points.
        std::vector<std::uint64_t> point_ids;
        for (std::uint64_t point_id = frame.first_point; point_id < frame.first_point + frame.point_count; ++point_id) {
          const auto* point = internal::FindPoint(session.get(), point_id);
          if (point != nullptr && !point->cross_queue) {
            point_ids.push_back(point_id);
          }
        }
        point_ids.insert(point_ids.end(), frame.late_points.begin(), frame.late_points.end());
        for (const auto point_id : point_ids) {
          auto* point = internal::FindPoint(session.get(), point_id);
          if (point == nullptr) continue;
          point->decoded = true;
          if (mapped.row_count == 0u || point->refused != UnmeasuredReason::NONE || !point->slot.has_value()) continue;

          const auto* row = mapped.rows.data() + (static_cast<std::size_t>(point->slot.value()) * STATS_STRIDE);
          RegionStats stats = {};
          std::copy_n(row, STATS_STRIDE, stats.raw.begin());
          stats.finite_count = row[STATS_FINITE_COUNT];
          stats.non_finite_count = row[STATS_NON_FINITE_COUNT];
          stats.negative_count = row[STATS_NEGATIVE_COUNT];
          stats.above_one_count = row[STATS_ABOVE_ONE_COUNT];
          stats.black_count = row[STATS_BLACK_COUNT];
          stats.above_nits_counts = {row[STATS_ABOVE_203_COUNT], row[STATS_ABOVE_400_COUNT], row[STATS_ABOVE_1000_COUNT], row[STATS_ABOVE_4000_COUNT]};
          stats.alpha_above_one_count = row[STATS_ALPHA_ABOVE_ONE_COUNT];
          if (stats.finite_count != 0u) {
            stats.max_luminance = std::bit_cast<float>(row[STATS_MAX_LUMINANCE_BITS]);
            stats.min_luminance = std::bit_cast<float>(~row[STATS_MIN_LUMINANCE_INVERTED_BITS]);
            stats.max_channel = std::bit_cast<float>(row[STATS_MAX_CHANNEL_BITS]);
            stats.max_alpha = std::bit_cast<float>(row[STATS_MAX_ALPHA_BITS]);
            const auto luminance_sum = (static_cast<std::uint64_t>(row[STATS_LUMINANCE_SUM_HIGH]) << 32u) | row[STATS_LUMINANCE_SUM_LOW];
            stats.mean_luminance = static_cast<double>(luminance_sum) / (LUMINANCE_SUM_SCALE * static_cast<double>(stats.finite_count));
          }
          if (session->verify_point.has_value() && session->verify_point.value() == point_id) {
            session->verify_gpu_stats = stats;
          }
          if (session->end_point.has_value() && session->end_point.value() == point_id) {
            session->end_stats = stats;
          }
          point->stats = stats;
        }
        if (frame.counted) {
          ++session->frames_decoded;
          if (frame.truncated) {
            ++session->truncated_frames;
          }
        }
      }
      mapped_readbacks.clear();
      process_events();
    };

    // Stage 1 (locked): plan.
    struct PlannedReduction {
      std::uint64_t point_id = 0u;
      std::uint32_t entry_index = 0u;
      reshade::api::resource texture = {0u};
      std::uint32_t slot = 0u;
      bool verify = false;
    };
    std::vector<PlannedReduction> planned_reductions;
    std::vector<std::uint32_t> reduced_entries;
    std::vector<std::uint64_t> late_points;
    std::vector<std::uint32_t> cross_queue_reduced_entries;
    std::optional<std::uint32_t> back_buffer_entry = std::nullopt;
    std::uint64_t back_buffer_point = 0u;
    reshade::api::resource back_buffer = {0u};
    reshade::api::resource back_buffer_texture = {0u};
    std::optional<InFlightFrame> new_frame = std::nullopt;
    bool has_measurements = false;
    // D3D9 and OpenGL: the atlas slots filled this frame, reduced in stage 2.
    std::uint32_t d3d9_slots_to_reduce = 0u;
    std::uint32_t opengl_slots_to_reduce = 0u;
    bool frame_planned = false;
    bool request_canceled = false;
    bool warm_up_ended_with_snapshot = false;
    std::uint32_t scratch_growth = 0u;
    {
      WatcherLock lock(watcher, "ProcessAtFrameEnd: planning the frame end");
      if (watcher->session != session) return;
      {
        std::scoped_lock request_lock(session->request->mutex);
        request_canceled = session->request->canceled;
      }
      if (session->failure.empty() && watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) {
        session->failure = "The watched texture was destroyed during the watch (for example by a resize).";
      }
      if (session->failure.empty() && session->device_api == reshade::api::device_api::opengl
          && session->opengl.functions->get_current_context() != session->opengl.context) {
        session->failure = "The OpenGL context that presents the swap chain changed during the watch.";
      }

      if (!request_canceled && session->failure.empty()) {
        frame_planned = true;
        const auto is_last_frame = (!session->warming_up && !session->closing && session->frames_recorded + 1u >= session->frames);

        if (session->explicit_api) {
          // Final measurement of the frame: only the presented back buffer is in a known state (present) here; any
          // other texture's last writer is closed by the next exact point (usually in the next frame). A watch on a
          // swap-chain image follows the swap chain, so this is whichever of its images is presented.
          const auto current_back_buffer = runtime->get_current_back_buffer();
          const auto wants_final_point = session->open_event.has_value() || is_last_frame;
          if (current_back_buffer.handle != 0u && internal::GetWatchedImage(*session, current_back_buffer.handle) == current_back_buffer.handle && wants_final_point) {
            back_buffer = current_back_buffer;
            const auto [entry_index, reserve_reason] = internal::ReserveScratchEntry(session.get(), 0u, true);
            back_buffer_entry = entry_index;
            internal::Commit(session.get(), Record{.measurement = back_buffer_entry, .reason = reserve_reason, .image = back_buffer.handle}, UnmeasuredReason::NONE);
            if (back_buffer_entry.has_value()) {
              back_buffer_point = session->points_base + session->points.size() - 1u;
              back_buffer_texture = session->scratch_entries[back_buffer_entry.value()].texture;
              if (is_last_frame) {
                session->end_point = back_buffer_point;
                if (session->verify) {
                  session->verify_point = session->end_point;
                }
              }
            }
          }

          // Reductions of every point committed this frame, in commit order; the barriers order them after the game's
          // copies, which ran earlier on this queue.
          const auto frame_end_point = session->points_base + session->points.size();
          for (auto point_id = std::max(session->frame_first_point, session->points_base); point_id < frame_end_point; ++point_id) {
            auto* point = internal::FindPoint(session.get(), point_id);
            if (point == nullptr || !point->scratch_entry.has_value() || point->deferred) continue;
            reduced_entries.push_back(point->scratch_entry.value());
            if (point->refused == UnmeasuredReason::NONE && planned_reductions.size() >= MAX_SLOTS) {
              point->refused = UnmeasuredReason::SLOT_LIMIT;
              session->truncated = true;
            }
            if (point->refused != UnmeasuredReason::NONE) continue;
            point->slot = static_cast<std::uint32_t>(planned_reductions.size());
            planned_reductions.push_back(PlannedReduction{
                .point_id = point_id,
                .entry_index = point->scratch_entry.value(),
                .texture = session->scratch_entries[point->scratch_entry.value()].texture,
                .slot = point->slot.value(),
                .verify = (session->verify_point.has_value() && session->verify_point.value() == point_id && !session->verify_copied),
            });
          }

          // Cross-queue points whose copy the GPU has finished on the game's queue, judged on the CPU from that queue's
          // lifetime fence; the reduction below then runs on the presenting queue strictly after it. The others wait
          // for a later frame end. Refused ones (resubmitted) are dropped: decoded, with no statistics.
          std::erase_if(session->deferred_points, [&](std::uint64_t point_id) {
            auto* point = internal::FindPoint(session.get(), point_id);
            if (point == nullptr) return true;
            const auto entry_index = point->scratch_entry.value();
            if (point->refused == UnmeasuredReason::NONE) {
              if (planned_reductions.size() >= MAX_SLOTS) return false;
              const auto& submissions = session->scratch_entries[entry_index].submissions;
              if (!internal::AreSubmissionsComplete(submissions, watcher->completed_fence_values)) return false;
            }
            point->deferred = false;
            reduced_entries.push_back(entry_index);
            if (point->refused != UnmeasuredReason::NONE) {
              point->decoded = true;
              return true;
            }
            point->slot = static_cast<std::uint32_t>(planned_reductions.size());
            planned_reductions.push_back(PlannedReduction{
                .point_id = point_id,
                .entry_index = entry_index,
                .texture = session->scratch_entries[entry_index].texture,
                .slot = point->slot.value(),
                .verify = (session->verify_point.has_value() && session->verify_point.value() == point_id && !session->verify_copied),
            });
            late_points.push_back(point_id);
            cross_queue_reduced_entries.push_back(entry_index);
            return true;
          });
          has_measurements = !planned_reductions.empty();
        } else if (session->open_event.has_value() || is_last_frame) {
          // D3D11: the region is measured in place on the immediate context. D3D9: copied into its atlas slot.
          const auto final_slot = internal::MeasureRegion(session.get(), cmd_list);
          if (session->failure.empty()) {
            internal::Commit(
                session.get(),
                Record{.measurement = final_slot, .reason = (final_slot.has_value() ? UnmeasuredReason::NONE : UnmeasuredReason::SLOT_LIMIT)},
                UnmeasuredReason::NONE);
            if (final_slot.has_value() && is_last_frame) {
              session->end_point = session->points_base + session->points.size() - 1u;
            }
          }
          // Nothing writes the texture between this measurement and the copy below, so both see the same pixels.
          if (session->verify && is_last_frame && final_slot.has_value()
              && device->create_resource(
                  reshade::api::resource_desc(session->width, session->height, 1, 1, session->scratch_format, 1, reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
                  nullptr,
                  reshade::api::resource_usage::copy_dest,
                  &session->verify_readback)) {
            cmd_list->copy_texture_region(session->watched_resource, session->subresource, &session->region, session->verify_readback, 0u, nullptr);
            RestoreDefaultReadBuffer(session->opengl);
            session->verify_point = session->points_base + session->points.size() - 1u;
            session->verify_copied = true;
          }
        }
        if (!session->explicit_api) {
          has_measurements = (session->slots_used != 0u);
        }
        if (session->device_api == reshade::api::device_api::d3d9 && session->failure.empty()) {
          d3d9_slots_to_reduce = session->slots_used;
        }
        if (session->device_api == reshade::api::device_api::opengl && session->failure.empty()) {
          opengl_slots_to_reduce = session->slots_used;
        }

        const auto frame_first_point = std::max(session->frame_first_point, session->points_base);
        const auto frame_point_count = session->points_base + session->points.size() - frame_first_point;
        if (session->failure.empty() && (has_measurements || frame_point_count != 0u)) {
          new_frame = InFlightFrame{
              .frame_index = session->frames_recorded,
              .first_point = frame_first_point,
              .point_count = frame_point_count,
              .counted = !session->warming_up && !session->closing,
              .truncated = session->truncated,
              .fence_value = (std::numeric_limits<std::uint64_t>::max)(),
              .late_points = std::move(late_points),
          };
        }

        session->slots_used = 0u;
        session->command_count = 0u;
        session->stats_cleared = false;
        session->truncated = false;
        session->frame_first_point = session->points_base + session->points.size();
        session->frame_command_base = 0u;
        if (session->warming_up) {
          session->warming_up = false;
          warm_up_ended_with_snapshot = session->with_snapshot;
        } else if (session->closing) {
          ++session->closing_frames;
        } else {
          ++session->frames_recorded;
          session->closing = session->explicit_api && session->frames_recorded >= session->frames;
          if (session->closing) {
            session->closing_started_at = std::chrono::steady_clock::now();
          }
        }
      }
      // The pool grows so that the next frame starts with as many free textures as this frame asked for (at least by
      // what the hooks were short of), within the budget. Free after this frame end: the entries not in use, plus the
      // ones stage 3 releases (reduced, no command list owner left, no GPU submission; D3D12 entries with submissions
      // are freed later by ProcessLifetimes). Closing frames only need the point after the last writer, so they grow
      // the pool only by what the hooks were short of: the watch may end at this frame end, and textures created for a
      // whole frame's demand would be destroyed at once. The textures are created in stage 2 on this thread and added
      // in stage 3.
      if (session->explicit_api && frame_planned) {
        std::uint32_t free_after = 0u;
        for (const auto& entry : session->scratch_entries) {
          if (!entry.in_use && entry.texture.handle != 0u) ++free_after;
        }
        auto released_entries = reduced_entries;
        std::ranges::sort(released_entries);
        released_entries.erase(std::ranges::unique(released_entries).begin(), released_entries.end());
        for (const auto entry_index : released_entries) {
          const auto& entry = session->scratch_entries[entry_index];
          if (entry.in_use && (entry.owner_list == 0u || entry.release_after_reduction) && entry.submissions.empty()
              && std::ranges::find(cross_queue_reduced_entries, entry_index) == cross_queue_reduced_entries.end()) {
            ++free_after;
          }
        }
        const auto pool_size = static_cast<std::uint32_t>(session->scratch_entries.size());
        const auto demand_shortfall = (session->scratch_frame_demand > free_after ? session->scratch_frame_demand - free_after : 0u);
        const auto shortfall = (session->closing ? session->scratch_pool_misses : std::max(session->scratch_pool_misses, demand_shortfall));
        session->scratch_entries_needed = std::max(session->scratch_entries_needed, pool_size + shortfall);
        scratch_growth = std::min(shortfall, session->max_scratch_entries - std::min(session->max_scratch_entries, pool_size));
      }
      session->scratch_pool_misses = 0u;
      session->scratch_frame_demand = 0u;
    }
    if (warm_up_ended_with_snapshot) {
      begin_linked_snapshot(session->request);
    }

    // Stage 2 (unlocked): GPU work on the plan.
    if (frame_planned) {
      if (back_buffer_entry.has_value() && back_buffer_texture.handle == 0u) {
        back_buffer_texture = internal::CreateScratchTexture(device, *session);
      }
      if (back_buffer_entry.has_value() && back_buffer_texture.handle != 0u) {
        internal::RecordRegionCopy(*session, cmd_list, back_buffer.handle, back_buffer_texture, reshade::api::resource_usage::present);
      }
      bool stats_cleared = false;
      for (auto& reduction : planned_reductions) {
        if (back_buffer_entry.has_value() && reduction.entry_index == back_buffer_entry.value()) {
          reduction.texture = back_buffer_texture;
        }
        if (reduction.texture.handle == 0u || !unlocked_failure.empty()) continue;
        if (!stats_cleared) {
          const std::array<std::uint32_t, 4> zeros = {};
          cmd_list->clear_unordered_access_view_uint(session->stats_uav, zeros.data());
          cmd_list->barrier(session->stats_texture, reshade::api::resource_usage::unordered_access, reshade::api::resource_usage::unordered_access);
          stats_cleared = true;
        }
        cmd_list->barrier(reduction.texture, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::copy_source);
        cmd_list->copy_texture_region(reduction.texture, 0u, nullptr, session->scratch, 0u, nullptr);
        cmd_list->barrier(session->scratch, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::shader_resource);
        session->constants = {
            std::bit_cast<float>(session->width),
            std::bit_cast<float>(session->height),
            std::bit_cast<float>(static_cast<std::uint32_t>(session->signal)),
            std::bit_cast<float>(reduction.slot),
        };
        if (!session->pass.Render(cmd_list)) {
          unlocked_failure = "The region reduction pass failed to render.";
        }
        cmd_list->barrier(session->scratch, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::copy_dest);
        if (reduction.verify
            && device->create_resource(
                reshade::api::resource_desc(session->width, session->height, 1, 1, session->scratch_format, 1, reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
                nullptr,
                reshade::api::resource_usage::copy_dest,
                &session->verify_readback)) {
          cmd_list->copy_texture_region(reduction.texture, 0u, nullptr, session->verify_readback, 0u, nullptr);
          session->verify_copied = true;
        }
        cmd_list->barrier(reduction.texture, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::copy_dest);
      }

      // D3D9: reduces every slot filled this frame into its stats row. The passes run inside the scene ReShade opens around
      // its present work (Direct3DSwapChain9::on_present calls BeginScene before raising present and running the effect
      // runtime, whose last step is reshade_present, and EndScene after), so they issue no BeginScene/EndScene of their
      // own: a nested BeginScene fails, and an EndScene here would close ReShade's scene.
      if (d3d9_slots_to_reduce != 0u) {
        auto* native_device = reinterpret_cast<IDirect3DDevice9*>(static_cast<std::uintptr_t>(device->get_native()));
        auto& d3d9 = session->d3d9;

        // State blocks do not record render targets or the depth-stencil surface: the four render target slots the passes
        // use and the depth-stencil surface are saved and restored here.
        std::array<IDirect3DSurface9*, D3D9_STATS_GROUPS> game_render_targets = {};
        for (std::uint32_t index = 0u; index < D3D9_STATS_GROUPS; ++index) {
          if (FAILED(native_device->GetRenderTarget(index, &game_render_targets[index]))) {
            game_render_targets[index] = nullptr;
          }
        }
        IDirect3DSurface9* game_depth_stencil = nullptr;
        if (FAILED(native_device->GetDepthStencilSurface(&game_depth_stencil))) {
          game_depth_stencil = nullptr;
        }
        d3d9.state_block->Capture();

        // A full known state: nothing the game left set may change the passes' output.
        native_device->SetVertexDeclaration(d3d9.vertex_declaration.get());
        native_device->SetVertexShader(d3d9.vertex_shader.get());
        native_device->SetPixelShader(d3d9.pixel_shader.get());
        native_device->SetDepthStencilSurface(nullptr);
        constexpr DWORD ALL_CHANNELS = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;
        constexpr std::array<std::pair<D3DRENDERSTATETYPE, DWORD>, 17> RENDER_STATES = {{
            {D3DRS_ZENABLE, D3DZB_FALSE},
            {D3DRS_ZWRITEENABLE, FALSE},
            {D3DRS_STENCILENABLE, FALSE},
            {D3DRS_ALPHABLENDENABLE, FALSE},
            {D3DRS_SEPARATEALPHABLENDENABLE, FALSE},
            {D3DRS_ALPHATESTENABLE, FALSE},
            {D3DRS_SCISSORTESTENABLE, FALSE},
            {D3DRS_CLIPPLANEENABLE, 0u},
            {D3DRS_FOGENABLE, FALSE},
            {D3DRS_SRGBWRITEENABLE, FALSE},
            {D3DRS_CULLMODE, D3DCULL_NONE},
            {D3DRS_FILLMODE, D3DFILL_SOLID},
            {D3DRS_DITHERENABLE, FALSE},
            {D3DRS_COLORWRITEENABLE, ALL_CHANNELS},
            {D3DRS_COLORWRITEENABLE1, ALL_CHANNELS},
            {D3DRS_COLORWRITEENABLE2, ALL_CHANNELS},
            {D3DRS_COLORWRITEENABLE3, ALL_CHANNELS},
        }};
        for (const auto& [render_state, value] : RENDER_STATES) {
          native_device->SetRenderState(render_state, value);
        }
        constexpr std::array<std::pair<D3DSAMPLERSTATETYPE, DWORD>, 8> SAMPLER_STATES = {{
            {D3DSAMP_SRGBTEXTURE, FALSE},
            {D3DSAMP_MINFILTER, D3DTEXF_POINT},
            {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
            {D3DSAMP_MIPFILTER, D3DTEXF_NONE},
            {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
            {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP},
            {D3DSAMP_MAXMIPLEVEL, 0u},
            {D3DSAMP_MIPMAPLODBIAS, 0u},
        }};
        for (std::uint32_t sampler = 0u; sampler < D3D9_STATS_GROUPS; ++sampler) {
          for (const auto& [sampler_state, value] : SAMPLER_STATES) {
            native_device->SetSamplerState(sampler, sampler_state, value);
          }
        }

        constexpr std::array<float, 8> QUAD = {-1.f, 1.f, 1.f, 1.f, -1.f, -1.f, 1.f, -1.f};
        const auto stats_target = internal::GetD3D9Surface(session->stats_texture, 0u);
        for (std::uint32_t slot = 0u; slot < d3d9_slots_to_reduce && unlocked_failure.empty(); ++slot) {
          // Pass inputs: the slot's cell of the atlas first, then each intermediate level in turn.
          std::array<reshade::api::resource, D3D9_STATS_GROUPS> inputs = {session->scratch};
          std::array<float, 12> constants = {
              static_cast<float>((slot % d3d9.atlas_columns) * session->width),
              static_cast<float>((slot / d3d9.atlas_columns) * session->height),
              static_cast<float>(d3d9.atlas_width),
              static_cast<float>(d3d9.atlas_height),
              static_cast<float>(session->width),
              static_cast<float>(session->height),
              1.f,
              static_cast<float>(static_cast<std::uint32_t>(session->signal)),
              0.f,
              0.f,
              0.f,
              0.f,
          };
          for (std::size_t level_index = 0u; level_index <= d3d9.levels.size(); ++level_index) {
            const auto last_pass = (level_index == d3d9.levels.size());
            // Inputs first, so a level is never bound as a texture while it becomes a render target.
            for (std::uint32_t index = 0u; index < D3D9_STATS_GROUPS; ++index) {
              native_device->SetTexture(index, reinterpret_cast<IDirect3DTexture9*>(static_cast<std::uintptr_t>(inputs[index].handle)));
            }
            D3DVIEWPORT9 viewport = {.X = 0u, .Y = 0u, .Width = 0u, .Height = 0u, .MinZ = 0.f, .MaxZ = 1.f};
            if (last_pass) {
              native_device->SetRenderTarget(0u, stats_target.get());
              for (std::uint32_t index = 1u; index < D3D9_STATS_GROUPS; ++index) {
                native_device->SetRenderTarget(index, nullptr);
              }
              viewport.Y = slot;
              viewport.Width = D3D9_STATS_GROUPS;
              viewport.Height = 1u;
            } else {
              const auto& level = d3d9.levels[level_index];
              for (std::uint32_t index = 0u; index < D3D9_STATS_GROUPS; ++index) {
                const auto target = internal::GetD3D9Surface(level.targets[index], 0u);
                native_device->SetRenderTarget(index, target.get());
              }
              viewport.Width = level.width;
              viewport.Height = level.height;
            }
            // Set after the render targets: SetRenderTarget resets the viewport.
            native_device->SetViewport(&viewport);
            constants[8] = (last_pass ? 1.f : 0.f);
            native_device->SetPixelShaderConstantF(0u, constants.data(), static_cast<UINT>(constants.size() / 4u));
            if (FAILED(native_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2u, QUAD.data(), sizeof(float) * 2u))) {
              unlocked_failure = "The Direct3D 9 region reduction pass failed to draw.";
              break;
            }
            if (!last_pass) {
              const auto& level = d3d9.levels[level_index];
              inputs = level.targets;
              constants = {
                  0.f,
                  0.f,
                  static_cast<float>(level.width),
                  static_cast<float>(level.height),
                  static_cast<float>(level.width),
                  static_cast<float>(level.height),
                  0.f,
                  constants[7],
                  0.f,
                  0.f,
                  0.f,
                  0.f,
              };
            }
          }
        }

        for (std::uint32_t index = 0u; index < D3D9_STATS_GROUPS; ++index) {
          native_device->SetRenderTarget(index, game_render_targets[index]);
          if (game_render_targets[index] != nullptr) {
            game_render_targets[index]->Release();
          }
        }
        native_device->SetDepthStencilSurface(game_depth_stencil);
        if (game_depth_stencil != nullptr) {
          game_depth_stencil->Release();
        }
        // After the render targets, which reset the viewport and the scissor rectangle; restores the game's textures,
        // shaders, constants, vertex declaration, stream 0 (DrawPrimitiveUP clears it) and render/sampler states.
        d3d9.state_block->Apply();
      }

      // OpenGL: reduces every slot filled this frame into its stats row with one dispatch (the slot is the z group). The
      // game's GL state is only changed in four places, all restored below: the current program, the alpha test (ReShade's
      // bind_pipeline disables it on compatibility contexts), and the two image units the reduction reads and writes. The
      // stats clear restores the texture binding it uses itself. ReShade raises no add-on event for anything done through
      // its own API; the restores of the game's own bindings are raised, and are the game's own state.
      if (opengl_slots_to_reduce != 0u && unlocked_failure.empty()) {
        auto& opengl = session->opengl;
        const auto& functions = *opengl.functions;

        int game_program = 0;
        functions.get_integerv(gl_enum::CURRENT_PROGRAM, &game_program);
        const auto game_alpha_test = (opengl.compatibility_context && functions.is_enabled(gl_enum::ALPHA_TEST) != 0u);
        struct GameImageUnit {
          int name = 0;
          int level = 0;
          int layered = 0;
          int layer = 0;
          int access = 0;
          int format = 0;
        };
        const std::array<std::uint32_t, 2> image_units = {opengl.atlas_unit, opengl.stats_unit};
        std::array<GameImageUnit, 2> game_image_units = {};
        for (std::size_t index = 0u; index < image_units.size(); ++index) {
          auto& game_unit = game_image_units[index];
          functions.get_integeri_v(gl_enum::IMAGE_BINDING_NAME, image_units[index], &game_unit.name);
          functions.get_integeri_v(gl_enum::IMAGE_BINDING_LEVEL, image_units[index], &game_unit.level);
          functions.get_integeri_v(gl_enum::IMAGE_BINDING_LAYERED, image_units[index], &game_unit.layered);
          functions.get_integeri_v(gl_enum::IMAGE_BINDING_LAYER, image_units[index], &game_unit.layer);
          functions.get_integeri_v(gl_enum::IMAGE_BINDING_ACCESS, image_units[index], &game_unit.access);
          functions.get_integeri_v(gl_enum::IMAGE_BINDING_FORMAT, image_units[index], &game_unit.format);
        }

        if (!opengl.first_reduction_checked) {
          DrainOpenGLErrors(functions);
        }

        // The UAV clear is an assert stub on ReShade's OpenGL backend: the rows of the used slots are zeroed with an upload.
        static std::vector<std::uint32_t> zero_stats(static_cast<std::size_t>(STATS_STRIDE) * MAX_SLOTS, 0u);
        const auto used_rows = reshade::api::subresource_box{
            .left = 0u,
            .top = 0u,
            .front = 0u,
            .right = STATS_STRIDE,
            .bottom = opengl_slots_to_reduce,
            .back = 1u,
        };
        device->update_texture_region(
            reshade::api::subresource_data{
                .data = zero_stats.data(),
                .row_pitch = STATS_STRIDE * sizeof(std::uint32_t),
                .slice_pitch = STATS_STRIDE * MAX_SLOTS * sizeof(std::uint32_t),
            },
            session->stats_texture,
            0u,
            &used_rows);
        functions.memory_barrier(gl_enum::ALL_BARRIER_BITS);
        cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_compute, opengl.pipeline);
        cmd_list->push_descriptors(
            reshade::api::shader_stage::compute,
            reshade::api::pipeline_layout{0u},
            0u,
            reshade::api::descriptor_table_update{
                .binding = opengl.atlas_unit,
                .count = 1u,
                .type = reshade::api::descriptor_type::texture_unordered_access_view,
                .descriptors = &opengl.atlas_view,
            });
        cmd_list->push_descriptors(
            reshade::api::shader_stage::compute,
            reshade::api::pipeline_layout{0u},
            0u,
            reshade::api::descriptor_table_update{
                .binding = opengl.stats_unit,
                .count = 1u,
                .type = reshade::api::descriptor_type::texture_unordered_access_view,
                .descriptors = &opengl.stats_view,
            });
        cmd_list->dispatch((session->width + GROUP_SIZE - 1u) / GROUP_SIZE, (session->height + GROUP_SIZE - 1u) / GROUP_SIZE, opengl_slots_to_reduce);
        functions.memory_barrier(gl_enum::ALL_BARRIER_BITS);

        unsigned int reduction_error = 0u;
        if (!opengl.first_reduction_checked) {
          opengl.first_reduction_checked = true;
          reduction_error = functions.get_error();
        }

        cmd_list->bind_pipeline(
            reshade::api::pipeline_stage::all_compute,
            reshade::api::pipeline{(static_cast<std::uint64_t>(gl_enum::PROGRAM) << 40u) | static_cast<std::uint32_t>(game_program)});
        if (game_alpha_test) {
          functions.enable(gl_enum::ALPHA_TEST);
        }
        for (std::size_t index = 0u; index < image_units.size(); ++index) {
          const auto& game_unit = game_image_units[index];
          // An empty unit is not rebound: ReShade's hook of glBindImageTexture queries the texture target, which is a GL
          // error for texture 0 on 4.5 contexts. Our textures unbind themselves when they are deleted at the end of the watch.
          if (game_unit.name == 0) continue;
          functions.bind_image_texture(
              image_units[index],
              static_cast<unsigned int>(game_unit.name),
              game_unit.level,
              static_cast<unsigned char>(game_unit.layered),
              game_unit.layer,
              static_cast<unsigned int>(game_unit.access),
              static_cast<unsigned int>(game_unit.format));
        }
        if (reduction_error != 0u) {
          unlocked_failure = std::format("The OpenGL region reduction pass failed (OpenGL error 0x{:04X}).", reduction_error);
        }
      }

      if (new_frame.has_value() && has_measurements && unlocked_failure.empty()) {
        // Keep at most two frames in flight so the newest copy never overwrites an unread readback.
        while (session->in_flight.size() >= 2u && unlocked_failure.empty()) {
          map_oldest(true);
        }
        auto& frame = new_frame.value();
        frame.ring_index = static_cast<std::uint32_t>(session->readback_pushes++ % READBACK_RING_SIZE);
        if (session->explicit_api) {
          cmd_list->barrier(session->stats_texture, reshade::api::resource_usage::unordered_access, reshade::api::resource_usage::copy_source);
          cmd_list->copy_texture_region(session->stats_texture, 0u, nullptr, session->readbacks[frame.ring_index], 0u, nullptr);
          cmd_list->barrier(session->stats_texture, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::unordered_access);
          frame.fence_value = 0u;
          if (session->stats_timeline != nullptr && session->device_api == reshade::api::device_api::d3d12) {
            // gpu_fence's D3D12 signal does not flush ReShade's immediate command list, which holds this frame's
            // reductions and stats copy.
            queue->flush_immediate_command_list();
            frame.fence_value = internal::SignalTimeline(session->stats_timeline.get(), queue);
            if (frame.fence_value == 0u) {
              unlocked_failure = "The stats readback fence could not be signalled.";
            }
          } else if (session->stats_timeline != nullptr) {
            // A frame left unsignalled because finish_present did not follow (failed present) is signalled now.
            internal::SignalPendingStatsFrame(session.get(), queue);
            session->stats_signal_pending = true;
          }
        } else {
          cmd_list->copy_texture_region(session->stats_texture, 0u, nullptr, session->readbacks[frame.ring_index], 0u, nullptr);
          frame.fence_value = 0u;
          if (session->device_api == reshade::api::device_api::d3d9 || session->device_api == reshade::api::device_api::opengl) {
            // gpu_fence's D3DKMT signal needs the device's command buffer flushed first, or it can land before the
            // reductions and the stats copy. The OpenGL sync object flushes after it is created (SignalTimeline).
            if (session->device_api == reshade::api::device_api::d3d9) {
              queue->flush_immediate_command_list();
            }
            frame.fence_value = internal::SignalTimeline(session->stats_timeline.get(), queue);
            if (frame.fence_value == 0u) {
              unlocked_failure = "The stats readback fence could not be signalled.";
            }
          }
        }
      }
      if (new_frame.has_value() && unlocked_failure.empty()) {
        session->in_flight.push_back(new_frame.value());
      }
      // D3D9/D3D12/Vulkan: map whatever the GPU has already finished.
      while ((session->explicit_api || session->stats_timeline != nullptr) && !session->in_flight.empty() && unlocked_failure.empty()) {
        if (!map_oldest(false)) break;
      }
    }
    std::vector<reshade::api::resource> new_scratch_textures;
    for (std::uint32_t index = 0u; index < scratch_growth; ++index) {
      const auto texture = internal::CreateScratchTexture(device, *session);
      if (texture.handle == 0u) break;
      new_scratch_textures.push_back(texture);
    }
    // Creating many textures in one frame end can cause a hitch: logged so that a stutter during a watch is explained.
    if (!new_scratch_textures.empty()) {
      reshade::log::message(
          reshade::log::level::info,
          std::format("devkit::watch_region(created {} of {} wanted scratch texture(s) at a frame end)", new_scratch_textures.size(), scratch_growth).c_str());
    }

    // Stage 3 (locked): merge.
    std::shared_ptr<Session> finished_session;
    std::vector<reshade::api::resource> released_textures;
    {
      WatcherLock lock(watcher, "ProcessAtFrameEnd: merging the frame end");
      if (watcher->session != session) {
        // Removed meanwhile by OnCommandQueueDestroyed or Shutdown, which took care of the session's entries.
        if (back_buffer_texture.handle != 0u && back_buffer_entry.has_value()) {
          released_textures.push_back(back_buffer_texture);
        }
        released_textures.insert(released_textures.end(), new_scratch_textures.begin(), new_scratch_textures.end());
      } else {
        for (const auto texture : new_scratch_textures) {
          session->scratch_entries.push_back(ScratchEntry{.texture = texture});
        }
        if (back_buffer_entry.has_value()) {
          auto& entry = session->scratch_entries[back_buffer_entry.value()];
          if (back_buffer_texture.handle != 0u) {
            entry.texture = back_buffer_texture;
          } else if (auto* point = internal::FindPoint(session.get(), back_buffer_point); point != nullptr) {
            point->refused = UnmeasuredReason::SCRATCH_BUDGET;
            point->slot.reset();
          }
        }
        // The reduction of a cross-queue entry reads it on the presenting queue: the entry is reused or destroyed only
        // after the GPU passed that too (its fence value is attached like any submission's, ProcessLifetimes).
        for (const auto entry_index : cross_queue_reduced_entries) {
          internal::AddSubmission(&session->scratch_entries[entry_index].submissions, GpuSubmission{
              .queue = session->queue,
              .frame_end_index = watcher->frame_end_index,
          });
        }
        for (const auto entry_index : reduced_entries) {
          auto& entry = session->scratch_entries[entry_index];
          entry.awaiting_reduction = false;
          if (entry.release_after_reduction) {
            entry.owner_list = 0u;
            entry.release_after_reduction = false;
          }
          // D3D12 entries with submissions are freed by ProcessLifetimes once the GPU passed them.
          if (entry.owner_list == 0u && entry.submissions.empty()) {
            entry.in_use = false;
          }
        }
        if (!unlocked_failure.empty() && session->failure.empty()) {
          session->failure = unlocked_failure;
        }
        decode_mapped_readbacks();

        const auto closed = !session->open_event.has_value() && (!session->verify || session->verify_point.has_value())
                            && session->deferred_points.empty();
        const auto closing_limit_reached = session->closing_frames >= MAX_CLOSING_FRAMES
                                           && (session->deferred_points.empty()
                                               || std::chrono::steady_clock::now() - session->closing_started_at >= CROSS_QUEUE_CLOSING_TIMEOUT);
        const auto finished = request_canceled || !session->failure.empty()
                              || (session->frames_recorded >= session->frames && (!session->explicit_api || closed || closing_limit_reached));
        if (finished) {
          // Cross-queue points still waiting are reported, never waited for: their fence may never arrive.
          for (const auto point_id : session->deferred_points) {
            if (auto* point = internal::FindPoint(session.get(), point_id); point != nullptr) {
              point->deferred = false;
              point->decoded = true;
              if (point->refused == UnmeasuredReason::NONE) {
                point->refused = UnmeasuredReason::CROSS_QUEUE_NOT_READY;
              }
            }
          }
          session->deferred_points.clear();
          finished_session = internal::DetachSession(watcher, &released_textures);
        }
      }
    }

    // Stage 4 (unlocked): complete a finished watch on its detached session.
    if (finished_session != nullptr) {
      internal::SignalPendingStatsFrame(session.get(), queue);
      queue->wait_idle();
      while (!session->in_flight.empty() && unlocked_failure.empty()) {
        map_oldest(true);
      }
      // The presenting queue is idle; the game's other queues may still run region copies into retired textures.
      WaitForAttachedFenceValues(watcher, device);
      if (!unlocked_failure.empty() && session->failure.empty()) {
        session->failure = unlocked_failure;
      }
      decode_mapped_readbacks();
      if (session->open_event.has_value()) {
        auto& open_event = session->events[static_cast<std::size_t>(session->open_event.value() - session->events_base)];
        open_event.after_resolved = true;
        open_event.after_reason = UnmeasuredReason::NO_LATER_MEASUREMENT;
        session->open_event.reset();
      }
      for (auto& point : session->points) {
        point.decoded = true;
      }
      process_events();

      if (!request_canceled) {
        if (!session->failure.empty()) {
          complete_request(session->request, make_error(session->failure));
        } else {
          const auto unit = (session->signal == Signal::RAW ? "decoded value" : "nits");
          const auto range_to_json = [](const RunningRange& range) {
            if (range.count == 0u) return json(nullptr);
            return json{
                {"min", range.minimum},
                {"mean", range.sum / static_cast<double>(range.count)},
                {"max", range.maximum},
            };
          };
          const auto reasons_to_json = [&](const std::map<UnmeasuredReason, std::uint64_t>& reasons) {
            auto reasons_json = json::object();
            for (const auto& [reason, count] : reasons) {
              reasons_json[std::string(get_unmeasured_reason_name(reason))] = count;
            }
            return reasons_json;
          };

          std::vector<const std::pair<const std::string, WriterAggregate>*> ordered_aggregates;
          for (const auto& entry : session->aggregates) {
            ordered_aggregates.push_back(&entry);
          }
          std::ranges::sort(ordered_aggregates, [](const auto* left, const auto* right) {
            if (left->second.changed_executions != right->second.changed_executions) {
              return left->second.changed_executions > right->second.changed_executions;
            }
            return left->second.executions > right->second.executions;
          });

          auto writers = json::array();
          std::string summary = std::format(
              "Watched {}x{} region at ({}, {}) over {} frame(s): {} writer execution(s) measured, {} changed the region ({}).",
              session->width,
              session->height,
              session->region.left,
              session->region.top,
              session->frames_decoded,
              session->events_decoded - session->events_unmeasured,
              session->events_changed,
              unit);
          if (!session->swap_chain_images.empty()) {
            summary += std::format(" Followed the swap chain: all {} of its images count as the watched texture.", session->swap_chain_images.size());
          }
          if (session->truncated_frames != 0u) {
            summary += std::format(" {} frame(s) hit the {}-slot or scratch budget limit.", session->truncated_frames, session->slot_capacity);
          }
          if (session->explicit_api && session->scratch_entries_needed > session->max_scratch_entries) {
            summary += std::format(
                " The scratch budget allows {} texture(s) but a frame needed up to {}: use a region or shaderHash (or raise"
                " scratchBudgetMB).",
                session->max_scratch_entries,
                session->scratch_entries_needed);
          }
          if (session->events_cross_queue != 0u) {
            summary += std::format(
                " {} writer execution(s) ran on another queue than the presenting one (crossQueue: ordered by CPU submission"
                " order, correct only if the game synchronizes its queues).",
                session->events_cross_queue);
          }
          if (session->events_unmeasured != 0u) {
            summary += std::format(" {} writer execution(s) NOT measured:", session->events_unmeasured);
            for (const auto& [reason, count] : session->unmeasured_reasons) {
              summary += std::format(" {} {}", count, get_unmeasured_reason_name(reason));
            }
            summary += " (see unmeasured).";
          }
          std::size_t summarized_writers = 0u;
          for (const auto* entry : ordered_aggregates) {
            const auto& [key, aggregate] = *entry;
            auto writer_json = json{
                {"writer", key},
                {"kind", std::string(internal::GetWriterKindName(aggregate.kind))},
                {"executions", aggregate.executions},
                {"framesSeen", aggregate.frames_seen},
                {"changedExecutions", aggregate.changed_executions},
                {"peakLoweredExecutions", aggregate.peak_lowered_executions},
                {"peakRaisedExecutions", aggregate.peak_raised_executions},
                {"droppedAboveOneExecutions", aggregate.dropped_above_one_executions},
                {"raisedAlphaAboveOneExecutions", aggregate.raised_alpha_above_one_executions},
                {"afterMaxAlpha", range_to_json(aggregate.after_max_alpha)},
                {"unmeasuredExecutions", aggregate.unmeasured_executions},
                {"beforePeak", range_to_json(aggregate.before_peak)},
                {"afterPeak", range_to_json(aggregate.after_peak)},
                {"beforeMean", range_to_json(aggregate.before_mean)},
                {"afterMean", range_to_json(aggregate.after_mean)},
            };
            if (aggregate.unmeasured_executions != 0u) {
              writer_json["unmeasuredReasons"] = reasons_to_json(aggregate.unmeasured_reasons);
            }
            if (aggregate.cross_queue_executions != 0u) {
              writer_json["crossQueueExecutions"] = aggregate.cross_queue_executions;
            }
            if (aggregate.pixel_shader_hash != 0u) {
              writer_json["pixelShaderHash"] = formatting::FormatShaderHash(aggregate.pixel_shader_hash);
            }
            if (aggregate.vertex_shader_hash != 0u) {
              writer_json["vertexShaderHash"] = formatting::FormatShaderHash(aggregate.vertex_shader_hash);
            }
            if (aggregate.compute_shader_hash != 0u) {
              writer_json["computeShaderHash"] = formatting::FormatShaderHash(aggregate.compute_shader_hash);
            }

            const auto mode = std::ranges::max_element(aggregate.after_peak_buckets, [](const auto& left, const auto& right) {
              return left.second < right.second;
            });
            if (mode != aggregate.after_peak_buckets.end() && mode->second >= 2u) {
              const auto mode_value = (mode->first == (std::numeric_limits<std::int64_t>::min)())
                                          ? 0.0
                                          : std::pow(10.0, static_cast<double>(mode->first) / CEILING_BUCKETS_PER_DECADE);
              writer_json["afterPeakMode"] = json{
                  {"value", mode_value},
                  {"executions", mode->second},
                  {"share", static_cast<double>(mode->second) / static_cast<double>(aggregate.after_peak.count)},
              };
            }
            writers.push_back(std::move(writer_json));

            if (aggregate.changed_executions != 0u && summarized_writers < 5u) {
              ++summarized_writers;
              summary += std::format(
                  "\n{}: changed the region in {} of {} execution(s) over {} frame(s); peak {:.2f}..{:.2f} -> {:.2f}..{:.2f}; lowered the peak {}x, took values from >1.0 to <=1.0 {}x.",
                  key,
                  aggregate.changed_executions,
                  aggregate.executions,
                  aggregate.frames_seen,
                  (aggregate.before_peak.count != 0u ? aggregate.before_peak.minimum : 0.0),
                  (aggregate.before_peak.count != 0u ? aggregate.before_peak.maximum : 0.0),
                  (aggregate.after_peak.count != 0u ? aggregate.after_peak.minimum : 0.0),
                  (aggregate.after_peak.count != 0u ? aggregate.after_peak.maximum : 0.0),
                  aggregate.peak_lowered_executions,
                  aggregate.dropped_above_one_executions);
            }
          }

          auto result_json = json{
              {"resourceHandle", formatting::FormatHandle(session->watched_resource.handle)},
              {"usedClone", session->used_clone},
              {"format", session->resource_format_name},
              {"region", json{{"x", session->region.left}, {"y", session->region.top}, {"width", session->width}, {"height", session->height}}},
              {"signal", std::string(resource_analysis::internal::GetSignalName(session->signal))},
              {"units", unit},
              {"framesRequested", session->frames},
              {"framesMeasured", session->frames_decoded},
              {"truncatedFrames", session->truncated_frames},
              {"shaderHashFilter", (session->shader_hash.has_value() ? json(formatting::FormatShaderHash(session->shader_hash.value())) : json(nullptr))},
              {"writerExecutions", session->events_decoded},
              {"writerExecutionsChanged", session->events_changed},
              {"writerExecutionsUnmeasured", session->events_unmeasured},
              {"unmeasured", json{
                                 {"reasons", reasons_to_json(session->unmeasured_reasons)},
                                 {"frames", session->frames_with_unmeasured_writers},
                             }},
              {"writers", std::move(writers)},
              {"summary", summary},
          };
          if (session->explicit_api) {
            result_json["scratchBudgetMB"] = session->scratch_budget_mb;
            result_json["scratchEntries"] = session->scratch_entries.size();
            result_json["scratchEntriesMax"] = session->max_scratch_entries;
            result_json["scratchEntriesNeeded"] = session->scratch_entries_needed;
            result_json["closingFrames"] = session->closing_frames;
            if (!session->swap_chain_images.empty()) {
              result_json["swapChainImages"] = session->swap_chain_images.size();
            }
          }
          if (session->device_api == reshade::api::device_api::d3d9 || session->device_api == reshade::api::device_api::opengl) {
            result_json["scratchBudgetMB"] = session->scratch_budget_mb;
            result_json["atlasSlots"] = session->slot_capacity;
          }
          if (session->frames == 1u) {
            result_json["trace"] = std::move(session->trace);
          }
          if (session->end_stats.has_value()) {
            const auto& end_stats = session->end_stats.value();
            result_json["endOfWatch"] = stats_to_json(end_stats);
            summary += std::format(
                "\nEnd of the last frame (after every add-on's present handler): peak {:.4f}, mean {:.4f}, {} pixel(s) above 1.0.",
                end_stats.max_luminance,
                end_stats.mean_luminance,
                end_stats.above_one_count);
          }

          if (session->verify) {
            reshade::api::subresource_data verify_data = {};
            if (!session->verify_gpu_stats.has_value() || !session->verify_copied
                || !device->map_texture_region(session->verify_readback, 0u, nullptr, reshade::api::map_access::read_only, &verify_data)) {
              result_json["verify"] = json{{"status", "skipped"}, {"reason", "No measurement point was available at the end of the watch (D3D12/Vulkan: the texture is not the presented back buffer and no exact measurement point followed the last frame within the closing frames, or that point was cross_queue_not_ready), or the CPU readback failed."}};
              summary += "\nVerify: skipped (no measurement at the end of the watch).";
            } else {
              // Same definitions as watch_region_reduce: non-finite pixels are excluded, luminance is clamped at 0 for
              // min/max/mean and at LUMINANCE_SUM_CLAMP for the mean.
              RegionStats cpu_stats = {};
              double cpu_luminance_sum = 0.0;
              double cpu_min_luminance = std::numeric_limits<double>::infinity();
              bool decoded = true;
              const auto pixel_stride = static_cast<std::size_t>(reshade::api::format_row_pitch(session->scratch_format, 1));
              constexpr std::array<double, 4> NITS_THRESHOLDS = {203.0, 400.0, 1000.0, 4000.0};
              for (std::uint32_t y = 0u; y < session->height && decoded; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(verify_data.data) + (static_cast<std::size_t>(verify_data.row_pitch) * y);
                for (std::uint32_t x = 0u; x < session->width; ++x) {
                  float rgba[4] = {0.f, 0.f, 0.f, 1.f};
                  std::uint32_t channel_count = 4u;
                  if (!resource_analysis::internal::DecodePixelToLinear(session->scratch_format, row + (pixel_stride * x), rgba, channel_count)) {
                    decoded = false;
                    break;
                  }
                  if (!std::isfinite(rgba[0]) || !std::isfinite(rgba[1]) || !std::isfinite(rgba[2])) {
                    ++cpu_stats.non_finite_count;
                    continue;
                  }
                  std::array<double, 3> display = {rgba[0], rgba[1], rgba[2]};
                  double luminance = (0.2126 * rgba[0]) + (0.7152 * rgba[1]) + (0.0722 * rgba[2]);
                  if (session->signal != Signal::RAW) {
                    const auto display_light = resource_analysis::internal::DecodeDisplayLight(session->signal, rgba);
                    display = display_light.nits;
                    luminance = display_light.luminance;
                  }
                  const auto non_negative_luminance = std::max(luminance, 0.0);
                  ++cpu_stats.finite_count;
                  cpu_stats.max_luminance = std::max(cpu_stats.max_luminance, non_negative_luminance);
                  cpu_min_luminance = std::min(cpu_min_luminance, non_negative_luminance);
                  cpu_stats.max_channel = std::max({cpu_stats.max_channel, display[0], display[1], display[2]});
                  cpu_luminance_sum += std::min(non_negative_luminance, LUMINANCE_SUM_CLAMP);
                  if (rgba[0] < 0.f || rgba[1] < 0.f || rgba[2] < 0.f) {
                    ++cpu_stats.negative_count;
                  }
                  if (rgba[0] > 1.f || rgba[1] > 1.f || rgba[2] > 1.f) {
                    ++cpu_stats.above_one_count;
                  }
                  if (rgba[0] == 0.f && rgba[1] == 0.f && rgba[2] == 0.f) {
                    ++cpu_stats.black_count;
                  }
                  if (std::isfinite(rgba[3])) {
                    cpu_stats.max_alpha = std::max(cpu_stats.max_alpha, std::max(static_cast<double>(rgba[3]), 0.0));
                    if (rgba[3] > 1.f) {
                      ++cpu_stats.alpha_above_one_count;
                    }
                  }
                  if (session->signal != Signal::RAW) {
                    for (std::size_t threshold_index = 0u; threshold_index < NITS_THRESHOLDS.size(); ++threshold_index) {
                      if (luminance > NITS_THRESHOLDS[threshold_index]) {
                        ++cpu_stats.above_nits_counts[threshold_index];
                      }
                    }
                  }
                }
              }
              device->unmap_texture_region(session->verify_readback, 0u);

              if (!decoded) {
                result_json["verify"] = json{{"status", "skipped"}, {"reason", std::format("CPU decoding is not implemented for {}.", session->resource_format_name)}};
                summary += "\nVerify: skipped (format not decodable on the CPU).";
              } else {
                if (cpu_stats.finite_count != 0u) {
                  cpu_stats.min_luminance = cpu_min_luminance;
                  cpu_stats.mean_luminance = cpu_luminance_sum / static_cast<double>(cpu_stats.finite_count);
                }
                const auto& gpu_stats = session->verify_gpu_stats.value();
                auto mismatches = json::array();
                const auto compare_count = [&](std::string_view field, std::uint32_t gpu_value, std::uint32_t cpu_value, std::uint32_t allowed_difference) {
                  const auto difference = (gpu_value > cpu_value ? gpu_value - cpu_value : cpu_value - gpu_value);
                  if (difference > allowed_difference) {
                    mismatches.push_back(json{{"field", std::string(field)}, {"gpu", gpu_value}, {"cpu", cpu_value}});
                  }
                };
                const auto compare_value = [&](std::string_view field, double gpu_value, double cpu_value) {
                  const auto allowed_difference = std::max(VERIFY_ABSOLUTE_TOLERANCE, VERIFY_RELATIVE_TOLERANCE * std::max(std::abs(gpu_value), std::abs(cpu_value)));
                  if (std::abs(gpu_value - cpu_value) > allowed_difference) {
                    mismatches.push_back(json{{"field", std::string(field)}, {"gpu", gpu_value}, {"cpu", cpu_value}});
                  }
                };
                compare_count("finitePixels", gpu_stats.finite_count, cpu_stats.finite_count, 0u);
                compare_count("nonFinitePixels", gpu_stats.non_finite_count, cpu_stats.non_finite_count, 0u);
                compare_count("negativePixels", gpu_stats.negative_count, cpu_stats.negative_count, 0u);
                compare_count("pixelsAboveOne", gpu_stats.above_one_count, cpu_stats.above_one_count, 0u);
                compare_count("blackPixels", gpu_stats.black_count, cpu_stats.black_count, 0u);
                // GPU float and CPU double nits can land on opposite sides of a threshold for a few pixels.
                const auto allowed_nits_difference = std::max<std::uint32_t>(2u, (session->width * session->height) / 10000u);
                compare_count("pixelsAboveNits.203", gpu_stats.above_nits_counts[0], cpu_stats.above_nits_counts[0], allowed_nits_difference);
                compare_count("pixelsAboveNits.400", gpu_stats.above_nits_counts[1], cpu_stats.above_nits_counts[1], allowed_nits_difference);
                compare_count("pixelsAboveNits.1000", gpu_stats.above_nits_counts[2], cpu_stats.above_nits_counts[2], allowed_nits_difference);
                compare_count("pixelsAboveNits.4000", gpu_stats.above_nits_counts[3], cpu_stats.above_nits_counts[3], allowed_nits_difference);
                compare_value("maxLuminance", gpu_stats.max_luminance, cpu_stats.max_luminance);
                compare_value("minLuminance", gpu_stats.min_luminance, cpu_stats.min_luminance);
                compare_value("meanLuminance", gpu_stats.mean_luminance, cpu_stats.mean_luminance);
                compare_value("maxChannel", gpu_stats.max_channel, cpu_stats.max_channel);
                compare_count("pixelsAlphaAboveOne", gpu_stats.alpha_above_one_count, cpu_stats.alpha_above_one_count, 0u);
                compare_value("maxAlpha", gpu_stats.max_alpha, cpu_stats.max_alpha);

                const auto verify_matched = mismatches.empty();
                summary += std::format("\nVerify (CPU readback of the same measurement point): {}.", (verify_matched ? "match" : "MISMATCH"));
                result_json["verify"] = json{
                    {"status", (verify_matched ? "match" : "mismatch")},
                    {"gpu", stats_to_json(gpu_stats)},
                    {"cpu", stats_to_json(cpu_stats)},
                    {"mismatches", std::move(mismatches)},
                };
              }
            }
          }
          result_json["summary"] = summary;
          complete_request(session->request, ToolResult{.text = summary, .structured_content = std::move(result_json), .is_error = false});
        }
      }

      internal::DestroySessionObjects(session.get(), device);
    }
    for (const auto texture : released_textures) {
      device->destroy_resource(texture);
    }
    if (finished_session != nullptr) {
      // Frees what the wait at the end of the watch showed the GPU is done with, without waiting for the next frame end.
      ProcessLifetimes(watcher, device, false);
    }
  }

  // Starts the next queued watch. Validation and GPU object creation run outside the lock. Frame ends of several
  // runtimes may get here at once: the session is installed only if none was installed meanwhile.
  while (true) {
    std::shared_ptr<Request> request;
    {
      WatcherLock lock(watcher, "ProcessAtFrameEnd: taking the next request");
      if (watcher->session != nullptr || watcher->pending_requests.empty()) break;
      request = watcher->pending_requests.front();
      watcher->pending_requests.pop_front();
    }
    {
      std::scoped_lock request_lock(request->mutex);
      if (request->completed || request->canceled) continue;
    }

    const auto device_api = device->get_api();
    if (device_api != reshade::api::device_api::d3d9 && device_api != reshade::api::device_api::d3d11 && device_api != reshade::api::device_api::opengl
        && !internal::IsExplicitApi(device_api)) {
      complete_request(request, make_error("devkit_watch_region supports D3D9Ex, D3D11, D3D12, Vulkan and OpenGL devices only."));
      continue;
    }

    resource_analysis::internal::AnalysisTarget target = {};
    try {
      target = resource_analysis::internal::ResolveAnalysisTarget(request->resource_view, device, request->prefer_clone);
    } catch (const std::exception& exception) {
      complete_request(request, make_error(exception.what()));
      continue;
    }

    // A back buffer is watched by the runtime that presents it, whose frame ends take its final measurement; any other
    // texture by the first runtime that gets here. A back buffer of another swap chain waits for that one's frame end.
    std::vector<std::uint64_t> back_buffer_handles;
    for (std::uint32_t index = 0u; index < runtime->get_back_buffer_count(); ++index) {
      back_buffer_handles.push_back(runtime->get_back_buffer(index).handle);
    }
    const auto watches_own_back_buffer =
        (std::ranges::find(back_buffer_handles, target.resolved_resource.handle) != back_buffer_handles.end());
    if (!watches_own_back_buffer && renodx::utils::swapchain::IsBackBuffer(target.resolved_resource)) {
      WatcherLock lock(watcher, "ProcessAtFrameEnd: requeueing another swap chain's request");
      watcher->pending_requests.push_front(request);
      break;
    }
    {
      std::scoped_lock request_lock(request->mutex);
      if (request->completed || request->canceled) continue;
      request->started = true;
    }
    request->cv.notify_all();

    const auto& resource_desc = target.resolved_resource_desc;
    if (resource_desc.type != reshade::api::resource_type::texture_2d && resource_desc.type != reshade::api::resource_type::surface) {
      complete_request(request, make_error("Only 2D texture resource views can be watched."));
      continue;
    }
    if (resource_desc.texture.samples != 1u) {
      complete_request(request, make_error("Multisampled resources cannot be watched."));
      continue;
    }
    // The watch copies a box of the texture; D3D10/11 drop and D3D12 rejects a box copy of a depth-stencil resource.
    if (renodx::utils::bitwise::HasAnyFlag(resource_desc.usage, reshade::api::resource_usage::depth_stencil)) {
      complete_request(request, make_error("Depth-stencil resources cannot be watched."));
      continue;
    }

    // A typeless resource is measured as its view types it (R16_TYPELESS viewed as R16_UNORM is not half floats); the
    // non-sRGB variant is kept because the sRGB decode happens in the view the reduction reads through.
    const auto view_format = target.resolved_view_desc.format;
    const bool view_types_resource = (reshade::api::format_is_typeless(resource_desc.texture.format)
                                      && view_format != reshade::api::format::unknown
                                      && reshade::api::format_to_typeless(view_format) == resource_desc.texture.format);
    auto scratch_format = reshade::api::format_to_default_typed((view_types_resource ? view_format : resource_desc.texture.format), 0);
    if (device_api == reshade::api::device_api::opengl
        && (scratch_format == reshade::api::format::r8g8b8x8_unorm || scratch_format == reshade::api::format::b8g8r8x8_unorm)
        && static_cast<unsigned int>(target.resolved_resource.handle >> 40u) == gl_enum::FRAMEBUFFER_DEFAULT) {
      // A back buffer without alpha bits is reported as an x8 format: it is watched as RGBA8, since its copy is a blit. A
      // GL_RGB8 texture is reported as x8 too, but cannot be copied into an RGBA8 atlas and stays refused.
      scratch_format = reshade::api::format::r8g8b8a8_unorm;
    }
    // OpenGL keeps every colour as logical RGBA whatever the window's pixel format says: a BGRA label (the default
    // framebuffer's, from the pixel format descriptor) makes the CPU readback of `verify` decode red and blue swapped.
    if (device_api == reshade::api::device_api::opengl) {
      if (scratch_format == reshade::api::format::b8g8r8a8_unorm) {
        scratch_format = reshade::api::format::r8g8b8a8_unorm;
      } else if (scratch_format == reshade::api::format::b8g8r8a8_unorm_srgb) {
        scratch_format = reshade::api::format::r8g8b8a8_unorm_srgb;
      }
    }
    switch (scratch_format) {
      case reshade::api::format::r8g8b8a8_unorm:
      case reshade::api::format::r8g8b8a8_unorm_srgb:
      case reshade::api::format::b8g8r8a8_unorm:
      case reshade::api::format::b8g8r8a8_unorm_srgb:
      case reshade::api::format::r10g10b10a2_unorm:
      case reshade::api::format::r11g11b10_float:
      case reshade::api::format::r16g16b16a16_unorm:
      case reshade::api::format::r16g16b16a16_float:
      case reshade::api::format::r32g32b32a32_float:
        break;
      // The reduction loads float4 and missing channels read 0, so one- and two-channel values are measured in R (and G):
      // the peak is the largest channel, the luminance means nothing and a signal is refused. OpenGL has no image format
      // for R9G9B9E5; D3D9 was not tried.
      case reshade::api::format::r8_unorm:
      case reshade::api::format::r16_unorm:
      case reshade::api::format::r16_float:
      case reshade::api::format::r32_float:
      case reshade::api::format::r8g8_unorm:
      case reshade::api::format::r16g16_unorm:
      case reshade::api::format::r16g16_float:
      case reshade::api::format::r32g32_float:
      case reshade::api::format::r9g9b9e5:
        if (device_api == reshade::api::device_api::d3d9
            || (device_api == reshade::api::device_api::opengl && scratch_format == reshade::api::format::r9g9b9e5)) {
          complete_request(request, make_error(std::format(
                                        "Watching format {} is not supported on this API.",
                                        formatting::StreamToString(scratch_format))));
          continue;
        }
        if (request->signal != Signal::RAW && scratch_format != reshade::api::format::r9g9b9e5) {
          complete_request(request, make_error(std::format(
                                        "Format {} has fewer than three channels, so a signal (nits) does not apply; watch it "
                                        "without a signal and read the peak (its largest channel).",
                                        formatting::StreamToString(scratch_format))));
          continue;
        }
        break;
      // D3D9 back buffers (D3DFMT_X8R8G8B8, D3DFMT_A2R10G10B10); not tried on the other APIs.
      case reshade::api::format::b8g8r8x8_unorm:
      case reshade::api::format::b10g10r10a2_unorm:
        if (device_api == reshade::api::device_api::d3d9) break;
        [[fallthrough]];
      default:
        complete_request(request, make_error(std::format(
                                      "Watching format {} is not supported.{}",
                                      formatting::StreamToString(scratch_format),
                                      (device_api == reshade::api::device_api::opengl ? " OpenGL three-channel formats (RGB8, RGB16F, RGB32F) have no image format and cannot be watched." : ""))));
        continue;
    }

    VulkanCmdPipelineBarrier vulkan_cmd_pipeline_barrier = nullptr;
    if (device_api == reshade::api::device_api::d3d12) {
      // Simultaneous-access textures can be promoted to almost any state implicitly, so no state is exactly known.
      const auto native_desc = reinterpret_cast<ID3D12Resource*>(static_cast<std::uintptr_t>(target.resolved_resource.handle))->GetDesc();
      if ((native_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0) {
        complete_request(request, make_error("Textures created with D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS cannot be watched: their state is not known at any point."));
        continue;
      }
    }
    if (device_api == reshade::api::device_api::vulkan) {
      if ((resource_desc.usage & reshade::api::resource_usage::copy_source) == 0) {
        complete_request(request, make_error("The image was created without VK_IMAGE_USAGE_TRANSFER_SRC_BIT, so its region cannot be copied. The Devkit setting \"Watch Region: Copyable Vulkan Images\" (Settings > Other) adds it and is on by default; if it was turned off, turn it on and restart the game. It skips shared and sparse images and formats without copy support, and cannot reach images created before the Devkit loaded or fallback copies a mod creates from a desc recorded before the Devkit's handler ran."));
        continue;
      }
      // Per-subresource barriers need vkCmdPipelineBarrier itself; called through the loader so ReShade sees it too.
      auto* vulkan_module = GetModuleHandleW(L"vulkan-1.dll");
      auto* get_device_proc_addr = (vulkan_module != nullptr ? reinterpret_cast<VulkanGetDeviceProcAddr>(GetProcAddress(vulkan_module, "vkGetDeviceProcAddr")) : nullptr);
      if (get_device_proc_addr != nullptr) {
        vulkan_cmd_pipeline_barrier = reinterpret_cast<VulkanCmdPipelineBarrier>(
            get_device_proc_addr(reinterpret_cast<void*>(static_cast<std::uintptr_t>(device->get_native())), "vkCmdPipelineBarrier"));
      }
      if (vulkan_cmd_pipeline_barrier == nullptr) {
        complete_request(request, make_error("vkCmdPipelineBarrier could not be resolved from vulkan-1.dll."));
        continue;
      }
    }

    const auto first_level = target.resolved_view_desc.texture.first_level;
    const auto first_layer = target.resolved_view_desc.texture.first_layer;
    const auto levels = std::max<std::uint32_t>(resource_desc.texture.levels, 1u);
    const auto layers = std::max<std::uint32_t>(resource_desc.texture.depth_or_layers, 1u);
    const auto texture_width = std::max<std::uint32_t>(resource_desc.texture.width >> first_level, 1u);
    const auto texture_height = std::max<std::uint32_t>(resource_desc.texture.height >> first_level, 1u);
    const auto region = request->region.value_or(reshade::api::subresource_box{
        .left = 0u,
        .top = 0u,
        .front = 0u,
        .right = texture_width,
        .bottom = texture_height,
        .back = 1u,
    });
    if (region.right > texture_width || region.bottom > texture_height) {
      complete_request(request, make_error(std::format(
                                    "The region {}x{} at ({}, {}) does not fit the {}x{} texture.",
                                    region.width(),
                                    region.height(),
                                    region.left,
                                    region.top,
                                    texture_width,
                                    texture_height)));
      continue;
    }

    // OpenGL: the entry points, the context the watch belongs to, the device limits the atlas and the dispatch must respect,
    // and the atlas format. The atlas is the non-sRGB twin of an sRGB source: glCopyImageSubData copies the bits and the
    // shader applies the sRGB decode, as D3D11 does through an sRGB shader resource view.
    OpenGLReduction opengl_setup;
    int opengl_max_texture_size = 0;
    auto opengl_atlas_format = reshade::api::format::unknown;
    std::string_view opengl_image_qualifier;
    bool opengl_srgb_decode = false;
    if (device_api == reshade::api::device_api::opengl) {
      const auto* functions = GetOpenGLFunctions();
      if (functions == nullptr || functions->get_current_context() == nullptr) {
        complete_request(request, make_error("The OpenGL entry points of the watch could not be loaded (no complete OpenGL 4.3 set, or no context is current)."));
        continue;
      }
      opengl_setup.functions = functions;
      opengl_setup.context = functions->get_current_context();
      switch (scratch_format) {
        case reshade::api::format::r8g8b8a8_unorm_srgb:
        case reshade::api::format::b8g8r8a8_unorm_srgb:
          opengl_srgb_decode = true;
          [[fallthrough]];
        case reshade::api::format::r8g8b8a8_unorm:
        case reshade::api::format::b8g8r8a8_unorm:
          opengl_atlas_format = reshade::api::format::r8g8b8a8_unorm;
          opengl_image_qualifier = "rgba8";
          break;
        case reshade::api::format::r10g10b10a2_unorm:
          opengl_atlas_format = reshade::api::format::r10g10b10a2_unorm;
          opengl_image_qualifier = "rgb10_a2";
          break;
        case reshade::api::format::r11g11b10_float:
          opengl_atlas_format = reshade::api::format::r11g11b10_float;
          opengl_image_qualifier = "r11f_g11f_b10f";
          break;
        case reshade::api::format::r16g16b16a16_unorm:
          opengl_atlas_format = reshade::api::format::r16g16b16a16_unorm;
          opengl_image_qualifier = "rgba16";
          break;
        case reshade::api::format::r16g16b16a16_float:
          opengl_atlas_format = reshade::api::format::r16g16b16a16_float;
          opengl_image_qualifier = "rgba16f";
          break;
        case reshade::api::format::r8_unorm:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "r8";
          break;
        case reshade::api::format::r16_unorm:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "r16";
          break;
        case reshade::api::format::r16_float:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "r16f";
          break;
        case reshade::api::format::r32_float:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "r32f";
          break;
        case reshade::api::format::r8g8_unorm:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "rg8";
          break;
        case reshade::api::format::r16g16_unorm:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "rg16";
          break;
        case reshade::api::format::r16g16_float:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "rg16f";
          break;
        case reshade::api::format::r32g32_float:
          opengl_atlas_format = scratch_format;
          opengl_image_qualifier = "rg32f";
          break;
        default:
          opengl_atlas_format = reshade::api::format::r32g32b32a32_float;
          opengl_image_qualifier = "rgba32f";
          break;
      }

      // Errors the game left pending would be taken for the watch's own by the checks after its first copy and reduction.
      DrainOpenGLErrors(*functions);
      int max_image_units = 0;
      int max_draw_buffers = 0;
      std::array<int, 3> max_group_counts = {};
      functions->get_integerv(gl_enum::MAX_TEXTURE_SIZE, &opengl_max_texture_size);
      functions->get_integerv(gl_enum::MAX_IMAGE_UNITS, &max_image_units);
      functions->get_integerv(gl_enum::MAX_DRAW_BUFFERS, &max_draw_buffers);
      for (std::uint32_t axis = 0u; axis < max_group_counts.size(); ++axis) {
        functions->get_integeri_v(gl_enum::MAX_COMPUTE_WORK_GROUP_COUNT, axis, &max_group_counts[axis]);
      }
      // ReShade's bind_pipeline disables the alpha test on what it calls a compatibility context: one that lists
      // GL_ARB_compatibility, or that the game created with wglCreateContext (the extension is not always listed for those).
      // Both are covered by "not a core profile", and the same test decides whether the watch restores the alpha test.
      int profile_mask = 0;
      functions->get_integerv(gl_enum::CONTEXT_PROFILE_MASK, &profile_mask);
      opengl_setup.compatibility_context = (static_cast<unsigned int>(profile_mask) & gl_enum::CONTEXT_CORE_PROFILE_BIT) == 0u;
      int extension_count = 0;
      functions->get_integerv(gl_enum::NUM_EXTENSIONS, &extension_count);
      for (int index = 0; index < extension_count && !opengl_setup.compatibility_context; ++index) {
        const auto* extension = functions->get_stringi(gl_enum::EXTENSIONS, static_cast<unsigned int>(index));
        opengl_setup.compatibility_context = (extension != nullptr && std::string_view(reinterpret_cast<const char*>(extension)) == "GL_ARB_compatibility");
      }

      if (max_image_units < 2 || opengl_max_texture_size <= 0 || max_draw_buffers <= 0) {
        complete_request(request, make_error("The OpenGL context reports fewer than two image units or no usable texture size, which the region reduction needs."));
        continue;
      }
      if (region.width() > static_cast<std::uint32_t>(opengl_max_texture_size) || region.height() > static_cast<std::uint32_t>(opengl_max_texture_size)
          || (region.width() + GROUP_SIZE - 1u) / GROUP_SIZE > static_cast<std::uint32_t>(max_group_counts[0])
          || (region.height() + GROUP_SIZE - 1u) / GROUP_SIZE > static_cast<std::uint32_t>(max_group_counts[1])
          || MAX_SLOTS > static_cast<std::uint32_t>(max_group_counts[2])) {
        complete_request(request, make_error("The region is larger than this OpenGL context can reduce (texture size or compute work group count): use a smaller region."));
        continue;
      }
      opengl_setup.max_image_units = static_cast<std::uint32_t>(max_image_units);
      opengl_setup.max_draw_buffers = static_cast<std::uint32_t>(std::min(max_draw_buffers, 8));
      opengl_setup.atlas_unit = opengl_setup.max_image_units - 1u;
      opengl_setup.stats_unit = opengl_setup.max_image_units - 2u;
      opengl_setup.watches_default_framebuffer = (static_cast<unsigned int>(target.resolved_resource.handle >> 40u) == gl_enum::FRAMEBUFFER_DEFAULT);
      if (opengl_setup.watches_default_framebuffer) {
        // ReShade's copy from the default framebuffer leaves its read buffer at GL_BACK; the game's value is put back
        // after each copy when it differs (MeasureRegion).
        int read_framebuffer = 0;
        functions->get_integerv(gl_enum::READ_FRAMEBUFFER_BINDING, &read_framebuffer);
        if (read_framebuffer != 0) {
          functions->bind_framebuffer(gl_enum::READ_FRAMEBUFFER, 0u);
        }
        functions->get_integerv(gl_enum::READ_BUFFER, &opengl_setup.default_read_buffer);
        if (read_framebuffer != 0) {
          functions->bind_framebuffer(gl_enum::READ_FRAMEBUFFER, static_cast<unsigned int>(read_framebuffer));
        }
      }
    }

    // D3D9: the readback fence needs D3D9Ex (gpu_fence's D3DKMT backend), the reduction four render targets, and the
    // float pixel counts of the reduction stay exact up to D3D9_MAX_REGION_PIXELS.
    D3DCAPS9 d3d9_caps = {};
    if (device_api == reshade::api::device_api::d3d9) {
      auto* native_device = reinterpret_cast<IDirect3DDevice9*>(static_cast<std::uintptr_t>(device->get_native()));
      IDirect3DDevice9Ex* native_device_ex = nullptr;
      if (FAILED(native_device->QueryInterface(IID_PPV_ARGS(&native_device_ex)))) {
        complete_request(request, make_error("DX9 watches need D3D9Ex: run the game with a RenoDX proxy mod or a wrapper that creates the device with CreateDeviceEx (for example dxwrapper's D3d9to9Ex), or turn on Force DX9Ex (boot) in the Devkit's proxy settings and restart the game (Force DX9Ex only works for games that start windowed or borderless)."));
        continue;
      }
      native_device_ex->Release();
      const auto native_type = reinterpret_cast<IDirect3DResource9*>(static_cast<std::uintptr_t>(target.resolved_resource.handle))->GetType();
      if (native_type != D3DRTYPE_SURFACE && native_type != D3DRTYPE_TEXTURE) {
        complete_request(request, make_error("Only 2D textures and surfaces can be watched on Direct3D 9."));
        continue;
      }
      if (static_cast<std::uint64_t>(region.width()) * region.height() > D3D9_MAX_REGION_PIXELS) {
        complete_request(request, make_error("On Direct3D 9 a watched region can have at most 16777216 pixels (4096x4096): use a region."));
        continue;
      }
      if (FAILED(native_device->GetDeviceCaps(&d3d9_caps)) || d3d9_caps.NumSimultaneousRTs < D3D9_STATS_GROUPS) {
        complete_request(request, make_error("The Direct3D 9 device does not support four simultaneous render targets, which the region reduction needs."));
        continue;
      }
    }

    auto new_session = std::make_shared<Session>();
    new_session->request = request;
    new_session->runtime = runtime;
    new_session->watched_resource = target.resolved_resource;
    new_session->original_resource = device->get_resource_from_view(request->resource_view);
    new_session->immediate_cmd_list = queue->get_immediate_command_list();
    new_session->subresource = first_level + (first_layer * levels);
    new_session->mip_level = first_level;
    new_session->array_layer = first_layer;
    new_session->single_subresource = (levels * layers == 1u);
    new_session->region = region;
    new_session->width = region.width();
    new_session->height = region.height();
    new_session->signal = request->signal;
    new_session->frames = request->frames;
    new_session->shader_hash = request->shader_hash;
    new_session->include_unchanged = request->include_unchanged;
    new_session->with_snapshot = request->with_snapshot;
    new_session->verify = request->verify;
    new_session->scratch_format = scratch_format;
    new_session->used_clone = target.used_clone;
    new_session->resource_format_name = formatting::StreamToString(scratch_format);
    new_session->device_api = device_api;
    new_session->explicit_api = internal::IsExplicitApi(device_api);
    new_session->warming_up = new_session->explicit_api;
    new_session->queue = queue;
    new_session->vulkan_cmd_pipeline_barrier = vulkan_cmd_pipeline_barrier;
    new_session->scratch_budget_mb = request->scratch_budget_mb;
    new_session->opengl = opengl_setup;
    if (new_session->explicit_api && watches_own_back_buffer) {
      new_session->swap_chain_images = std::move(back_buffer_handles);
    }
    const auto scratch_entry_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(reshade::api::format_row_pitch(scratch_format, new_session->width)) * new_session->height, 1u);
    new_session->max_scratch_entries = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        (static_cast<std::uint64_t>(new_session->scratch_budget_mb) * 1024u * 1024u) / scratch_entry_bytes, 1u, MAX_SCRATCH_ENTRIES));

    bool created = false;
    if (device_api == reshade::api::device_api::d3d9) {
      // The atlas: as many region-sized slots as the scratch budget and the device's texture size allow (at least the
      // end-of-frame one), at most MAX_SLOTS, in rows of `atlas_columns`.
      auto& d3d9 = new_session->d3d9;
      const auto budget_slots = (static_cast<std::uint64_t>(new_session->scratch_budget_mb) * 1024u * 1024u) / scratch_entry_bytes;
      const auto fitting_columns = std::min<std::uint64_t>(d3d9_caps.MaxTextureWidth / new_session->width, MAX_SLOTS);
      const auto fitting_rows = static_cast<std::uint64_t>(d3d9_caps.MaxTextureHeight / new_session->height);
      new_session->slot_capacity = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
          std::min({budget_slots, fitting_columns * fitting_rows, static_cast<std::uint64_t>(MAX_SLOTS)}), 1u, MAX_SLOTS));
      d3d9.atlas_columns = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(fitting_columns, 1u, new_session->slot_capacity));
      d3d9.atlas_width = d3d9.atlas_columns * new_session->width;
      d3d9.atlas_height = ((new_session->slot_capacity + d3d9.atlas_columns - 1u) / d3d9.atlas_columns) * new_session->height;

      // Intermediate levels until one 8x8 block is left; the last pass reduces that block into the stats row.
      auto level_width = (new_session->width + D3D9_REDUCTION_BLOCK_SIZE - 1u) / D3D9_REDUCTION_BLOCK_SIZE;
      auto level_height = (new_session->height + D3D9_REDUCTION_BLOCK_SIZE - 1u) / D3D9_REDUCTION_BLOCK_SIZE;
      while (level_width > 1u || level_height > 1u) {
        d3d9.levels.push_back(D3D9ReductionLevel{.width = level_width, .height = level_height});
        level_width = (level_width + D3D9_REDUCTION_BLOCK_SIZE - 1u) / D3D9_REDUCTION_BLOCK_SIZE;
        level_height = (level_height + D3D9_REDUCTION_BLOCK_SIZE - 1u) / D3D9_REDUCTION_BLOCK_SIZE;
      }

      auto* native_device = reinterpret_cast<IDirect3DDevice9*>(static_cast<std::uintptr_t>(device->get_native()));
      const std::array<D3DVERTEXELEMENT9, 2> vertex_elements = {{
          {.Stream = 0u, .Offset = 0u, .Type = D3DDECLTYPE_FLOAT2, .Method = D3DDECLMETHOD_DEFAULT, .Usage = D3DDECLUSAGE_POSITION, .UsageIndex = 0u},
          D3DDECL_END(),
      }};
      IDirect3DStateBlock9* state_block = nullptr;
      IDirect3DVertexDeclaration9* vertex_declaration = nullptr;
      IDirect3DVertexShader9* vertex_shader = nullptr;
      IDirect3DPixelShader9* pixel_shader = nullptr;
      const auto native_created = SUCCEEDED(native_device->CreateStateBlock(D3DSBT_ALL, &state_block))
                                  && SUCCEEDED(native_device->CreateVertexDeclaration(vertex_elements.data(), &vertex_declaration))
                                  && SUCCEEDED(native_device->CreateVertexShader(reinterpret_cast<const DWORD*>(d3d9_vertex_shader.data()), &vertex_shader))
                                  && SUCCEEDED(native_device->CreatePixelShader(reinterpret_cast<const DWORD*>(reduce_shader.data()), &pixel_shader));
      d3d9.state_block.reset(state_block);
      d3d9.vertex_declaration.reset(vertex_declaration);
      d3d9.vertex_shader.reset(vertex_shader);
      d3d9.pixel_shader.reset(pixel_shader);

      const auto create_target = [&](std::uint32_t width, std::uint32_t height, reshade::api::format format, reshade::api::resource* target) {
        return device->create_resource(
            reshade::api::resource_desc(width, height, 1, 1, format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::render_target | reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_source | reshade::api::resource_usage::copy_dest),
            nullptr,
            reshade::api::resource_usage::render_target,
            target);
      };
      created = native_created
                && create_target(d3d9.atlas_width, d3d9.atlas_height, scratch_format, &new_session->scratch)
                && create_target(D3D9_STATS_GROUPS, MAX_SLOTS, reshade::api::format::r32g32b32a32_float, &new_session->stats_texture)
                && std::ranges::all_of(d3d9.levels, [&](D3D9ReductionLevel& level) {
                     return std::ranges::all_of(level.targets, [&](reshade::api::resource& level_target) {
                       return create_target(level.width, level.height, reshade::api::format::r32g32b32a32_float, &level_target);
                     });
                   })
                && std::ranges::all_of(new_session->readbacks, [&](auto& readback) {
                     return device->create_resource(
                         reshade::api::resource_desc(D3D9_STATS_GROUPS, MAX_SLOTS, 1, 1, reshade::api::format::r32g32b32a32_float, 1, reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
                         nullptr,
                         reshade::api::resource_usage::copy_dest,
                         &readback);
                   });
      if (created) {
        new_session->stats_timeline = internal::CreateFenceTimeline(device);
        if (new_session->stats_timeline == nullptr) {
          internal::DestroySessionObjects(new_session.get(), device);
          complete_request(request, make_error("The Direct3D 9 readback fence (gpu_fence, D3DKMT) could not be created; see ReShade.log."));
          continue;
        }
      }
    } else if (device_api == reshade::api::device_api::opengl) {
      // The atlas: as many region-sized cells as the scratch budget and the context's texture size allow (at least the
      // end-of-frame one), at most MAX_SLOTS, in rows of `atlas_columns`.
      auto& opengl = new_session->opengl;
      const auto budget_slots = (static_cast<std::uint64_t>(new_session->scratch_budget_mb) * 1024u * 1024u) / scratch_entry_bytes;
      const auto fitting_columns = static_cast<std::uint64_t>(opengl_max_texture_size) / new_session->width;
      const auto fitting_rows = static_cast<std::uint64_t>(opengl_max_texture_size) / new_session->height;
      new_session->slot_capacity = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
          std::min({budget_slots, fitting_columns * fitting_rows, static_cast<std::uint64_t>(MAX_SLOTS)}), 1u, MAX_SLOTS));
      opengl.atlas_columns = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(fitting_columns, 1u, new_session->slot_capacity));
      const auto atlas_width = opengl.atlas_columns * new_session->width;
      const auto atlas_height = ((new_session->slot_capacity + opengl.atlas_columns - 1u) / opengl.atlas_columns) * new_session->height;

      // Region size, signal, sRGB decode, atlas layout and image units are compile-time constants of the program: they are
      // inserted after the #version line of the embedded source.
      const std::string glsl_source(reinterpret_cast<const char*>(reduce_shader.data()), reduce_shader.size());
      const auto version_line_end = glsl_source.find('\n');
      const auto glsl_defines = std::format(
          "\n#define REGION_WIDTH {}\n#define REGION_HEIGHT {}\n#define ATLAS_COLUMNS {}\n#define SIGNAL {}\n#define SRGB_DECODE {}\n"
          "#define ATLAS_FORMAT {}\n#define ATLAS_UNIT {}\n#define STATS_UNIT {}\n",
          new_session->width,
          new_session->height,
          opengl.atlas_columns,
          static_cast<std::uint32_t>(new_session->signal),
          (opengl_srgb_decode ? 1 : 0),
          opengl_image_qualifier,
          opengl.atlas_unit,
          opengl.stats_unit);
      const auto glsl = (version_line_end == std::string::npos ? glsl_source : (glsl_source.substr(0, version_line_end) + glsl_defines + glsl_source.substr(version_line_end)));
      reshade::api::shader_desc compute_shader_desc = {.code = glsl.data(), .code_size = glsl.size()};
      const reshade::api::pipeline_subobject compute_shader_subobject = {
          .type = reshade::api::pipeline_subobject_type::compute_shader,
          .count = 1,
          .data = &compute_shader_desc,
      };
      created = device->create_pipeline(reshade::api::pipeline_layout{0u}, 1, &compute_shader_subobject, &opengl.pipeline)
                && device->create_resource(
                    reshade::api::resource_desc(atlas_width, atlas_height, 1, 1, opengl_atlas_format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::unordered_access | reshade::api::resource_usage::copy_dest),
                    nullptr,
                    reshade::api::resource_usage::copy_dest,
                    &new_session->scratch)
                && device->create_resource_view(new_session->scratch, reshade::api::resource_usage::unordered_access, reshade::api::resource_view_desc(opengl_atlas_format), &opengl.atlas_view)
                && device->create_resource(
                    reshade::api::resource_desc(STATS_STRIDE, MAX_SLOTS, 1, 1, reshade::api::format::r32_uint, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::unordered_access | reshade::api::resource_usage::copy_source | reshade::api::resource_usage::copy_dest),
                    nullptr,
                    reshade::api::resource_usage::unordered_access,
                    &new_session->stats_texture)
                && device->create_resource_view(new_session->stats_texture, reshade::api::resource_usage::unordered_access, reshade::api::resource_view_desc(reshade::api::format::r32_uint), &opengl.stats_view)
                && std::ranges::all_of(new_session->readbacks, [&](auto& readback) {
                     return device->create_resource(
                         reshade::api::resource_desc(STATS_STRIDE, MAX_SLOTS, 1, 1, reshade::api::format::r32_uint, 1, reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
                         nullptr,
                         reshade::api::resource_usage::copy_dest,
                         &readback);
                   });
      if (created) {
        new_session->stats_timeline = internal::CreateFenceTimeline(device);
        if (new_session->stats_timeline == nullptr) {
          internal::DestroySessionObjects(new_session.get(), device);
          complete_request(request, make_error("The OpenGL readback fence could not be created; see ReShade.log."));
          continue;
        }
      }
    } else {
      created = device->create_resource(
                    reshade::api::resource_desc(new_session->width, new_session->height, 1, 1, scratch_format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_dest),
                    nullptr,
                    reshade::api::resource_usage::copy_dest,
                    &new_session->scratch)
                && device->create_resource_view(new_session->scratch, reshade::api::resource_usage::shader_resource, reshade::api::resource_view_desc(scratch_format), &new_session->scratch_srv)
                && device->create_resource(
                    reshade::api::resource_desc(STATS_STRIDE, MAX_SLOTS, 1, 1, reshade::api::format::r32_uint, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::unordered_access | reshade::api::resource_usage::copy_source | reshade::api::resource_usage::copy_dest),
                    nullptr,
                    reshade::api::resource_usage::unordered_access,
                    &new_session->stats_texture)
                && device->create_resource_view(new_session->stats_texture, reshade::api::resource_usage::unordered_access, reshade::api::resource_view_desc(reshade::api::format::r32_uint), &new_session->stats_uav)
                && std::ranges::all_of(new_session->readbacks, [&](auto& readback) {
                     return device->create_resource(
                         reshade::api::resource_desc(STATS_STRIDE, MAX_SLOTS, 1, 1, reshade::api::format::r32_uint, 1, reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
                         nullptr,
                         reshade::api::resource_usage::copy_dest,
                         &readback);
                   });
    }
    if (!created) {
      internal::DestroySessionObjects(new_session.get(), device);
      complete_request(request, make_error(std::format(
                                    "Failed to create the GPU resources for the region reduction.{}",
                                    (device_api == reshade::api::device_api::opengl ? " For OpenGL see ReShade.log for GLSL compile or link errors." : ""))));
      continue;
    }
    // Without a fence (Vulkan without timeline semaphores), a readback is only read after waiting for the queue.
    if (new_session->explicit_api) {
      new_session->stats_timeline = internal::CreateFenceTimeline(device);
    }
    // D3D12/Vulkan hooks never create scratch textures (see ReserveScratchEntry), so the pool starts here.
    if (new_session->explicit_api) {
      for (std::uint32_t index = 0u; index < std::min(INITIAL_SCRATCH_ENTRIES, new_session->max_scratch_entries); ++index) {
        const auto texture = internal::CreateScratchTexture(device, *new_session);
        if (texture.handle == 0u) break;
        new_session->scratch_entries.push_back(ScratchEntry{.texture = texture});
      }
    }

    if (device_api != reshade::api::device_api::d3d9 && device_api != reshade::api::device_api::opengl) {
      new_session->pass.pipeline_subobjects.compute_shader = reduce_shader;
      new_session->pass.shader_resource_slots.views = {new_session->scratch_srv};
      new_session->pass.unordered_access_slots.views = {new_session->stats_uav};
      new_session->pass.push_constants[renodx::utils::render::ConstantBuffersSlots{.slot = 0, .space = 0}] =
          std::span<const float>(new_session->constants.data(), new_session->constants.size());
      // D3D11 saves and restores the compute slots itself; D3D12/Vulkan render on ReShade's own immediate command list.
      new_session->pass.revert_state_after_render = false;
      new_session->pass.dispatch_group_counts = std::make_tuple(
          (new_session->width + GROUP_SIZE - 1u) / GROUP_SIZE,
          (new_session->height + GROUP_SIZE - 1u) / GROUP_SIZE,
          1u);
    }

    const auto start_message = std::format(
        "devkit::watch_region(device #{}, resource {}, {}x{} at ({}, {}), {} frame(s))",
        device_index,
        formatting::FormatHandle(new_session->watched_resource.handle),
        new_session->width,
        new_session->height,
        region.left,
        region.top,
        new_session->frames);
    const auto watched_resource_handle = new_session->watched_resource.handle;
    WatcherLock lock(watcher, "ProcessAtFrameEnd: starting a watch");
    if (watcher->session != nullptr) {
      // Another runtime's frame end started a watch meanwhile: this request waits for it.
      watcher->pending_requests.push_front(request);
      lock.Unlock();
      for (const auto& entry : new_session->scratch_entries) {
        device->destroy_resource(entry.texture);
      }
      internal::DestroySessionObjects(new_session.get(), device);
      break;
    }
    watcher->session = std::move(new_session);
    watcher->has_session.store(true, std::memory_order_release);
    watcher->opengl_context.store(watcher->session->opengl.context, std::memory_order_release);
    watcher->watched_resource_handle.store(watched_resource_handle, std::memory_order_release);
    lock.Unlock();
    if (request->with_snapshot && !internal::IsExplicitApi(device_api)) {
      begin_linked_snapshot(request);
    }
    reshade::log::message(reshade::log::level::info, start_message.c_str());
  }
}

// Fails whatever is still queued or running when the device goes away, and destroys every scratch texture (no command
// list of the device can run any more). On D3D12 the GPU may still be running region copies of the game's queues: those
// are waited for first (LIFETIME_FENCE_WAIT_TIMEOUT per queue), with one more signal on each queue that has submissions
// without a fence value; the game no longer submits while its device is destroyed. A frame end still signalling a queue
// is waited for first (bounded, like OnCommandQueueDestroyed): at process exit its thread may already be terminated;
// that queue is then neither signalled nor waited for, and its fence lives as long as that signal call.
inline void Shutdown(Watcher* watcher, reshade::api::device* device) {
  const auto signals_wait_start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - signals_wait_start < LIFETIME_FENCE_WAIT_TIMEOUT) {
    WatcherLock lock(watcher, "Shutdown: waiting for signals in flight");
    const auto has_signals_in_flight = std::ranges::any_of(watcher->queue_fences, [](const auto& queue_fence_pair) {
      const auto& [queue_handle, queue_fence] = queue_fence_pair;
      return queue_fence.signals_in_flight != 0u;
    });
    if (!has_signals_in_flight) break;
    lock.Unlock();
    std::this_thread::yield();
  }

  struct FinalSignal {
    reshade::api::command_queue* queue = nullptr;
    std::shared_ptr<FenceTimeline> timeline;
    // The highest value already attached; replaced by the final signal's value when one is needed.
    std::uint64_t value = 0u;
    bool needs_signal = false;
  };
  std::vector<FinalSignal> final_signals;
  std::uint32_t queues_still_signalling = 0u;

  // Everything is taken out under the lock and destroyed after releasing it, fences last (see FenceTimeline).
  std::unordered_map<std::uint64_t, QueueLifetimeFence> queue_fences;
  std::vector<std::shared_ptr<FenceTimeline>> orphan_timelines;
  std::shared_ptr<Session> session;
  std::vector<RetiredScratch> retired_scratch;
  std::deque<std::shared_ptr<Request>> pending_requests;
  {
    WatcherLock lock(watcher, "Shutdown");
    watcher->watched_resource_handle.store(0u, std::memory_order_release);
    for (auto& [fence_queue_handle, queue_fence] : watcher->queue_fences) {
      if (queue_fence.signals_in_flight != 0u) {
        ++queues_still_signalling;
        continue;
      }
      const auto queue_handle = fence_queue_handle;
      std::uint64_t highest_value = 0u;
      bool has_unfenced = false;
      internal::ForEachSubmissionList(watcher, [&](const std::vector<GpuSubmission>& submissions) {
        for (const auto& submission : submissions) {
          if (reinterpret_cast<std::uint64_t>(submission.queue) != queue_handle) continue;
          if (submission.fence_value.has_value()) {
            highest_value = std::max(highest_value, submission.fence_value.value());
          } else {
            has_unfenced = true;
          }
        }
      });
      if (highest_value != 0u || has_unfenced) {
        final_signals.push_back(FinalSignal{
            .queue = reinterpret_cast<reshade::api::command_queue*>(queue_handle),
            .timeline = queue_fence.timeline,
            .value = highest_value,
            .needs_signal = has_unfenced,
        });
      }
    }
    queue_fences = std::move(watcher->queue_fences);
    watcher->queue_fences.clear();
    orphan_timelines = std::move(watcher->orphan_timelines);
    watcher->orphan_timelines.clear();
    session = std::move(watcher->session);
    watcher->has_session.store(false, std::memory_order_release);
    retired_scratch = std::move(watcher->retired_scratch);
    watcher->retired_scratch.clear();
    watcher->has_retired_scratch.store(false, std::memory_order_release);
    pending_requests = std::move(watcher->pending_requests);
    watcher->pending_requests.clear();
  }

  if (queues_still_signalling != 0u) {
    reshade::log::message(reshade::log::level::warning, "devkit::watch_region(a frame end's lifetime fence signal did not finish before the device was destroyed; that queue is not waited for)");
  }
  bool all_reached = true;
  for (auto& final_signal : final_signals) {
    if (final_signal.needs_signal) {
      const auto value = internal::SignalTimeline(final_signal.timeline.get(), final_signal.queue);
      if (value == 0u) {
        reshade::log::message(reshade::log::level::warning, "devkit::watch_region(lifetime fence signal failed at device destruction)");
      } else {
        final_signal.value = value;
      }
    }
    if (final_signal.value != 0u && !internal::WaitTimeline(final_signal.timeline.get(), final_signal.value)) {
      all_reached = false;
    }
  }
  if (!all_reached) {
    reshade::log::message(reshade::log::level::warning, "devkit::watch_region(lifetime fence wait timed out at device destruction; the scratch textures are destroyed with the device)");
  }

  if (session != nullptr) {
    // The stats fence may still have signals pending (in-flight readbacks); gpu_fence wants its GPU uses retired before
    // it is destroyed.
    std::uint64_t pending_stats_value = 0u;
    for (const auto& frame : session->in_flight) {
      if (frame.fence_value != (std::numeric_limits<std::uint64_t>::max)()) {
        pending_stats_value = std::max(pending_stats_value, frame.fence_value);
      }
    }
    if (session->stats_timeline != nullptr && pending_stats_value != 0u && !internal::WaitTimeline(session->stats_timeline.get(), pending_stats_value)) {
      reshade::log::message(reshade::log::level::warning, "devkit::watch_region(stats fence wait failed at device destruction: timed out, or the OpenGL context is not current)");
    }
    for (const auto& entry : session->scratch_entries) {
      if (entry.texture.handle != 0u) {
        device->destroy_resource(entry.texture);
      }
    }
    internal::DestroySessionObjects(session.get(), device);
    internal::FailRequest(session->request, "The device was destroyed before the watch finished.");
  }
  for (const auto& retired : retired_scratch) {
    device->destroy_resource(retired.texture);
  }
  final_signals.clear();
  queue_fences.clear();
  orphan_timelines.clear();
  for (const auto& request : pending_requests) {
    internal::FailRequest(request, "The device was destroyed before the watch finished.");
  }
}

[[nodiscard]] inline ToolResult HandleWatchRegionTool(const json& arguments, const ToolContext& context) {
  if (!context.resolve_device_index || !context.enqueue_request || !context.cancel_request) {
    throw std::runtime_error("WatchRegionToolContext is not initialized.");
  }

  const auto resource_view_handle = resource_handle::GetRequired(arguments, "resourceViewHandle");
  const auto device_index = context.resolve_device_index(arguments);

  const auto frames = mcp_arguments::GetOptional<std::uint32_t>(arguments, "frames").value_or(1u);
  if (frames == 0u || frames > MAX_FRAMES) {
    throw std::runtime_error(std::format("frames must be between 1 and {}.", MAX_FRAMES));
  }
  const auto scratch_budget_mb = mcp_arguments::GetOptional<std::uint32_t>(arguments, "scratchBudgetMB").value_or(DEFAULT_SCRATCH_BUDGET_MB);
  if (scratch_budget_mb == 0u || scratch_budget_mb > MAX_SCRATCH_BUDGET_MB) {
    throw std::runtime_error(std::format("scratchBudgetMB must be between 1 and {}.", MAX_SCRATCH_BUDGET_MB));
  }

  auto request = std::make_shared<Request>();
  request->resource_view = reshade::api::resource_view{resource_view_handle};
  request->prefer_clone = mcp_arguments::GetOptional<bool>(arguments, "preferClone").value_or(false);
  request->region = resource_analysis::internal::ParseRegion(arguments);
  request->signal = resource_analysis::internal::ParseSignal(mcp_arguments::GetOptional<std::string>(arguments, "signal").value_or("raw"));
  request->frames = frames;
  request->include_unchanged = mcp_arguments::GetOptional<bool>(arguments, "includeUnchanged").value_or(false);
  request->verify = mcp_arguments::GetOptional<bool>(arguments, "verify").value_or(false);
  request->with_snapshot = mcp_arguments::GetOptional<bool>(arguments, "withSnapshot").value_or(false);
  if (request->with_snapshot && frames != 1u) {
    throw std::runtime_error("withSnapshot needs frames: 1 (a snapshot records one frame).");
  }
  request->scratch_budget_mb = scratch_budget_mb;
  const auto* shader_hash_value = mcp_arguments::FindValue(arguments, "shaderHash");
  if (shader_hash_value != nullptr && !shader_hash_value->is_null()) {
    request->shader_hash = shader_hash::GetRequired(arguments, "shaderHash");
  }

  if (!context.enqueue_request(device_index, request)) {
    const auto error = std::format("Failed to queue the region watch for device #{}.", device_index);
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }

  std::unique_lock request_lock(request->mutex);
  if (!request->cv.wait_for(request_lock, std::chrono::seconds(15), [&request] { return request->completed || request->started; })) {
    request->canceled = true;
    request_lock.unlock();
    context.cancel_request(device_index, request);
    return ToolResult{
        .text = "Timed out waiting for the next frame end (reshade_present) to start the region watch (another watch may still be running, or the swap chain has no ReShade effect runtime).",
        .structured_content = json{{"error", "Timed out waiting for the next frame end (reshade_present) to start the region watch (another watch may still be running, or the swap chain has no ReShade effect runtime)."}},
        .is_error = true,
    };
  }

  // A snapshot frame records every command and can take a minute in a busy game.
  const auto snapshot_allowance = (request->with_snapshot ? std::chrono::seconds(120) : std::chrono::seconds(0));
  const auto watch_timeout = std::chrono::seconds(30) + snapshot_allowance
                             + std::chrono::milliseconds(static_cast<std::int64_t>(frames + MAX_CLOSING_FRAMES) * 250);
  if (!request->cv.wait_for(request_lock, watch_timeout, [&request] { return request->completed; })) {
    request->canceled = true;
    request_lock.unlock();
    context.cancel_request(device_index, request);
    const auto error = std::string("Timed out while measuring the region: the swap chain the watch measures on stopped presenting frames.");
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }

  if (!request->result.has_value()) {
    return ToolResult{
        .text = "The region watch completed without a result.",
        .structured_content = json{{"error", "The region watch completed without a result."}},
        .is_error = true,
    };
  }
  auto result = request->result.value();
  auto snapshot_link_error = request->snapshot_link_error;
  const auto linked_snapshot_id = request->linked_snapshot_id;
  request_lock.unlock();
  if (!request->with_snapshot || result.is_error || !result.structured_content.is_object()) return result;

  // Each traced writer's snapshotCommand key becomes its snapshot draw index with the textures the snapshot recorded for
  // that command (as devkit_frame_overview describes them). The snapshot ended at the present after the watched frame,
  // before this watch completed.
  std::optional<frame_overview::Input> snapshot = std::nullopt;
  if (snapshot_link_error.empty() && !linked_snapshot_id.has_value()) {
    snapshot_link_error = "The watch ended before its snapshot was started.";
  }
  if (snapshot_link_error.empty()) {
    snapshot = context.collect_snapshot_commands(device_index);
    if (snapshot->snapshot_id != linked_snapshot_id.value()) {
      snapshot_link_error = "Another snapshot replaced the linked one before the watch completed.";
    } else if (snapshot->snapshot_pending) {
      snapshot_link_error = "The linked snapshot had not finished when the watch completed.";
    }
  }
  std::size_t linked_commands = 0u;
  const auto link = [&](json* entry) {
    const auto key = entry->find("snapshotCommand");
    if (key == entry->end()) return;
    const auto cmd_list_handle = (*key)["commandList"].get<std::uint64_t>();
    const auto cmd_list_recording = (*key)["recording"].get<std::uint64_t>();
    const auto cmd_list_draw_index = (*key)["index"].get<std::uint32_t>();
    entry->erase(key);
    if (!snapshot_link_error.empty()) return;
    const auto command = std::ranges::find_if(snapshot->commands, [&](const frame_overview::Command& candidate) {
      return candidate.cmd_list_handle == cmd_list_handle && candidate.cmd_list_recording == cmd_list_recording
             && candidate.cmd_list_draw_index == cmd_list_draw_index;
    });
    if (command == snapshot->commands.end()) return;
    (*entry)["snapshotDraw"] = command->draw_index;
    (*entry)["targets"] = command->targets;
    (*entry)["inputs"] = command->inputs;
    ++linked_commands;
  };
  auto& content = result.structured_content;
  if (const auto trace = content.find("trace"); trace != content.end() && trace->is_array()) {
    for (auto& entry : *trace) {
      link(&entry);
      if (const auto draws = entry.find("draws"); draws != entry.end() && draws->is_array()) {
        for (auto& member : *draws) {
          link(&member);
        }
      }
    }
  }
  if (snapshot_link_error.empty()) {
    content["snapshotLinkedCommands"] = linked_commands;
    result.text += std::format("\nSnapshot: {} traced command(s) linked to snapshot draws (snapshotDraw, targets, inputs).", linked_commands);
  } else {
    content["snapshotLinkError"] = snapshot_link_error;
    result.text += "\nSnapshot: not linked. " + snapshot_link_error;
  }
  return result;
}

}  // namespace renodx::addons::devkit::mcp::watch_region
