/*
 * Copyright (C) 2026 Vulta93
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d11.h>

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
#include <utility>
#include <vector>

#include <include/reshade.hpp>

#include "../../../utils/mcp/arguments.hpp"
#include "../../../utils/mcp/types.hpp"
#include "../../../utils/render.hpp"
#include "../../../utils/state.hpp"
#include "../formatting.hpp"
#include "./resource_analysis.hpp"
#include "./resource_handle.hpp"
#include "./shader_hash.hpp"

// devkit_watch_region: GPU region statistics around every command that writes one texture (DX11).
//
// The state of the watched texture after command N equals its state before the next command that writes it, so the
// tool only measures *before* each writer and once more at present. Each measurement copies the region into a scratch
// texture (a texture bound as render target cannot be bound as SRV) and reduces it with a compute shader into one
// row of an R32_UINT stats texture. The stats texture is copied to a small readback ring at present and decoded
// two frames later, so a long watch does not stall the GPU every frame.
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
constexpr double LUMINANCE_SUM_SCALE = 256.0;

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
};

struct Request {
  reshade::api::resource_view resource_view = {0u};
  bool prefer_clone = false;
  std::optional<reshade::api::subresource_box> region = std::nullopt;
  Signal signal = Signal::RAW;
  std::uint32_t frames = 1u;
  std::optional<std::uint32_t> shader_hash = std::nullopt;
  bool include_unchanged = false;
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

// One measured writer: the region state before it (before_slot) and before the next writer or present (after_slot).
struct Event {
  WriterKind kind = WriterKind::DRAW;
  std::uint32_t command_index = 0u;
  std::uint32_t pixel_shader_hash = 0u;
  std::uint32_t vertex_shader_hash = 0u;
  std::uint32_t compute_shader_hash = 0u;
  std::uint32_t before_slot = 0u;
  std::optional<std::uint32_t> after_slot = std::nullopt;
};

struct InFlightFrame {
  std::uint32_t ring_index = 0u;
  std::uint32_t frame_index = 0u;
  std::uint32_t slots_used = 0u;
  bool truncated = false;
  std::vector<Event> events;
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
  std::uint32_t frames_seen = 0u;
  std::uint32_t last_frame_index = (std::numeric_limits<std::uint32_t>::max)();
  RunningRange before_peak;
  RunningRange after_peak;
  RunningRange before_mean;
  RunningRange after_mean;
  std::map<std::int64_t, std::uint64_t> after_peak_buckets;
};

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
  reshade::api::subresource_box region = {};
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  Signal signal = Signal::RAW;
  std::uint32_t frames = 1u;
  std::optional<std::uint32_t> shader_hash = std::nullopt;
  bool include_unchanged = false;
  bool used_clone = false;
  std::string resource_format_name;

  reshade::api::resource scratch = {0u};
  reshade::api::resource_view scratch_srv = {0u};
  reshade::api::resource stats_texture = {0u};
  reshade::api::resource_view stats_uav = {0u};
  std::array<reshade::api::resource, READBACK_RING_SIZE> readbacks = {};
  renodx::utils::render::RenderPass pass;
  // width, height, signal, slot as raw bits; the compute pass reads this through push constants.
  std::array<float, 4> constants = {};

  // State of the frame being recorded.
  std::vector<Event> events;
  std::optional<std::size_t> open_event_index = std::nullopt;
  std::uint32_t slots_used = 0u;
  std::uint32_t command_count = 0u;
  bool stats_cleared = false;
  bool truncated = false;

  std::uint32_t frames_recorded = 0u;
  std::uint32_t frames_decoded = 0u;
  std::uint32_t truncated_frames = 0u;
  std::uint64_t readback_pushes = 0u;
  std::deque<InFlightFrame> in_flight;

  std::map<std::string, WriterAggregate> aggregates;
  json trace = json::array();
  std::uint64_t events_decoded = 0u;
  std::uint64_t events_changed = 0u;
};

// The watcher is owned by the per-device data. `watched_resource_handle` is a lock-free gate (0 = no watch running), so
// draws pay nothing while idle, and lets a destroyed watched texture stop the watch without taking the lock.
struct Watcher {
  std::shared_mutex mutex;
  std::atomic<std::uint64_t> watched_resource_handle = 0u;
  std::unique_ptr<Session> session;
  std::deque<std::shared_ptr<Request>> pending_requests;
};

namespace internal {

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
  }
  return "unknown";
}

[[nodiscard]] inline bool IsImmediateD3D11Context(reshade::api::command_list* cmd_list) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d11) return false;
  auto* native_context = reinterpret_cast<ID3D11DeviceContext*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
  return native_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
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
}

// Reduces the watched region into the next stats slot and returns that slot. Must run with the session locked.
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

// Closes the open event with a measurement taken now (it is the state after that writer) and, when the new writer
// matches the filter, opens an event for it. Writers outside the filter are only measured to close an open event.
inline void RecordWriter(Session* session, reshade::api::command_list* cmd_list, const Event& event, bool matches_filter) {
  if (!matches_filter && !session->open_event_index.has_value()) return;

  // The last slot stays free for the measurement taken at present.
  if (session->slots_used + 1u >= MAX_SLOTS) {
    session->truncated = true;
    return;
  }

  const auto slot = MeasureRegion(session, cmd_list);
  if (!slot.has_value()) return;

  if (session->open_event_index.has_value()) {
    session->events[session->open_event_index.value()].after_slot = slot;
    session->open_event_index.reset();
  }
  if (matches_filter) {
    auto opened_event = event;
    opened_event.before_slot = slot.value();
    session->events.push_back(opened_event);
    session->open_event_index = session->events.size() - 1u;
  }
}

}  // namespace internal

// Draw/dispatch hook. Counts the command and measures when it writes the watched texture through a render target
// (draws) or a compute UAV (dispatches).
inline void OnDrawOrDispatch(Watcher* watcher, reshade::api::command_list* cmd_list, bool is_dispatch) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (!internal::IsImmediateD3D11Context(cmd_list)) return;

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;

  const auto command_index = session->command_count++;
  auto* device = cmd_list->get_device();

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

  const Event event = {
      .kind = (is_dispatch ? WriterKind::DISPATCH : WriterKind::DRAW),
      .command_index = command_index,
      .pixel_shader_hash = (is_dispatch ? 0u : renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader)),
      .vertex_shader_hash = (is_dispatch ? 0u : renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader)),
      .compute_shader_hash = (is_dispatch ? renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::compute_shader) : 0u),
  };
  const auto matches_filter = !session->shader_hash.has_value()
                              || session->shader_hash.value() == event.pixel_shader_hash
                              || session->shader_hash.value() == event.vertex_shader_hash
                              || session->shader_hash.value() == event.compute_shader_hash;
  internal::RecordWriter(session, cmd_list, event, matches_filter);
}

// Copy, clear, resolve and update hook. Only copies count as numbered commands because those are the ones the
// snapshot draw list also contains. `cmd_list` is nullptr for device-level events (immediate-context updates).
inline void OnResourceWrite(
    Watcher* watcher,
    reshade::api::command_list* cmd_list,
    WriterKind kind,
    reshade::api::resource written_resource) {
  if (watcher->watched_resource_handle.load(std::memory_order_acquire) == 0u) return;
  if (cmd_list != nullptr && !internal::IsImmediateD3D11Context(cmd_list)) return;

  std::unique_lock lock(watcher->mutex);
  auto* session = watcher->session.get();
  if (session == nullptr || !session->failure.empty()) return;

  const auto command_index = session->command_count;
  if (kind == WriterKind::COPY) {
    ++session->command_count;
  }
  if (written_resource.handle != session->watched_resource.handle
      && written_resource.handle != session->original_resource.handle) return;

  auto* recording_cmd_list = (cmd_list != nullptr ? cmd_list : session->immediate_cmd_list);
  // A filter by shader hash never matches a non-shader writer; it is still measured when it closes an open event.
  const Event event = {.kind = kind, .command_index = command_index};
  internal::RecordWriter(session, recording_cmd_list, event, !session->shader_hash.has_value());
}

// destroy_resource hook. Lock-free on purpose: ReShade can fire it while this module already holds the watcher lock
// (for example when a released reference was the last one). Clearing the gate stops all measuring at once; the next
// present reports the watch as failed.
inline void OnResourceDestroyed(Watcher* watcher, reshade::api::resource resource) {
  auto expected_handle = resource.handle;
  if (expected_handle == 0u) return;
  watcher->watched_resource_handle.compare_exchange_strong(expected_handle, 0u, std::memory_order_acq_rel);
}

// Called on every present of the device: finishes the frame that just ended, completes the watch when enough
// frames were recorded, and starts the next queued watch.
inline void ProcessAtPresent(
    Watcher* watcher,
    std::uint32_t device_index,
    reshade::api::device* device,
    reshade::api::command_queue* queue,
    std::span<const std::uint8_t> reduce_shader) {
  std::unique_lock lock(watcher->mutex);

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

    // Maps the oldest in-flight readback and folds its events into the aggregates (and the trace for one frame).
    const auto drain_oldest = [&]() {
      const auto frame = std::move(session->in_flight.front());
      session->in_flight.pop_front();

      reshade::api::subresource_data mapped_data = {};
      if (!device->map_texture_region(session->readbacks[frame.ring_index], 0u, nullptr, reshade::api::map_access::read_only, &mapped_data)) {
        session->failure = "Failed to map the stats readback texture.";
        return;
      }

      std::vector<RegionStats> slot_stats(frame.slots_used);
      for (std::uint32_t slot = 0u; slot < frame.slots_used; ++slot) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(mapped_data.data) + (static_cast<std::size_t>(mapped_data.row_pitch) * slot));
        auto& stats = slot_stats[slot];
        std::copy_n(row, STATS_STRIDE, stats.raw.begin());
        stats.finite_count = row[STATS_FINITE_COUNT];
        stats.non_finite_count = row[STATS_NON_FINITE_COUNT];
        stats.negative_count = row[STATS_NEGATIVE_COUNT];
        stats.above_one_count = row[STATS_ABOVE_ONE_COUNT];
        stats.black_count = row[STATS_BLACK_COUNT];
        stats.above_nits_counts = {row[STATS_ABOVE_203_COUNT], row[STATS_ABOVE_400_COUNT], row[STATS_ABOVE_1000_COUNT], row[STATS_ABOVE_4000_COUNT]};
        if (stats.finite_count == 0u) continue;

        stats.max_luminance = std::bit_cast<float>(row[STATS_MAX_LUMINANCE_BITS]);
        stats.min_luminance = std::bit_cast<float>(~row[STATS_MIN_LUMINANCE_INVERTED_BITS]);
        stats.max_channel = std::bit_cast<float>(row[STATS_MAX_CHANNEL_BITS]);
        const auto luminance_sum = (static_cast<std::uint64_t>(row[STATS_LUMINANCE_SUM_HIGH]) << 32u) | row[STATS_LUMINANCE_SUM_LOW];
        stats.mean_luminance = static_cast<double>(luminance_sum) / (LUMINANCE_SUM_SCALE * static_cast<double>(stats.finite_count));
      }
      device->unmap_texture_region(session->readbacks[frame.ring_index], 0u);

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

      ++session->frames_decoded;
      if (frame.truncated) {
        ++session->truncated_frames;
      }

      for (const auto& event : frame.events) {
        if (!event.after_slot.has_value()) continue;
        const auto& before = slot_stats[event.before_slot];
        const auto& after = slot_stats[event.after_slot.value()];
        const auto changed = before.raw != after.raw;
        ++session->events_decoded;
        if (changed) {
          ++session->events_changed;
        }

        const auto key = std::format(
            "{} ps={} vs={} cs={}",
            internal::GetWriterKindName(event.kind),
            formatting::FormatShaderHash(event.pixel_shader_hash),
            formatting::FormatShaderHash(event.vertex_shader_hash),
            formatting::FormatShaderHash(event.compute_shader_hash));
        auto& aggregate = session->aggregates.try_emplace(key, WriterAggregate{
                                                                   .kind = event.kind,
                                                                   .pixel_shader_hash = event.pixel_shader_hash,
                                                                   .vertex_shader_hash = event.vertex_shader_hash,
                                                                   .compute_shader_hash = event.compute_shader_hash,
                                                               }).first->second;
        ++aggregate.executions;
        if (aggregate.last_frame_index != frame.frame_index) {
          aggregate.last_frame_index = frame.frame_index;
          ++aggregate.frames_seen;
        }
        if (changed) {
          ++aggregate.changed_executions;
        }
        if (after.max_luminance < before.max_luminance * (1.0 - PEAK_TOLERANCE)) {
          ++aggregate.peak_lowered_executions;
        }
        if (after.max_luminance > before.max_luminance * (1.0 + PEAK_TOLERANCE)) {
          ++aggregate.peak_raised_executions;
        }
        if (before.above_one_count != 0u && after.above_one_count == 0u) {
          ++aggregate.dropped_above_one_executions;
        }
        if (before.finite_count != 0u) {
          aggregate.before_peak.Add(before.max_luminance);
          aggregate.before_mean.Add(before.mean_luminance);
        }
        if (after.finite_count != 0u) {
          aggregate.after_peak.Add(after.max_luminance);
          aggregate.after_mean.Add(after.mean_luminance);
          const auto bucket = (after.max_luminance > 0.0)
                                  ? static_cast<std::int64_t>(std::llround(std::log10(after.max_luminance) * CEILING_BUCKETS_PER_DECADE))
                                  : (std::numeric_limits<std::int64_t>::min)();
          ++aggregate.after_peak_buckets[bucket];
        }

        if (session->frames == 1u && (changed || session->include_unchanged)) {
          auto entry = json{
              {"commandIndex", event.command_index},
              {"writer", std::string(internal::GetWriterKindName(event.kind))},
              {"changed", changed},
              {"before", stats_to_json(before)},
              {"after", stats_to_json(after)},
          };
          if (event.pixel_shader_hash != 0u) {
            entry["pixelShaderHash"] = formatting::FormatShaderHash(event.pixel_shader_hash);
          }
          if (event.vertex_shader_hash != 0u) {
            entry["vertexShaderHash"] = formatting::FormatShaderHash(event.vertex_shader_hash);
          }
          if (event.compute_shader_hash != 0u) {
            entry["computeShaderHash"] = formatting::FormatShaderHash(event.compute_shader_hash);
          }
          if (before.finite_count != 0u && after.finite_count != 0u) {
            entry["peakDelta"] = after.max_luminance - before.max_luminance;
          }
          session->trace.push_back(std::move(entry));
        }
      }
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
      if (session->open_event_index.has_value()) {
        const auto final_slot = internal::MeasureRegion(session, cmd_list);
        session->events[session->open_event_index.value()].after_slot = final_slot;
        session->open_event_index.reset();
      }

      if (!session->events.empty() && session->failure.empty()) {
        // Keep at most two frames in flight so the newest copy never overwrites an unread readback.
        while (session->in_flight.size() >= 2u && session->failure.empty()) {
          drain_oldest();
        }
        const auto ring_index = static_cast<std::uint32_t>(session->readback_pushes++ % READBACK_RING_SIZE);
        cmd_list->copy_texture_region(session->stats_texture, 0u, nullptr, session->readbacks[ring_index], 0u, nullptr);
        session->in_flight.push_back(InFlightFrame{
            .ring_index = ring_index,
            .frame_index = session->frames_recorded,
            .slots_used = session->slots_used,
            .truncated = session->truncated,
            .events = std::move(session->events),
        });
      }

      session->events.clear();
      session->slots_used = 0u;
      session->command_count = 0u;
      session->stats_cleared = false;
      session->truncated = false;
      ++session->frames_recorded;
    }

    const auto finished = request_canceled || !session->failure.empty() || session->frames_recorded >= session->frames;
    if (finished) {
      queue->wait_idle();
      while (!session->in_flight.empty() && session->failure.empty()) {
        drain_oldest();
      }

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
              session->events_decoded,
              session->events_changed,
              unit);
          if (session->truncated_frames != 0u) {
            summary += std::format(" {} frame(s) hit the {}-slot limit; later writers in those frames were not measured.", session->truncated_frames, MAX_SLOTS);
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
                {"beforePeak", range_to_json(aggregate.before_peak)},
                {"afterPeak", range_to_json(aggregate.after_peak)},
                {"beforeMean", range_to_json(aggregate.before_mean)},
                {"afterMean", range_to_json(aggregate.after_mean)},
            };
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
              {"writers", std::move(writers)},
              {"summary", summary},
          };
          if (session->frames == 1u) {
            result_json["trace"] = std::move(session->trace);
          }
          complete_request(session->request, ToolResult{.text = summary, .structured_content = std::move(result_json), .is_error = false});
        }
      }

      watcher->watched_resource_handle.store(0u, std::memory_order_release);
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

    if (device->get_api() != reshade::api::device_api::d3d11) {
      complete_request(request, make_error("devkit_watch_region currently supports D3D11 devices only."));
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

    const auto first_level = target.resolved_view_desc.texture.first_level;
    const auto first_layer = target.resolved_view_desc.texture.first_layer;
    const auto levels = std::max<std::uint32_t>(resource_desc.texture.levels, 1u);
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
    session->region = region;
    session->width = region.width();
    session->height = region.height();
    session->signal = request->signal;
    session->frames = request->frames;
    session->shader_hash = request->shader_hash;
    session->include_unchanged = request->include_unchanged;
    session->used_clone = target.used_clone;
    session->resource_format_name = formatting::StreamToString(scratch_format);

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

    session->pass.pipeline_subobjects.compute_shader = reduce_shader;
    session->pass.shader_resource_slots.views = {session->scratch_srv};
    session->pass.unordered_access_slots.views = {session->stats_uav};
    session->pass.push_constants[renodx::utils::render::ConstantBuffersSlots{.slot = 0, .space = 0}] =
        std::span<const float>(session->constants.data(), session->constants.size());
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

// Fails whatever is still queued or running when the device goes away.
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
    internal::DestroySessionObjects(watcher->session.get(), device);
    fail_request(watcher->session->request);
    watcher->session.reset();
  }
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

  auto request = std::make_shared<Request>();
  request->resource_view = reshade::api::resource_view{resource_view_handle};
  request->prefer_clone = mcp_arguments::GetOptional<bool>(arguments, "preferClone").value_or(false);
  request->region = resource_analysis::internal::ParseRegion(arguments);
  request->signal = resource_analysis::internal::ParseSignal(mcp_arguments::GetOptional<std::string>(arguments, "signal").value_or("raw"));
  request->frames = frames;
  request->include_unchanged = mcp_arguments::GetOptional<bool>(arguments, "includeUnchanged").value_or(false);
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
        .text = "Timed out waiting for the next present to start the region watch (another watch may still be running).",
        .structured_content = json{{"error", "Timed out waiting for the next present to start the region watch (another watch may still be running)."}},
        .is_error = true,
    };
  }

  const auto watch_timeout = std::chrono::seconds(30) + std::chrono::milliseconds(static_cast<std::int64_t>(frames) * 250);
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
