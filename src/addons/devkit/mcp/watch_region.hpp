/*
 * Copyright (C) 2026 Vulta93
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <d3d11.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <include/reshade.hpp>

#include "../../../utils/bitwise.hpp"
#include "../../../utils/data.hpp"
#include "../../../utils/descriptor.hpp"
#include "../../../utils/mcp/arguments.hpp"
#include "../../../utils/mcp/types.hpp"
#include "../../../utils/pipeline_layout.hpp"
#include "../../../utils/render.hpp"
#include "../../../utils/state.hpp"
#include "../../../utils/swapchain.hpp"
#include "../formatting.hpp"
#include "./resource_analysis.hpp"
#include "./resource_handle.hpp"
#include "./shader_hash.hpp"

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
constexpr std::uint32_t MAX_COMPUTE_UAV_SLOTS = 8u;
// D3D12/Vulkan: default and maximum VRAM for the region copies taken inside the game's command lists.
constexpr std::uint32_t DEFAULT_SCRATCH_BUDGET_MB = 512u;
constexpr std::uint32_t MAX_SCRATCH_BUDGET_MB = 8192u;
constexpr std::uint32_t MAX_SCRATCH_ENTRIES = 4096u;
// D3D12/Vulkan: extra frames a watch may run to measure the state after the last writer of its last frame.
constexpr std::uint32_t MAX_CLOSING_FRAMES = 4u;
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
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false;
  bool canceled = false;
  bool completed = false;
  std::optional<ToolResult> result = std::nullopt;
};

struct ToolContext {
  std::function<std::uint32_t(const json& arguments)> resolve_device_index;
  std::function<bool(std::uint32_t device_index, const std::shared_ptr<Request>& request)> enqueue_request;
  std::function<void(std::uint32_t device_index, const std::shared_ptr<Request>& request)> cancel_request;
};

struct DrawMember {
  std::uint32_t command_index = 0u;
  std::uint32_t pixel_shader_hash = 0u;
  std::uint32_t vertex_shader_hash = 0u;
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
};

// One command that writes the watched subresource, or a pure measurement point (no writer), in command order.
struct Record {
  std::optional<WriterInfo> writer = std::nullopt;
  // D3D11: stats slot of the measurement taken before the command. D3D12/Vulkan: scratch entry holding the copy.
  std::optional<std::uint32_t> measurement = std::nullopt;
  // Why no measurement was taken before this writer.
  UnmeasuredReason reason = UnmeasuredReason::NONE;
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
  double max_luminance = 0.0;
  double min_luminance = 0.0;
  double mean_luminance = 0.0;
  double max_channel = 0.0;
};

// A committed measurement, identified by its position in commit (submission) order.
struct Point {
  std::uint32_t frame_index = 0u;
  std::optional<std::uint32_t> slot = std::nullopt;
  std::optional<std::uint32_t> scratch_entry = std::nullopt;
  UnmeasuredReason refused = UnmeasuredReason::NONE;
  std::optional<RegionStats> stats = std::nullopt;
  bool decoded = false;
};

// A writer and the measurements around it: the state before it and before the next writer (or the end of the frame).
struct Event {
  WriterInfo writer;
  std::uint32_t frame_index = 0u;
  std::optional<std::uint64_t> before_point = std::nullopt;
  UnmeasuredReason before_reason = UnmeasuredReason::NONE;
  bool after_resolved = false;
  std::optional<std::uint64_t> after_point = std::nullopt;
  UnmeasuredReason after_reason = UnmeasuredReason::NONE;
};

struct InFlightFrame {
  std::uint32_t ring_index = 0u;
  std::uint32_t frame_index = 0u;
  std::uint64_t first_point = 0u;
  std::uint64_t point_count = 0u;
  // False for the D3D12/Vulkan warm-up and closing frames, which are not part of the requested frames.
  bool counted = true;
  bool truncated = false;
  std::uint64_t fence_value = 0u;
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
  std::uint64_t unmeasured_executions = 0u;
  std::map<UnmeasuredReason, std::uint64_t> unmeasured_reasons = {};
  std::uint32_t frames_seen = 0u;
  std::uint32_t last_frame_index = (std::numeric_limits<std::uint32_t>::max)();
  RunningRange before_peak;
  RunningRange after_peak;
  RunningRange before_mean;
  RunningRange after_mean;
  std::map<std::int64_t, std::uint64_t> after_peak_buckets;
};

// D3D12/Vulkan: a region-sized texture that receives one measurement copy inside a game command list. It belongs to
// that command list until the list is reset or destroyed, because a list may be executed again without being
// re-recorded and would copy into it again.
struct ScratchEntry {
  reshade::api::resource texture = {0u};
  std::uint64_t owner_list = 0u;
  bool in_use = false;
  bool awaiting_reduction = false;
  bool release_after_reduction = false;
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
static_assert(sizeof(VulkanImageMemoryBarrier) == 72u);
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

struct Session {
  std::shared_ptr<Request> request;
  std::string failure;

  reshade::api::resource watched_resource = {0u};
  // With preferClone, copies/clears/resolves/updates may name the original texture while the game's draws write the
  // clone; non-draw writers naming either one are measured.
  reshade::api::resource original_resource = {0u};
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
  bool used_clone = false;
  reshade::api::format scratch_format = reshade::api::format::unknown;
  std::string resource_format_name;

  // D3D12/Vulkan: measurements are copies in the game's command lists, reduced at the end of the frame.
  bool explicit_api = false;
  reshade::api::device_api device_api = reshade::api::device_api::d3d11;
  reshade::api::command_queue* queue = nullptr;
  VulkanCmdPipelineBarrier vulkan_cmd_pipeline_barrier = nullptr;
  std::vector<ScratchEntry> scratch_entries;
  std::uint32_t max_scratch_entries = 0u;
  std::uint32_t scratch_budget_mb = DEFAULT_SCRATCH_BUDGET_MB;
  std::unordered_map<std::uint64_t, ListState> lists;
  reshade::api::fence fence = {0u};
  std::uint64_t fence_value = 0u;
  // Vulkan: the fence is signalled at finish_present, after ReShade submitted the frame's immediate command list with
  // the present's wait semaphores; signalling inside reshade_present would submit it early, without them.
  std::optional<std::uint64_t> unsignaled_fence_value = std::nullopt;

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

  // D3D11: the region copy that is reduced. D3D12/Vulkan: a scratch entry is copied here before it is reduced.
  reshade::api::resource scratch = {0u};
  reshade::api::resource_view scratch_srv = {0u};
  reshade::api::resource stats_texture = {0u};
  reshade::api::resource_view stats_uav = {0u};
  std::array<reshade::api::resource, READBACK_RING_SIZE> readbacks = {};
  renodx::utils::render::RenderPass pass;
  // width, height, signal, slot as raw bits; the compute pass reads this through push constants.
  std::array<float, 4> constants = {};

  // State of the frame being recorded.
  std::uint32_t slots_used = 0u;
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
  std::map<UnmeasuredReason, std::uint64_t> unmeasured_reasons = {};
  std::vector<std::uint32_t> frames_with_unmeasured_writers;
};

// A D3D12/Vulkan scratch texture still referenced by a command list recorded during a finished watch. It is destroyed
// when that list is reset or destroyed (the game guarantees the GPU is done with the list by then) or with the device.
struct RetiredScratch {
  reshade::api::resource texture = {0u};
  std::uint64_t owner_list = 0u;
};

// The watcher is owned by the per-device data. `watched_resource_handle` is a lock-free gate (0 = no watch running), so
// draws pay nothing while idle, and lets a destroyed watched texture stop the watch without taking the lock.
struct Watcher {
  std::shared_mutex mutex;
  std::atomic<std::uint64_t> watched_resource_handle = 0u;
  std::atomic<bool> has_retired_scratch = false;
  std::unique_ptr<Session> session;
  std::deque<std::shared_ptr<Request>> pending_requests;
  std::vector<RetiredScratch> retired_scratch;
};

namespace internal {

// Set while the watch issues its own native Vulkan barrier: ReShade reports it as a barrier event on this thread while
// the watcher lock is held.
static thread_local bool issuing_native_commands = false;

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

[[nodiscard]] inline bool IsImmediateD3D11Context(reshade::api::command_list* cmd_list) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d11) return false;
  auto* native_context = reinterpret_cast<ID3D11DeviceContext*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
  return native_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

[[nodiscard]] inline bool IsExplicitApi(reshade::api::device_api api) {
  return api == reshade::api::device_api::d3d12 || api == reshade::api::device_api::vulkan;
}

// True when the view (render target, UAV, attachment) includes the watched mip level and array layer.
[[nodiscard]] inline bool ViewCoversWatchedSubresource(
    const Session& session,
    reshade::api::device* device,
    reshade::api::resource_view view) {
  if (view.handle == 0u) return false;
  const auto resource = device->get_resource_from_view(view);
  if (resource.handle != session.watched_resource.handle && resource.handle != session.original_resource.handle) return false;
  const auto view_desc = device->get_resource_view_desc(view);
  const auto& texture = view_desc.texture;
  const auto covers_level = session.mip_level >= texture.first_level
                            && (texture.level_count == UINT32_MAX || session.mip_level < texture.first_level + texture.level_count);
  const auto covers_layer = session.array_layer >= texture.first_layer
                            && (texture.layer_count == UINT32_MAX || session.array_layer < texture.first_layer + texture.layer_count);
  return covers_level && covers_layer;
}

[[nodiscard]] inline bool MatchesFilter(const Session& session, const WriterInfo& writer) {
  if (!session.shader_hash.has_value()) return true;
  const auto hash = session.shader_hash.value();
  if (writer.pixel_shader_hash == hash || writer.vertex_shader_hash == hash || writer.compute_shader_hash == hash) return true;
  return std::ranges::any_of(writer.members, [hash](const DrawMember& member) {
    return member.pixel_shader_hash == hash || member.vertex_shader_hash == hash;
  });
}

// D3D12/Vulkan: copies the region, which is in `state` right now, into a free scratch entry owned by `owner_list`
// (0 = owned by the frame end). Returns the entry, or why no copy was taken.
[[nodiscard]] inline std::pair<std::optional<std::uint32_t>, UnmeasuredReason> CopyRegionToScratch(
    Session* session,
    reshade::api::command_list* cmd_list,
    std::uint64_t owner_list,
    reshade::api::resource_usage state) {
  auto* device = cmd_list->get_device();
  std::optional<std::uint32_t> entry_index = std::nullopt;
  for (std::uint32_t index = 0u; index < session->scratch_entries.size(); ++index) {
    if (!session->scratch_entries[index].in_use) {
      entry_index = index;
      break;
    }
  }
  if (!entry_index.has_value()) {
    if (session->scratch_entries.size() >= session->max_scratch_entries) return {std::nullopt, UnmeasuredReason::SCRATCH_BUDGET};
    reshade::api::resource texture = {0u};
    if (!device->create_resource(
            reshade::api::resource_desc(session->width, session->height, 1, 1, session->scratch_format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::copy_dest | reshade::api::resource_usage::copy_source),
            nullptr,
            reshade::api::resource_usage::copy_dest,
            &texture)) {
      return {std::nullopt, UnmeasuredReason::SCRATCH_BUDGET};
    }
    session->scratch_entries.push_back(ScratchEntry{.texture = texture});
    entry_index = static_cast<std::uint32_t>(session->scratch_entries.size() - 1u);
  }

  // Transitions only the watched subresource. ReShade's barrier() transitions every subresource, which would corrupt
  // the state of the other slices or mips of an array texture, so textures with more than one subresource use the
  // native barrier on D3D12 and Vulkan.
  const auto transition_watched_subresource = [&](reshade::api::resource_usage old_state, reshade::api::resource_usage new_state) {
    if (session->single_subresource) {
      cmd_list->barrier(session->watched_resource, old_state, new_state);
      return;
    }
    if (session->device_api == reshade::api::device_api::d3d12) {
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
              .pResource = reinterpret_cast<ID3D12Resource*>(static_cast<std::uintptr_t>(session->watched_resource.handle)),
              .Subresource = session->subresource,
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
        .image = session->watched_resource.handle,
        .subresource_range = {
            .aspect_mask = 1u,  // VK_IMAGE_ASPECT_COLOR_BIT
            .base_mip_level = session->mip_level,
            .level_count = 1u,
            .base_array_layer = session->array_layer,
            .layer_count = 1u,
        },
    };
    issuing_native_commands = true;
    session->vulkan_cmd_pipeline_barrier(
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

  auto& entry = session->scratch_entries[entry_index.value()];
  entry.in_use = true;
  entry.owner_list = owner_list;
  entry.awaiting_reduction = false;
  entry.release_after_reduction = false;

  transition_watched_subresource(state, reshade::api::resource_usage::copy_source);
  cmd_list->copy_texture_region(session->watched_resource, session->subresource, &session->region, entry.texture, 0u, nullptr);
  transition_watched_subresource(reshade::api::resource_usage::copy_source, state);
  return {entry_index, UnmeasuredReason::NONE};
}

[[nodiscard]] inline Point* FindPoint(Session* session, std::uint64_t point_id) {
  if (point_id < session->points_base || point_id - session->points_base >= session->points.size()) return nullptr;
  return &session->points[static_cast<std::size_t>(point_id - session->points_base)];
}

// Commits one record in submission order: a measurement closes the open event (it is the state after that writer) and
// a writer that matches the filter opens a new event. `refused` drops the record's measurement (for example a command
// list executed on another queue); its writer is still accounted for.
inline void Commit(Session* session, const Record& record, UnmeasuredReason refused) {
  std::optional<std::uint64_t> point_id = std::nullopt;
  auto unmeasured_reason = record.reason;
  if (record.measurement.has_value() && refused == UnmeasuredReason::NONE) {
    point_id = session->points_base + session->points.size();
    auto point = Point{.frame_index = session->frames_recorded};
    if (session->explicit_api) {
      point.scratch_entry = record.measurement;
      session->scratch_entries[record.measurement.value()].awaiting_reduction = true;
    } else {
      point.slot = record.measurement;
    }
    session->points.push_back(point);
    if (session->closing && session->verify && !session->verify_point.has_value()) {
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
    open_event.after_resolved = true;
    open_event.after_point = point_id;
    open_event.after_reason = (point_id.has_value() ? UnmeasuredReason::NONE : unmeasured_reason);
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
  const auto before_point = (point_id.has_value() ? point_id : session->last_point);
  session->last_point.reset();
  if (session->warming_up || session->closing || !MatchesFilter(*session, writer)) return;

  session->events.push_back(Event{
      .writer = std::move(writer),
      .frame_index = session->frames_recorded,
      .before_point = before_point,
      .before_reason = (before_point.has_value() ? UnmeasuredReason::NONE : unmeasured_reason),
  });
  session->open_event = session->events_base + session->events.size() - 1u;
}

inline void DestroySessionObjects(Session* session, reshade::api::device* device) {
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
  if (session->fence.handle != 0u) {
    device->destroy_fence(session->fence);
    session->fence = {0u};
  }
}

// D3D11: reduces the watched region into the next stats slot and returns that slot. Must run with the session locked.
[[nodiscard]] inline std::optional<std::uint32_t> MeasureRegion(Session* session, reshade::api::command_list* cmd_list) {
  if (session->slots_used >= MAX_SLOTS) {
    session->truncated = true;
    return std::nullopt;
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

// D3D11 immediate context: recording order is execution order, so the record is committed at once. Writers outside
// the filter are only measured to close an open event.
inline void RecordImmediateWriter(Session* session, reshade::api::command_list* cmd_list, const WriterInfo& writer, bool matches_filter) {
  if (!matches_filter && !session->open_event.has_value()) return;

  auto record = Record{.writer = writer};
  // The last slot stays free for the measurement taken at the end of the frame.
  if (session->slots_used + 1u >= MAX_SLOTS) {
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

// D3D12/Vulkan: records a writer (or, without `writer`, a pure measurement point) in its command list. `state` is the
// exactly known state of the watched subresource at this point, or nullopt with the reason it is not known.
// Writers are always measured when their state is known; outside the shaderHash filter only when the measurement can
// close a matching writer (the first record of a list, or right after a writer of the same list).
inline void RecordExplicit(
    Session* session,
    reshade::api::command_list* cmd_list,
    std::optional<WriterInfo> writer,
    std::optional<reshade::api::resource_usage> state,
    UnmeasuredReason unknown_state_reason) {
  auto* list = GetListState(session, cmd_list);
  if (list->bundle) return;

  auto record = Record{.writer = std::move(writer)};

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

  if (reason == UnmeasuredReason::NONE && wanted) {
    auto [entry, copy_reason] = CopyRegionToScratch(session, cmd_list, reinterpret_cast<std::uint64_t>(cmd_list), state.value());
    record.measurement = entry;
    record.reason = copy_reason;
    if (entry.has_value()) {
      list->scratch_entries.push_back(entry.value());
    }
  } else {
    record.reason = reason;
  }

  if (!record.writer.has_value() && !record.measurement.has_value()) return;
  list->records.push_back(std::move(record));
}

}  // namespace internal

// Draw/dispatch hook. Counts the command and measures when it writes the watched texture through a render target
// (draws) or a compute UAV (dispatches).
inline void OnDrawOrDispatch(Watcher* watcher, reshade::api::command_list* cmd_list, bool is_dispatch) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  const auto device_api = cmd_list->get_device()->get_api();
  const auto explicit_api = internal::IsExplicitApi(device_api);
  if (!explicit_api && !internal::IsImmediateD3D11Context(cmd_list)) return;

  std::unique_lock lock(watcher->mutex);
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
      bool writes_watched = false;
      for (std::uint32_t slot = 0u; slot < MAX_COMPUTE_UAV_SLOTS && !writes_watched; ++slot) {
        const auto view = renodx::utils::state::GetBoundResourceView(
            cmd_list, reshade::api::shader_stage::compute, reshade::api::descriptor_type::unordered_access_view, slot);
        writes_watched = internal::ViewCoversWatchedSubresource(*session, device, view);
      }
      std::optional<renodx::utils::state::BoundDescriptorTables> bound_tables = std::nullopt;
      if (!writes_watched) {
        bound_tables = renodx::utils::state::CopyBoundDescriptorTables(cmd_list, renodx::utils::state::PipelineBindPoint::COMPUTE);
      }
      auto* descriptor_data = renodx::utils::data::Get<renodx::utils::descriptor::DeviceData>(device);
      if (bound_tables.has_value() && bound_tables->layout.handle != 0u && descriptor_data != nullptr) {
        renodx::utils::pipeline_layout::GetPipelineLayoutData(bound_tables->layout, [&](const auto* layout_data) {
          for (const auto& [param_index, table] : bound_tables->tables.entries()) {
            if (writes_watched || table.handle == 0u || param_index >= layout_data->params.size()) continue;
            const auto scan_range = [&](const reshade::api::descriptor_range& range) {
              if (writes_watched) return;
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
              for (std::uint32_t index = 0u; index < scanned && !writes_watched; ++index) {
                const auto& descriptor = heap_slots[base_offset + index];
                if (!descriptor.HasResourceView()) continue;
                writes_watched = internal::ViewCoversWatchedSubresource(*session, device, descriptor.resource_view);
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
      if (!writes_watched) return;
      internal::RecordExplicit(
          session,
          cmd_list,
          WriterInfo{
              .kind = WriterKind::DISPATCH,
              .command_index = command_index,
              .compute_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::compute_shader),
          },
          reshade::api::resource_usage::unordered_access,
          UnmeasuredReason::NONE);
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
        });
      }
      return;
    }

    const auto& render_targets = renodx::utils::swapchain::GetRenderTargets(cmd_list);
    const auto writes_watched = std::ranges::any_of(render_targets, [&](const reshade::api::resource_view& view) {
      return internal::ViewCoversWatchedSubresource(*session, device, view);
    });
    if (!writes_watched) return;
    // D3D12 draws outside a render pass need their render targets in RENDER_TARGET state. Vulkan draws are always
    // inside a render pass; one seen here comes from a list whose render pass state is not tracked.
    std::optional<reshade::api::resource_usage> draw_state = std::nullopt;
    if (session->device_api == reshade::api::device_api::d3d12) {
      draw_state = reshade::api::resource_usage::render_target;
    }
    internal::RecordExplicit(
        session,
        cmd_list,
        WriterInfo{
            .kind = WriterKind::DRAW,
            .command_index = command_index,
            .pixel_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader),
            .vertex_shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader),
        },
        draw_state,
        UnmeasuredReason::STATE_UNKNOWN);
    return;
  }

  const auto command_index = session->command_count++;

  bool writes_watched_resource = false;
  if (is_dispatch) {
    for (std::uint32_t slot = 0u; slot < MAX_COMPUTE_UAV_SLOTS && !writes_watched_resource; ++slot) {
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
  }
  if (!writes_watched_resource) return;

  const WriterInfo writer = {
      .kind = (is_dispatch ? WriterKind::DISPATCH : WriterKind::DRAW),
      .command_index = command_index,
      .pixel_shader_hash = (is_dispatch ? 0u : renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader)),
      .vertex_shader_hash = (is_dispatch ? 0u : renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader)),
      .compute_shader_hash = (is_dispatch ? renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::compute_shader) : 0u),
  };
  internal::RecordImmediateWriter(session, cmd_list, writer, internal::MatchesFilter(*session, writer));
}


// Copy, clear, resolve and update hook. Only copies count as numbered commands because those are the ones the
// snapshot draw list also contains. `cmd_list` is nullptr for device-level events (D3D11 immediate-context updates).
inline void OnResourceWrite(Watcher* watcher, reshade::api::command_list* cmd_list, WriterKind kind, const WrittenTarget& target) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  const auto explicit_api = (cmd_list != nullptr && internal::IsExplicitApi(cmd_list->get_device()->get_api()));
  if (cmd_list != nullptr && !explicit_api && !internal::IsImmediateD3D11Context(cmd_list)) return;

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;
  // Re-checked under the lock: the texture may have been destroyed while this thread waited.
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  // D3D12/Vulkan device-level updates have no place in the command stream (documented limit).
  if (cmd_list == nullptr && session->explicit_api) return;

  if (explicit_api) {
    auto* list = internal::GetListState(session, cmd_list);
    if (list->bundle) return;
    const auto command_index = list->command_count;
    if (kind == WriterKind::COPY) {
      ++list->command_count;
    }
    bool writes_watched = false;
    if (target.view.handle != 0u) {
      writes_watched = internal::ViewCoversWatchedSubresource(*session, cmd_list->get_device(), target.view);
    } else {
      writes_watched = (target.resource.handle == session->watched_resource.handle || target.resource.handle == session->original_resource.handle)
                       && (!target.subresource.has_value() || target.subresource.value() == session->subresource);
    }
    if (!writes_watched) return;

    // Clears give an exact state: on D3D12 the view must be in that state; on Vulkan ReShade transitions the cleared
    // ranges to COLOR_ATTACHMENT_OPTIMAL around the event but does not report the ranges, so only an image with a
    // single subresource is known to be in it. Copy and resolve destinations are ambiguous (D3D12 promotion from
    // COMMON; Vulkan GENERAL or TRANSFER_DST), so the state before them comes from the previous point.
    auto state = target.state;
    if (session->device_api == reshade::api::device_api::vulkan && !session->single_subresource) {
      state.reset();
    }
    internal::RecordExplicit(session, cmd_list, WriterInfo{.kind = kind, .command_index = command_index}, state, UnmeasuredReason::STATE_UNKNOWN);
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

  // A filter by shader hash never matches a non-shader writer; it is still measured when it closes an open event.
  internal::RecordImmediateWriter(session, recording_cmd_list, WriterInfo{.kind = kind, .command_index = command_index}, !session->shader_hash.has_value());
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

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;

  auto* list = internal::GetListState(session, cmd_list);
  if (list->bundle) return;
  list->render_pass_record.reset();
  auto* device = cmd_list->get_device();
  const auto writes_watched = std::ranges::any_of(render_targets, [&](const reshade::api::render_pass_render_target_desc& render_target) {
    return internal::ViewCoversWatchedSubresource(*session, device, render_target.view);
  });
  if (writes_watched) {
    std::optional<reshade::api::resource_usage> state = std::nullopt;
    if (session->device_api == reshade::api::device_api::d3d12 && !list->in_render_pass) {
      state = reshade::api::resource_usage::render_target;
    }
    internal::RecordExplicit(session, cmd_list, WriterInfo{.kind = WriterKind::RENDER_PASS, .command_index = list->command_count}, state, UnmeasuredReason::STATE_UNKNOWN);
    if (!list->records.empty() && list->records.back().writer.has_value()
        && list->records.back().writer->kind == WriterKind::RENDER_PASS) {
      list->render_pass_record = list->records.size() - 1u;
    }
  }
  list->in_render_pass = true;
}

inline void OnEndRenderPass(Watcher* watcher, reshade::api::command_list* cmd_list) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsExplicitApi(cmd_list->get_device()->get_api())) return;

  std::unique_lock lock(watcher->mutex);
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

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty() || !session->single_subresource) return;
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;

  for (std::size_t index = 0u; index < resources.size(); ++index) {
    if (resources[index].handle != session->watched_resource.handle) continue;
    switch (new_states[index]) {
      case reshade::api::resource_usage::render_target:
      case reshade::api::resource_usage::shader_resource:
      case reshade::api::resource_usage::copy_source:
      case reshade::api::resource_usage::copy_dest:
      case reshade::api::resource_usage::present:
        internal::RecordExplicit(session, cmd_list, std::nullopt, new_states[index], UnmeasuredReason::STATE_UNKNOWN);
        break;
      default:
        break;
    }
  }
}

// init_command_list and reset_command_list hook (D3D12 lists start recording when created; Vulkan reports
// vkBeginCommandBuffer as a reset). The GPU is done with the list's previous recording at this point, so scratch
// entries it copied into can be reused or, after a finished watch, destroyed.
inline void OnCommandListReset(Watcher* watcher, reshade::api::command_list* cmd_list, bool destroyed) {
  const auto list_handle = reinterpret_cast<std::uint64_t>(cmd_list);
  if (watcher->has_retired_scratch.load(std::memory_order_acquire)) {
    std::unique_lock lock(watcher->mutex);
    auto* device = cmd_list->get_device();
    std::erase_if(watcher->retired_scratch, [&](const RetiredScratch& retired) {
      if (retired.owner_list != list_handle) return false;
      device->destroy_resource(retired.texture);
      return true;
    });
    watcher->has_retired_scratch.store(!watcher->retired_scratch.empty(), std::memory_order_release);
  }

  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsExplicitApi(cmd_list->get_device()->get_api())) return;

  std::unique_lock lock(watcher->mutex);
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
      entry.in_use = false;
      entry.owner_list = 0u;
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
inline void OnExecuteCommandList(Watcher* watcher, reshade::api::command_queue* queue, reshade::api::command_list* cmd_list) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsExplicitApi(cmd_list->get_device()->get_api())) return;

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;

  const auto pair = session->lists.find(reinterpret_cast<std::uint64_t>(cmd_list));
  if (pair == session->lists.end()) return;
  auto& list = pair->second;

  auto refused = UnmeasuredReason::NONE;
  if (queue != session->queue) {
    refused = UnmeasuredReason::OTHER_QUEUE;
  } else if (list.executed_frame.has_value()) {
    refused = UnmeasuredReason::RESUBMITTED;
    // Executed twice before the end of the frame: the second execution overwrites the copies of the first.
    if (list.executed_frame.value() == session->frames_recorded) {
      for (const auto point_id : list.committed_points) {
        if (auto* point = internal::FindPoint(session, point_id); point != nullptr) {
          point->refused = UnmeasuredReason::RESUBMITTED;
        }
      }
    }
  }

  for (const auto& record : list.records) {
    const auto next_point = session->points_base + session->points.size();
    internal::Commit(session, record, refused);
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

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;

  auto* list = internal::GetListState(session, cmd_list);
  if (session->device_api == reshade::api::device_api::d3d12) {
    auto* device = cmd_list->get_device();
    const auto& render_targets = renodx::utils::swapchain::GetRenderTargets(cmd_list);
    const auto writes_watched = list->render_pass_record.has_value()
                                || std::ranges::any_of(render_targets, [&](const reshade::api::resource_view& view) {
                                     return internal::ViewCoversWatchedSubresource(*session, device, view);
                                   });
    if (writes_watched) {
      internal::RecordExplicit(session, cmd_list, WriterInfo{.kind = WriterKind::BUNDLE, .command_index = list->command_count}, std::nullopt, UnmeasuredReason::BUNDLE);
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

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || session->queue != queue || !session->unsignaled_fence_value.has_value()) return;
  queue->signal(session->fence, session->unsignaled_fence_value.value());
  session->unsignaled_fence_value.reset();
}

// Called from reshade_present, which ReShade raises after every add-on's present handler and after its own effects
// and overlay, so the end-of-frame measurement includes whatever any add-on wrote during present, independent of load
// order. Finishes the frame that just ended (final measurement, D3D12/Vulkan reductions, stats readback), completes
// the watch when enough frames were recorded, and starts the next queued watch.
inline void ProcessAtFrameEnd(
    Watcher* watcher,
    std::uint32_t device_index,
    reshade::api::effect_runtime* runtime,
    std::span<const std::uint8_t> reduce_shader) {
  std::unique_lock lock(watcher->mutex);

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

  if (auto* session = watcher->session.get(); session != nullptr) {
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
      const auto* point = internal::FindPoint(session, point_id.value());
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
          if (before_value.finite_count != 0u) {
            aggregate.before_peak.Add(before_value.max_luminance);
            aggregate.before_mean.Add(before_value.mean_luminance);
          }
          if (after_value.finite_count != 0u) {
            aggregate.after_peak.Add(after_value.max_luminance);
            aggregate.after_mean.Add(after_value.mean_luminance);
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

    // Maps the oldest in-flight readback and stores the stats of its points. D3D12/Vulkan mapping does not wait for
    // the GPU, so the frame's fence must have passed; with `wait` the CPU waits for it.
    const auto drain_oldest = [&](bool wait) -> bool {
      const auto frame = session->in_flight.front();
      const auto has_readback = (frame.fence_value != (std::numeric_limits<std::uint64_t>::max)());
      if (session->explicit_api && has_readback) {
        if (session->fence.handle != 0u) {
          if (device->get_completed_fence_value(session->fence) < frame.fence_value) {
            if (!wait) return false;
            if (session->unsignaled_fence_value.has_value()) {
              queue->signal(session->fence, session->unsignaled_fence_value.value());
              session->unsignaled_fence_value.reset();
            }
            device->wait(session->fence, frame.fence_value);
          }
        } else if (wait) {
          queue->wait_idle();
        } else {
          return false;
        }
      }
      session->in_flight.pop_front();

      reshade::api::subresource_data mapped_data = {};
      if (has_readback
          && !device->map_texture_region(session->readbacks[frame.ring_index], 0u, nullptr, reshade::api::map_access::read_only, &mapped_data)) {
        session->failure = "Failed to map the stats readback texture.";
        return false;
      }
      for (std::uint64_t point_id = frame.first_point; point_id < frame.first_point + frame.point_count; ++point_id) {
        auto* point = internal::FindPoint(session, point_id);
        if (point == nullptr) continue;
        point->decoded = true;
        if (!has_readback || point->refused != UnmeasuredReason::NONE || !point->slot.has_value()) continue;

        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(mapped_data.data) + (static_cast<std::size_t>(mapped_data.row_pitch) * point->slot.value()));
        RegionStats stats = {};
        std::copy_n(row, STATS_STRIDE, stats.raw.begin());
        stats.finite_count = row[STATS_FINITE_COUNT];
        stats.non_finite_count = row[STATS_NON_FINITE_COUNT];
        stats.negative_count = row[STATS_NEGATIVE_COUNT];
        stats.above_one_count = row[STATS_ABOVE_ONE_COUNT];
        stats.black_count = row[STATS_BLACK_COUNT];
        stats.above_nits_counts = {row[STATS_ABOVE_203_COUNT], row[STATS_ABOVE_400_COUNT], row[STATS_ABOVE_1000_COUNT], row[STATS_ABOVE_4000_COUNT]};
        if (stats.finite_count != 0u) {
          stats.max_luminance = std::bit_cast<float>(row[STATS_MAX_LUMINANCE_BITS]);
          stats.min_luminance = std::bit_cast<float>(~row[STATS_MIN_LUMINANCE_INVERTED_BITS]);
          stats.max_channel = std::bit_cast<float>(row[STATS_MAX_CHANNEL_BITS]);
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
      if (has_readback) {
        device->unmap_texture_region(session->readbacks[frame.ring_index], 0u);
      }

      if (frame.counted) {
        ++session->frames_decoded;
        if (frame.truncated) {
          ++session->truncated_frames;
        }
      }
      process_events();
      return true;
    };

    bool request_canceled = false;
    {
      std::scoped_lock request_lock(session->request->mutex);
      request_canceled = session->request->canceled;
    }
    if (session->failure.empty() && watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) {
      session->failure = "The watched texture was destroyed during the watch (for example by a resize).";
    }

    if (!request_canceled && session->failure.empty()) {
      const auto is_last_frame = (!session->warming_up && !session->closing && session->frames_recorded + 1u >= session->frames);
      std::uint32_t reduced_points = 0u;

      if (session->explicit_api) {
        // Final measurement of the frame: only the presented back buffer is in a known state (present) here; any other
        // texture's last writer is closed by the next exact point (usually in the next frame).
        const auto back_buffer = runtime->get_current_back_buffer();
        const auto wants_final_point = session->open_event.has_value() || is_last_frame;
        if (back_buffer.handle == session->watched_resource.handle && wants_final_point) {
          const auto [entry, reason] = internal::CopyRegionToScratch(session, cmd_list, 0u, reshade::api::resource_usage::present);
          internal::Commit(session, Record{.measurement = entry, .reason = reason}, UnmeasuredReason::NONE);
          if (entry.has_value() && is_last_frame) {
            session->end_point = session->points_base + session->points.size() - 1u;
            if (session->verify) {
              session->verify_point = session->end_point;
            }
          }
        }

        // Reductions of every point committed this frame, in commit order; the barriers order them after the game's
        // copies, which ran earlier on this queue.
        const auto frame_end_point = session->points_base + session->points.size();
        for (auto point_id = std::max(session->frame_first_point, session->points_base); point_id < frame_end_point; ++point_id) {
          auto* point = internal::FindPoint(session, point_id);
          if (point == nullptr || !point->scratch_entry.has_value()) continue;
          auto& entry = session->scratch_entries[point->scratch_entry.value()];
          if (point->refused == UnmeasuredReason::NONE && reduced_points >= MAX_SLOTS) {
            point->refused = UnmeasuredReason::SLOT_LIMIT;
            session->truncated = true;
          }
          if (point->refused == UnmeasuredReason::NONE && session->failure.empty()) {
            if (reduced_points == 0u) {
              const std::array<std::uint32_t, 4> zeros = {};
              cmd_list->clear_unordered_access_view_uint(session->stats_uav, zeros.data());
              cmd_list->barrier(session->stats_texture, reshade::api::resource_usage::unordered_access, reshade::api::resource_usage::unordered_access);
            }
            const auto slot = reduced_points++;
            cmd_list->barrier(entry.texture, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::copy_source);
            cmd_list->copy_texture_region(entry.texture, 0u, nullptr, session->scratch, 0u, nullptr);
            cmd_list->barrier(session->scratch, reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::shader_resource);
            session->constants = {
                std::bit_cast<float>(session->width),
                std::bit_cast<float>(session->height),
                std::bit_cast<float>(static_cast<std::uint32_t>(session->signal)),
                std::bit_cast<float>(slot),
            };
            if (!session->pass.Render(cmd_list)) {
              session->failure = "The region reduction pass failed to render.";
            }
            cmd_list->barrier(session->scratch, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::copy_dest);
            if (session->verify_point.has_value() && session->verify_point.value() == point_id && !session->verify_copied
                && device->create_resource(
                    reshade::api::resource_desc(session->width, session->height, 1, 1, session->scratch_format, 1, reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
                    nullptr,
                    reshade::api::resource_usage::copy_dest,
                    &session->verify_readback)) {
              cmd_list->copy_texture_region(entry.texture, 0u, nullptr, session->verify_readback, 0u, nullptr);
              session->verify_copied = true;
            }
            cmd_list->barrier(entry.texture, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::copy_dest);
            point->slot = slot;
          }
          entry.awaiting_reduction = false;
          if (entry.release_after_reduction || entry.owner_list == 0u) {
            entry.in_use = false;
            entry.owner_list = 0u;
            entry.release_after_reduction = false;
          }
        }
      } else if (session->open_event.has_value() || is_last_frame) {
        // D3D11: the region is measured in place on the immediate context.
        const auto final_slot = internal::MeasureRegion(session, cmd_list);
        if (session->failure.empty()) {
          internal::Commit(
              session,
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
          session->verify_point = session->points_base + session->points.size() - 1u;
          session->verify_copied = true;
        }
      }

      const auto has_measurements = (session->explicit_api ? reduced_points != 0u : session->slots_used != 0u);
      const auto frame_first_point = std::max(session->frame_first_point, session->points_base);
      const auto frame_point_count = session->points_base + session->points.size() - frame_first_point;
      if (session->failure.empty() && (has_measurements || frame_point_count != 0u)) {
        auto frame = InFlightFrame{
            .frame_index = session->frames_recorded,
            .first_point = frame_first_point,
            .point_count = frame_point_count,
            .counted = !session->warming_up && !session->closing,
            .truncated = session->truncated,
            .fence_value = (std::numeric_limits<std::uint64_t>::max)(),
        };
        if (has_measurements) {
          // Keep at most two frames in flight so the newest copy never overwrites an unread readback.
          while (session->in_flight.size() >= 2u && session->failure.empty()) {
            drain_oldest(true);
          }
          frame.ring_index = static_cast<std::uint32_t>(session->readback_pushes++ % READBACK_RING_SIZE);
          if (session->explicit_api) {
            cmd_list->barrier(session->stats_texture, reshade::api::resource_usage::unordered_access, reshade::api::resource_usage::copy_source);
            cmd_list->copy_texture_region(session->stats_texture, 0u, nullptr, session->readbacks[frame.ring_index], 0u, nullptr);
            cmd_list->barrier(session->stats_texture, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::unordered_access);
            frame.fence_value = ++session->fence_value;
            if (session->fence.handle != 0u && session->device_api == reshade::api::device_api::d3d12) {
              queue->signal(session->fence, frame.fence_value);
            } else if (session->fence.handle != 0u) {
              // A value left unsignalled because finish_present did not follow (failed present) is signalled now.
              if (session->unsignaled_fence_value.has_value()) {
                queue->signal(session->fence, session->unsignaled_fence_value.value());
              }
              session->unsignaled_fence_value = frame.fence_value;
            }
          } else {
            cmd_list->copy_texture_region(session->stats_texture, 0u, nullptr, session->readbacks[frame.ring_index], 0u, nullptr);
            frame.fence_value = 0u;
          }
        }
        session->in_flight.push_back(frame);
      }
      // D3D12/Vulkan: decode whatever the GPU has already finished.
      while (session->explicit_api && !session->in_flight.empty() && session->failure.empty()) {
        if (!drain_oldest(false)) break;
      }

      session->slots_used = 0u;
      session->command_count = 0u;
      session->stats_cleared = false;
      session->truncated = false;
      session->frame_first_point = session->points_base + session->points.size();
      session->frame_command_base = 0u;
      if (session->warming_up) {
        session->warming_up = false;
      } else if (session->closing) {
        ++session->closing_frames;
      } else {
        ++session->frames_recorded;
        session->closing = session->explicit_api && session->frames_recorded >= session->frames;
      }
    }

    const auto closed = !session->open_event.has_value() && (!session->verify || session->verify_point.has_value());
    const auto finished = request_canceled || !session->failure.empty()
                          || (session->frames_recorded >= session->frames && (!session->explicit_api || closed || session->closing_frames >= MAX_CLOSING_FRAMES));
    if (finished) {
      if (session->unsignaled_fence_value.has_value()) {
        queue->signal(session->fence, session->unsignaled_fence_value.value());
        session->unsignaled_fence_value.reset();
      }
      queue->wait_idle();
      while (!session->in_flight.empty() && session->failure.empty()) {
        drain_oldest(true);
      }
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
          if (session->truncated_frames != 0u) {
            summary += std::format(" {} frame(s) hit the {}-slot or scratch budget limit.", session->truncated_frames, MAX_SLOTS);
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
                {"unmeasuredExecutions", aggregate.unmeasured_executions},
                {"beforePeak", range_to_json(aggregate.before_peak)},
                {"afterPeak", range_to_json(aggregate.after_peak)},
                {"beforeMean", range_to_json(aggregate.before_mean)},
                {"afterMean", range_to_json(aggregate.after_mean)},
            };
            if (aggregate.unmeasured_executions != 0u) {
              writer_json["unmeasuredReasons"] = reasons_to_json(aggregate.unmeasured_reasons);
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
            result_json["closingFrames"] = session->closing_frames;
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
              result_json["verify"] = json{{"status", "skipped"}, {"reason", "No measurement point was available at the end of the watch (D3D12/Vulkan: the texture is not the presented back buffer and no exact measurement point followed the last frame within the closing frames), or the CPU readback failed."}};
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

      watcher->watched_resource_handle.store(0u, std::memory_order_release);
      // D3D12/Vulkan scratch entries still owned by a command list may be copied into again if the game executes that
      // list later, so they are kept until the list is reset or destroyed. The GPU is idle here.
      for (auto& entry : session->scratch_entries) {
        if (entry.texture.handle == 0u) continue;
        if (entry.in_use && entry.owner_list != 0u) {
          watcher->retired_scratch.push_back(RetiredScratch{.texture = entry.texture, .owner_list = entry.owner_list});
        } else {
          device->destroy_resource(entry.texture);
        }
        entry.texture = {0u};
      }
      watcher->has_retired_scratch.store(!watcher->retired_scratch.empty(), std::memory_order_release);
      internal::DestroySessionObjects(session, device);
      watcher->session.reset();
    }
  }

  while (watcher->session == nullptr && !watcher->pending_requests.empty()) {
    const auto request = watcher->pending_requests.front();
    watcher->pending_requests.pop_front();
    {
      std::scoped_lock request_lock(request->mutex);
      if (request->completed || request->canceled) continue;
      request->started = true;
    }
    request->cv.notify_all();

    const auto device_api = device->get_api();
    if (device_api != reshade::api::device_api::d3d11 && !internal::IsExplicitApi(device_api)) {
      complete_request(request, make_error("devkit_watch_region supports D3D11, D3D12 and Vulkan devices only."));
      continue;
    }

    resource_analysis::internal::AnalysisTarget target = {};
    try {
      target = resource_analysis::internal::ResolveAnalysisTarget(request->resource_view, device, request->prefer_clone);
    } catch (const std::exception& exception) {
      complete_request(request, make_error(exception.what()));
      continue;
    }

    const auto& resource_desc = target.resolved_resource_desc;
    if (resource_desc.type != reshade::api::resource_type::texture_2d && resource_desc.type != reshade::api::resource_type::surface) {
      complete_request(request, make_error("Only 2D texture resource views can be watched."));
      continue;
    }
    if (resource_desc.texture.samples != 1u) {
      complete_request(request, make_error("Multisampled resources cannot be watched."));
      continue;
    }

    const auto scratch_format = reshade::api::format_to_default_typed(resource_desc.texture.format, 0);
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
      default:
        complete_request(request, make_error(std::format("Watching format {} is not supported.", formatting::StreamToString(scratch_format))));
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
        complete_request(request, make_error("The image was created without VK_IMAGE_USAGE_TRANSFER_SRC_BIT, so its region cannot be copied."));
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

    auto session = std::make_unique<Session>();
    session->request = request;
    session->watched_resource = target.resolved_resource;
    session->original_resource = device->get_resource_from_view(request->resource_view);
    session->immediate_cmd_list = queue->get_immediate_command_list();
    session->subresource = first_level + (first_layer * levels);
    session->mip_level = first_level;
    session->array_layer = first_layer;
    session->single_subresource = (levels * layers == 1u);
    session->region = region;
    session->width = region.width();
    session->height = region.height();
    session->signal = request->signal;
    session->frames = request->frames;
    session->shader_hash = request->shader_hash;
    session->include_unchanged = request->include_unchanged;
    session->verify = request->verify;
    session->scratch_format = scratch_format;
    session->used_clone = target.used_clone;
    session->resource_format_name = formatting::StreamToString(scratch_format);
    session->device_api = device_api;
    session->explicit_api = internal::IsExplicitApi(device_api);
    session->warming_up = session->explicit_api;
    session->queue = queue;
    session->vulkan_cmd_pipeline_barrier = vulkan_cmd_pipeline_barrier;
    session->scratch_budget_mb = request->scratch_budget_mb;
    const auto scratch_entry_bytes = std::max<std::uint64_t>(
        static_cast<std::uint64_t>(reshade::api::format_row_pitch(scratch_format, session->width)) * session->height, 1u);
    session->max_scratch_entries = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
        (static_cast<std::uint64_t>(session->scratch_budget_mb) * 1024u * 1024u) / scratch_entry_bytes, 1u, MAX_SCRATCH_ENTRIES));

    const auto created = device->create_resource(
                             reshade::api::resource_desc(session->width, session->height, 1, 1, scratch_format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_dest),
                             nullptr,
                             reshade::api::resource_usage::copy_dest,
                             &session->scratch)
                         && device->create_resource_view(session->scratch, reshade::api::resource_usage::shader_resource, reshade::api::resource_view_desc(scratch_format), &session->scratch_srv)
                         && device->create_resource(
                             reshade::api::resource_desc(STATS_STRIDE, MAX_SLOTS, 1, 1, reshade::api::format::r32_uint, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::unordered_access | reshade::api::resource_usage::copy_source | reshade::api::resource_usage::copy_dest),
                             nullptr,
                             reshade::api::resource_usage::unordered_access,
                             &session->stats_texture)
                         && device->create_resource_view(session->stats_texture, reshade::api::resource_usage::unordered_access, reshade::api::resource_view_desc(reshade::api::format::r32_uint), &session->stats_uav)
                         && std::ranges::all_of(session->readbacks, [&](auto& readback) {
                              return device->create_resource(
                                  reshade::api::resource_desc(STATS_STRIDE, MAX_SLOTS, 1, 1, reshade::api::format::r32_uint, 1, reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
                                  nullptr,
                                  reshade::api::resource_usage::copy_dest,
                                  &readback);
                            });
    if (!created) {
      internal::DestroySessionObjects(session.get(), device);
      complete_request(request, make_error("Failed to create the GPU resources for the region reduction."));
      continue;
    }
    // Without a fence (Vulkan without timeline semaphores), a readback is only read after waiting for the queue.
    if (session->explicit_api && !device->create_fence(0u, reshade::api::fence_flags::none, &session->fence)) {
      session->fence = {0u};
    }

    session->pass.pipeline_subobjects.compute_shader = reduce_shader;
    session->pass.shader_resource_slots.views = {session->scratch_srv};
    session->pass.unordered_access_slots.views = {session->stats_uav};
    session->pass.push_constants[renodx::utils::render::ConstantBuffersSlots{.slot = 0, .space = 0}] =
        std::span<const float>(session->constants.data(), session->constants.size());
    // D3D11 saves and restores the compute slots itself; D3D12/Vulkan render on ReShade's own immediate command list.
    session->pass.revert_state_after_render = false;
    session->pass.dispatch_group_counts = std::make_tuple(
        (session->width + GROUP_SIZE - 1u) / GROUP_SIZE,
        (session->height + GROUP_SIZE - 1u) / GROUP_SIZE,
        1u);

    reshade::log::message(
        reshade::log::level::info,
        std::format(
            "devkit::watch_region(device #{}, resource {}, {}x{} at ({}, {}), {} frame(s))",
            device_index,
            formatting::FormatHandle(session->watched_resource.handle),
            session->width,
            session->height,
            region.left,
            region.top,
            session->frames)
            .c_str());

    const auto watched_resource_handle = session->watched_resource.handle;
    watcher->session = std::move(session);
    watcher->watched_resource_handle.store(watched_resource_handle, std::memory_order_release);
  }
}

// Fails whatever is still queued or running when the device goes away, and destroys every scratch texture (no command
// list of the device can run any more).
inline void Shutdown(Watcher* watcher, reshade::api::device* device) {
  std::unique_lock lock(watcher->mutex);
  watcher->watched_resource_handle.store(0u, std::memory_order_release);

  const auto fail_request = [](const std::shared_ptr<Request>& request) {
    {
      std::scoped_lock request_lock(request->mutex);
      request->result = ToolResult{
          .text = "The device was destroyed before the watch finished.",
          .structured_content = json{{"error", "The device was destroyed before the watch finished."}},
          .is_error = true,
      };
      request->completed = true;
    }
    request->cv.notify_all();
  };
  if (watcher->session != nullptr) {
    for (const auto& entry : watcher->session->scratch_entries) {
      if (entry.texture.handle != 0u) {
        device->destroy_resource(entry.texture);
      }
    }
    internal::DestroySessionObjects(watcher->session.get(), device);
    fail_request(watcher->session->request);
    watcher->session.reset();
  }
  for (const auto& retired : watcher->retired_scratch) {
    device->destroy_resource(retired.texture);
  }
  watcher->retired_scratch.clear();
  watcher->has_retired_scratch.store(false, std::memory_order_release);
  for (const auto& request : watcher->pending_requests) {
    fail_request(request);
  }
  watcher->pending_requests.clear();
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

  const auto watch_timeout = std::chrono::seconds(30) + std::chrono::milliseconds(static_cast<std::int64_t>(frames + MAX_CLOSING_FRAMES) * 250);
  if (!request->cv.wait_for(request_lock, watch_timeout, [&request] { return request->completed; })) {
    request->canceled = true;
    request_lock.unlock();
    context.cancel_request(device_index, request);
    return ToolResult{
        .text = "Timed out while measuring the region; the game may not be presenting frames.",
        .structured_content = json{{"error", "Timed out while measuring the region; the game may not be presenting frames."}},
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
  return request->result.value();
}

}  // namespace renodx::addons::devkit::mcp::watch_region
