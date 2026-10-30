/*
 * Copyright (C) 2026 Vulta93
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <include/reshade.hpp>

#include "../../../utils/data.hpp"
#include "../../../utils/gpu_fence.hpp"
#include "../../../utils/mcp/arguments.hpp"
#include "../../../utils/mcp/types.hpp"
#include "../../../utils/resource.hpp"
#include "../../../utils/state.hpp"
#include "../../../utils/swapchain.hpp"
#include "../formatting.hpp"
#include "./frame_overview.hpp"

// devkit_gpu_timing: GPU time per pass and per shader of a normally running frame, from timestamp queries.
//
// The snapshot records every draw on the CPU, which starves the GPU, so it cannot be timed. This tool does little CPU
// work per draw: one timestamp query written right before the draw and a note of the bound shader and render targets.
// A draw's time runs from its stamp to the next stamp in GPU order (draws overlap on a GPU, so only passes and shaders
// are reported). Clears, copies and resolves have no stamp and are billed to the previous draw.
//
// D3D12 records the stamps per command list (the model of watch_region) and commits them at execute_command_list in
// submission order; only lists executed on the presenting queue are timed. The first stamp of every list (its first
// draw) starts a new "between lists" interval, which is idle time plus the previous list's last draw. D3D11 stamps the
// immediate context only and commits its list at present; each frame is wrapped in a D3D11_QUERY_TIMESTAMP_DISJOINT.
//
// Phases of PHASE_PRESENTS presents alternate between instrumented frames (a stamp per draw) and plain frames (a stamp
// per list only). A frame is classified after the fact from the lists it executed; mixed frames are dropped. The plain
// frames give the GPU time of a frame without the instrumentation, to show how much the stamps changed it.
namespace renodx::addons::devkit::mcp::gpu_timing {

using json = renodx::utils::mcp::json;
using ToolResult = renodx::utils::mcp::ToolResult;
namespace mcp_arguments = renodx::utils::mcp::arguments;

constexpr std::uint32_t DEFAULT_FRAMES = 6u;
constexpr std::uint32_t MAX_FRAMES = 32u;
constexpr std::uint32_t PLAIN_FRAMES_NEEDED = 3u;
constexpr std::uint32_t PHASE_PRESENTS = 4u;
constexpr std::uint32_t MAX_PRESENTS = 1200u;
// Slots a list takes at once, so that its stamps are contiguous and one resolve covers many of them.
constexpr std::uint32_t INSTRUMENTED_CHUNK = 32u;
// D3D11 makes one query object per slot, so its default is smaller; D3D12 uses a native heap.
constexpr std::uint32_t DEFAULT_MAX_STAMPS_D3D11 = 65536u;
constexpr std::uint32_t DEFAULT_MAX_STAMPS_D3D12 = 262144u;
constexpr std::uint32_t MIN_MAX_STAMPS = 1024u;
constexpr std::uint32_t MAX_MAX_STAMPS = 524288u;
constexpr std::uint32_t DEFAULT_MAX_LISTED = 40u;
constexpr std::uint32_t MAX_MAX_LISTED = 500u;
constexpr std::uint32_t D3D11_DISJOINT_QUERIES = 16u;
constexpr std::size_t MAX_RETIRED_SESSIONS = 8u;
constexpr std::size_t MAX_PENDING_FRAMES = 8u;
constexpr auto PENDING_FRAME_TIMEOUT = std::chrono::seconds(8);
// The game must present within this long after the tool is called, or it is paused or minimised.
constexpr auto START_TIMEOUT = std::chrono::seconds(15);
// The presenting swap chain presents nothing for this long while others do: it was replaced.
constexpr auto SWAPCHAIN_LOST_TIMEOUT = std::chrono::seconds(5);
constexpr auto TOOL_TIMEOUT = std::chrono::seconds(180);
// The instrumented total may differ from the plain one by this fraction before the run is flagged.
constexpr double CONTAMINATED_FRACTION = 0.10;
// A plain frame whose GPU span is below this share of the present-to-present interval means the GPU waits for the CPU.
constexpr double CPU_BOUND_FRACTION = 0.80;
constexpr std::uint32_t MAX_PASS_SHADERS = 4u;

enum class RunKind : std::uint8_t {
  DRAW,
  DISPATCH,
};

// What a pass wrote, described while it was bound: a view can be released or rewritten before the result is built.
struct RunTarget {
  std::uint64_t resource_handle = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  reshade::api::format format = reshade::api::format::unknown;

  bool operator==(const RunTarget&) const = default;
};

// Consecutive stamps with equal runs form a pass: draws by render target resources, dispatches by shader.
struct Run {
  RunKind kind = RunKind::DRAW;
  std::uint32_t dispatch_shader_hash = 0u;
  std::vector<RunTarget> targets;

  bool operator==(const Run& other) const {
    return kind == other.kind && dispatch_shader_hash == other.dispatch_shader_hash
           && std::ranges::equal(targets, other.targets, [](const RunTarget& left, const RunTarget& right) {
                return left.resource_handle == right.resource_handle;
              });
  }
};

struct Stamp {
  std::uint32_t slot = 0u;
  std::uint32_t shader_hash = 0u;
  RunKind kind = RunKind::DRAW;
  // Index into the list's runs until committed, then into the session's runs.
  std::uint32_t run = 0u;
  bool list_start = false;
  bool end_of_frame = false;
  bool valid = true;
};

struct ListState {
  bool bundle = false;
  bool instrumented = false;
  // Executed (D3D12) or committed (D3D11) since the last reset; a second execution is a resubmission.
  bool executed = false;
  std::uint32_t chunk_next = 0u;
  std::uint32_t chunk_end = 0u;
  // The render target views of the last stamp, to skip the resource lookups while they do not change.
  std::vector<std::uint64_t> last_views;
  std::vector<Stamp> stamps;
  std::vector<Run> runs;
  std::optional<std::size_t> committed_list = std::nullopt;
};

struct CommittedList {
  bool instrumented = false;
  std::size_t first_stamp = 0u;
  std::size_t stamp_count = 0u;
};

struct FrameRecord {
  std::size_t first_stamp = 0u;
  std::size_t stamp_end = 0u;
  std::size_t first_list = 0u;
  std::size_t list_end = 0u;
  std::chrono::steady_clock::time_point queued_at;
  double present_interval_ms = 0.0;
  // D3D12: the fence value after the resolves, signalled at the next present (0 until then or when the signal failed).
  // D3D11: the frame's disjoint query.
  std::uint64_t fence_value = 0u;
  bool needs_signal = false;
  bool signal_failed = false;
  std::uint32_t disjoint_index = 0u;
};

struct PassSegment {
  Run run;
  double ms = 0.0;
  std::uint32_t draws = 0u;
  std::vector<std::uint32_t> shader_hashes;
};

struct ShaderSegment {
  double ms = 0.0;
  std::uint32_t draws = 0u;
};

struct FrameResult {
  double total_ms = 0.0;
  double between_lists_ms = 0.0;
  double present_interval_ms = 0.0;
  // Command lists the frame executed on the presenting queue (with stamps).
  std::size_t lists = 0u;
  std::vector<PassSegment> passes;
  std::map<std::pair<std::uint32_t, RunKind>, ShaderSegment> shaders;
};

// One pass of the reference frame as the snapshot comparison needs it.
struct PassSignature {
  RunKind kind = RunKind::DRAW;
  std::uint32_t dispatch_shader_hash = 0u;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> target_sizes;
};

struct Request {
  std::uint32_t frames = DEFAULT_FRAMES;
  std::uint32_t max_listed = DEFAULT_MAX_LISTED;
  std::optional<std::uint32_t> max_stamps = std::nullopt;
  bool stable_power_state = false;
  std::mutex mutex;
  std::condition_variable cv;
  // The first present of the presenting swap chain was seen: from there on the run ends by itself.
  bool started = false;
  bool completed = false;
  std::optional<ToolResult> result = std::nullopt;
  // Set with the result: the reference frame's passes in order, for matching them to a snapshot.
  std::vector<PassSignature> pass_signatures;
};

struct ToolContext {
  std::function<std::uint32_t(const json& arguments)> resolve_device_index;
  // Returns the reason when the session could not start.
  std::function<std::optional<std::string>(std::uint32_t device_index, const std::shared_ptr<Request>& request)> enqueue_request;
  std::function<void(std::uint32_t device_index, const std::shared_ptr<Request>& request)> cancel_request;
  std::function<frame_overview::Input(std::uint32_t device_index)> collect_snapshot_commands;
};

struct Session {
  reshade::api::device* device = nullptr;
  reshade::api::device_api api = reshade::api::device_api::d3d11;
  std::shared_ptr<Request> request;
  std::uint32_t capacity = 0u;
  reshade::api::query_heap heap = {0u};
  // D3D12: a native heap (ReShade's creates one fence per slot) and a readback buffer for the resolves.
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> native_heap;
  reshade::api::resource readback = {0u};
  std::unique_ptr<renodx::utils::gpu_fence::Fence> fence;
  // D3D11: a ring of disjoint queries, one per frame in flight.
  std::vector<Microsoft::WRL::ComPtr<ID3D11Query>> disjoint_queries;
  std::string stable_power_state = "off";
  bool stable_power_state_set = false;

  std::atomic<bool> running = true;
  std::atomic<bool> finished = false;
  std::atomic<bool> stamp_draws = true;
  std::atomic<bool> exhausted = false;
  std::atomic<std::uint32_t> next_slot = 0u;
  // Set by the first present that claims the run (two swap chains can present at once); queue follows once it is set up.
  std::atomic<bool> present_claimed = false;
  std::atomic<reshade::api::command_queue*> queue = nullptr;
  reshade::api::swapchain* swapchain = nullptr;
  std::uint64_t frequency = 0u;

  std::atomic<std::uint32_t> untracked_lists = 0u;
  std::atomic<std::uint32_t> other_queue_lists = 0u;
  std::atomic<std::uint32_t> resubmitted_lists = 0u;
  std::atomic<std::uint32_t> bundle_executions = 0u;
  std::atomic<std::uint64_t> deferred_draws = 0u;
  std::atomic<std::uint32_t> deferred_executions = 0u;

  std::shared_mutex lists_mutex;
  std::unordered_map<std::uint64_t, ListState> lists;

  std::shared_mutex commit_mutex;
  std::vector<Stamp> committed;
  std::vector<CommittedList> committed_lists;
  std::vector<Run> runs;

  // Only the presenting thread touches the rest.
  std::size_t frame_stamp_cursor = 0u;
  std::size_t frame_list_cursor = 0u;
  std::uint32_t presents = 0u;
  // Atomic because a second swap chain's present reads it to notice that the presenting one is gone.
  std::atomic<std::chrono::steady_clock::time_point> last_present_time;
  std::deque<FrameRecord> pending_frames;
  std::vector<FrameResult> instrumented_results;
  std::vector<FrameResult> plain_results;
  std::uint32_t dropped_mixed = 0u;
  std::uint32_t dropped_disjoint = 0u;
  std::uint32_t dropped_lost = 0u;
  std::uint32_t dropped_empty = 0u;
  std::uint32_t non_monotonic_intervals = 0u;
  // Only ReleaseFinishedSessions touches this, under the timer's lock.
  bool cpu_data_released = false;
};

// Owned by the per-device data. `active` is a lock-free gate, so draws pay one atomic load while no session runs.
struct Timer {
  std::shared_mutex mutex;
  std::atomic<bool> active = false;
  // Set while Start creates its objects, so that two starts cannot both run.
  std::atomic<bool> starting = false;
  std::atomic<bool> shut_down = false;
  // A finished session whose D3D11 queries or recorded CPU data are not released yet.
  std::atomic<bool> has_unreleased_sessions = false;
  std::shared_ptr<Session> session;
  // Finished D3D12 sessions stay until the device is destroyed: a command list recorded during a session may still
  // execute stamps into its heap.
  std::vector<std::shared_ptr<Session>> retired_sessions;
};

namespace internal {

[[nodiscard]] inline std::shared_ptr<Session> GetRunningSession(Timer* timer) {
  if (!timer->active.load(std::memory_order_acquire)) return nullptr;
  std::shared_ptr<Session> session;
  {
    std::shared_lock lock(timer->mutex);
    session = timer->session;
  }
  if (session == nullptr || !session->running.load(std::memory_order_acquire)) return nullptr;
  return session;
}

// Moves a list's stamps into the session's committed sequence. Returns false for a resubmission (the list was committed
// before without a reset): both executions overwrite the same slots, so the stamps of both are invalidated. The caller
// holds lists_mutex (shared, or exclusive while it resets the list); the commit lock is taken here.
inline void CommitList(Session* session, ListState* list) {
  std::unique_lock lock(session->commit_mutex);
  if (list->executed) {
    if (list->committed_list.has_value()) {
      const auto& earlier = session->committed_lists[list->committed_list.value()];
      for (auto index = earlier.first_stamp; index < earlier.first_stamp + earlier.stamp_count; ++index) {
        session->committed[index].valid = false;
      }
      session->resubmitted_lists.fetch_add(1u, std::memory_order_relaxed);
    }
    return;
  }
  list->executed = true;
  if (list->stamps.empty()) return;

  const auto run_base = static_cast<std::uint32_t>(session->runs.size());
  session->runs.insert(session->runs.end(), std::make_move_iterator(list->runs.begin()), std::make_move_iterator(list->runs.end()));
  const auto first_stamp = session->committed.size();
  for (auto stamp : list->stamps) {
    stamp.run += run_base;
    session->committed.push_back(stamp);
  }
  session->committed_lists.push_back(CommittedList{
      .instrumented = list->instrumented,
      .first_stamp = first_stamp,
      .stamp_count = list->stamps.size(),
  });
  list->committed_list = session->committed_lists.size() - 1u;
  list->stamps.clear();
  list->runs.clear();
}

}  // namespace internal

// Releases the resources a finished session no longer needs; the heap of a D3D12 session stays (see Timer).
inline void ReleaseSessionObjects(Session* session) {
  // A D3D12 heap is native, owned by native_heap; ReShade only owns the D3D11 one.
  if (session->api == reshade::api::device_api::d3d11 && session->heap.handle != 0u) {
    session->device->destroy_query_heap(session->heap);
  }
  session->heap = {0u};
  if (session->readback.handle != 0u) {
    session->device->destroy_resource(session->readback);
    session->readback = {0u};
  }
  session->fence.reset();
  session->native_heap.Reset();
  session->disjoint_queries.clear();
}

inline void ReleaseStablePowerState(Session* session) {
  if (!session->stable_power_state_set) return;
  reinterpret_cast<ID3D12Device*>(static_cast<std::uintptr_t>(session->device->get_native()))->SetStablePowerState(FALSE);
  session->stable_power_state_set = false;
}

// Ends a session (once): the gate closes and the request gets its result. The session's objects and recorded data are
// released later by ReleaseFinishedSessions, once no hook holds the session any more.
inline void FinishSession(Timer* timer, const std::shared_ptr<Session>& session, std::optional<ToolResult> result) {
  if (session->finished.exchange(true)) return;
  session->running.store(false, std::memory_order_release);
  ReleaseStablePowerState(session.get());
  {
    std::unique_lock lock(timer->mutex);
    if (timer->session == session) {
      timer->session = nullptr;
      timer->active.store(false, std::memory_order_release);
    }
    timer->retired_sessions.push_back(session);
  }
  timer->has_unreleased_sessions.store(true, std::memory_order_release);
  if (result.has_value()) {
    {
      std::unique_lock request_lock(session->request->mutex);
      session->request->result = std::move(result);
      session->request->completed = true;
    }
    session->request->cv.notify_all();
  }
}

// Frees what finished sessions that no hook still uses (use_count 1: only the timer holds them) do not need any more, at
// the next start and at the next present: the D3D11 queries, and of a D3D12 session the recorded stamps and results (it
// keeps the heap, readback buffer and fence, see Timer). The sessions can be failed or cancelled ones, which never got to
// release their data themselves. A D3D12 session that never wrote a stamp (the game was not presenting) has nothing a list
// could still execute into its heap, so it is released like a D3D11 one and does not use up one of the retired slots.
// The count check is only valid because every copy of a session is taken under timer->mutex.
inline void ReleaseFinishedSessions(Timer* timer) {
  std::unique_lock lock(timer->mutex);
  std::erase_if(timer->retired_sessions, [](const std::shared_ptr<Session>& session) {
    if (session.use_count() != 1) return false;
    if (session->api == reshade::api::device_api::d3d11 || session->next_slot.load(std::memory_order_relaxed) == 0u) {
      ReleaseSessionObjects(session.get());
      return true;
    }
    if (!session->cpu_data_released) {
      session->instrumented_results = {};
      session->plain_results = {};
      session->pending_frames = {};
      session->lists = {};
      session->committed = {};
      session->committed_lists = {};
      session->runs = {};
      session->cpu_data_released = true;
    }
    return false;
  });
  timer->has_unreleased_sessions.store(
      std::ranges::any_of(timer->retired_sessions, [](const std::shared_ptr<Session>& session) { return session->api == reshade::api::device_api::d3d11 || !session->cpu_data_released; }),
      std::memory_order_release);
}

inline std::optional<std::string> Start(Timer* timer, reshade::api::device* device, const std::shared_ptr<Request>& request) {
  const auto api = device->get_api();
  if (api != reshade::api::device_api::d3d11 && api != reshade::api::device_api::d3d12) {
    return "GPU timing supports D3D11 and D3D12 devices only.";
  }
  if (timer->shut_down.load(std::memory_order_acquire)) return "The device is being destroyed.";
  if (timer->starting.exchange(true)) return "A GPU timing run is already in progress on this device.";
  struct StartingReset {
    std::atomic<bool>* starting;
    ~StartingReset() { starting->store(false); }
  } starting_reset{.starting = &timer->starting};

  ReleaseFinishedSessions(timer);
  {
    std::shared_lock lock(timer->mutex);
    if (timer->session != nullptr) return "A GPU timing run is already in progress on this device.";
    if (timer->retired_sessions.size() >= MAX_RETIRED_SESSIONS) {
      return "This device already ran as many GPU timing sessions as it allows (their query heaps stay until the game exits). Restart the game.";
    }
  }

  auto session = std::make_shared<Session>();
  session->device = device;
  session->api = api;
  session->request = request;
  const auto default_max_stamps = (api == reshade::api::device_api::d3d11 ? DEFAULT_MAX_STAMPS_D3D11 : DEFAULT_MAX_STAMPS_D3D12);
  session->capacity = std::clamp(request->max_stamps.value_or(default_max_stamps), MIN_MAX_STAMPS, MAX_MAX_STAMPS);
  if (api == reshade::api::device_api::d3d11) {
    if (!device->create_query_heap(reshade::api::query_type::timestamp, session->capacity, &session->heap)) {
      return "The device could not create the timestamp queries.";
    }
    auto* native_device = reinterpret_cast<ID3D11Device*>(static_cast<std::uintptr_t>(device->get_native()));
    const D3D11_QUERY_DESC disjoint_desc = {.Query = D3D11_QUERY_TIMESTAMP_DISJOINT, .MiscFlags = 0u};
    session->disjoint_queries.resize(D3D11_DISJOINT_QUERIES);
    for (auto& query : session->disjoint_queries) {
      if (FAILED(native_device->CreateQuery(&disjoint_desc, &query))) {
        ReleaseSessionObjects(session.get());
        return "The device could not create a timestamp-disjoint query.";
      }
    }
  } else {
    auto* native_device = reinterpret_cast<ID3D12Device*>(static_cast<std::uintptr_t>(device->get_native()));
    const D3D12_QUERY_HEAP_DESC heap_desc = {.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = session->capacity, .NodeMask = 0u};
    if (FAILED(native_device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&session->native_heap)))) {
      return "The device could not create a timestamp query heap.";
    }
    session->heap = {reinterpret_cast<std::uintptr_t>(session->native_heap.Get())};
    if (!device->create_resource(
            reshade::api::resource_desc(static_cast<std::uint64_t>(session->capacity) * sizeof(std::uint64_t), reshade::api::memory_heap::gpu_to_cpu, reshade::api::resource_usage::copy_dest),
            nullptr,
            reshade::api::resource_usage::copy_dest,
            &session->readback)) {
      session->heap = {0u};
      return "The device could not create the readback buffer.";
    }
    try {
      session->fence = std::make_unique<renodx::utils::gpu_fence::Fence>(device);
    } catch (const std::exception& exception) {
      ReleaseSessionObjects(session.get());
      return std::format("The device could not create a GPU fence: {}", exception.what());
    }
    if (request->stable_power_state) {
      // SetStablePowerState removes the device when Developer Mode is off, so the setting is checked first.
      DWORD developer_mode = 0u;
      DWORD developer_mode_size = sizeof(developer_mode);
      const auto read = RegGetValueW(
          HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
          L"AllowDevelopmentWithoutDevLicense",
          RRF_RT_REG_DWORD,
          nullptr,
          &developer_mode,
          &developer_mode_size);
      if (read == ERROR_SUCCESS && developer_mode == 1u) {
        if (SUCCEEDED(native_device->SetStablePowerState(TRUE))) {
          session->stable_power_state = "active";
          session->stable_power_state_set = true;
        } else {
          session->stable_power_state = "unavailable (SetStablePowerState failed)";
        }
      } else {
        session->stable_power_state = "unavailable (Windows Developer Mode is off)";
      }
    }
  }

  // Shutdown sets shut_down before it takes this lock, so a start that gets here either sees it or is finished by it.
  std::unique_lock lock(timer->mutex);
  if (timer->shut_down.load(std::memory_order_acquire)) {
    lock.unlock();
    ReleaseStablePowerState(session.get());
    ReleaseSessionObjects(session.get());
    return "The device is being destroyed.";
  }
  timer->session = session;
  timer->active.store(true, std::memory_order_release);
  return std::nullopt;
}

// A tool timeout: the session ends without a result.
inline void Cancel(Timer* timer, const std::shared_ptr<Request>& request) {
  std::shared_ptr<Session> session;
  {
    std::shared_lock lock(timer->mutex);
    session = timer->session;
  }
  if (session != nullptr && session->request == request) {
    FinishSession(timer, session, std::nullopt);
  }
}

inline void Shutdown(Timer* timer, reshade::api::device* device) {
  timer->shut_down.store(true, std::memory_order_release);
  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::unique_lock lock(timer->mutex);
    timer->active.store(false, std::memory_order_release);
    if (timer->session != nullptr) {
      sessions.push_back(timer->session);
    }
  }
  for (const auto& session : sessions) {
    FinishSession(timer, session, ToolResult{
                                      .text = "The device was destroyed during the GPU timing run.",
                                      .structured_content = json{{"error", "The device was destroyed during the GPU timing run."}},
                                      .is_error = true,
                                  });
  }
  std::unique_lock lock(timer->mutex);
  for (const auto& session : timer->retired_sessions) {
    if (session->device == device) {
      ReleaseSessionObjects(session.get());
    }
  }
  timer->retired_sessions.clear();
}

// init_command_list / reset_command_list / destroy_command_list. A D3D12 list reset after the session started is
// tracked; lists seen only recording are not, because their state is unknown.
inline void OnCommandListReset(Timer* timer, reshade::api::command_list* cmd_list, bool destroyed) {
  const auto session = internal::GetRunningSession(timer);
  if (session == nullptr || session->api != reshade::api::device_api::d3d12) return;

  const auto handle = reinterpret_cast<std::uint64_t>(cmd_list);
  std::unique_lock lock(session->lists_mutex);
  if (destroyed) {
    session->lists.erase(handle);
    return;
  }
  auto& list = session->lists[handle];
  list = ListState{};
  auto* native_cmd_list = reinterpret_cast<ID3D12GraphicsCommandList*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
  list.bundle = (native_cmd_list->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE);
}

// Called before every draw and dispatch: writes the stamp and notes what is bound.
inline void OnDrawOrDispatch(Timer* timer, reshade::api::command_list* cmd_list, bool is_dispatch) {
  const auto session = internal::GetRunningSession(timer);
  if (session == nullptr || cmd_list->get_device() != session->device) return;

  if (session->exhausted.load(std::memory_order_relaxed)) return;
  if (session->api == reshade::api::device_api::d3d11) {
    auto* native_context = reinterpret_cast<ID3D11DeviceContext*>(static_cast<std::uintptr_t>(cmd_list->get_native()));
    if (native_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
      session->deferred_draws.fetch_add(1u, std::memory_order_relaxed);
      return;
    }
  }

  // The list is used under the lock, as watch_region does: the D3D11 present resets the immediate context's list, and a
  // reset or destroy of a D3D12 list takes the lock exclusively.
  const auto handle = reinterpret_cast<std::uint64_t>(cmd_list);
  std::shared_lock lists_lock(session->lists_mutex);
  auto pair = session->lists.find(handle);
  if (pair == session->lists.end()) {
    // A D3D12 list the session saw no reset of was already recording; the D3D11 immediate context is tracked from its
    // first draw.
    if (session->api == reshade::api::device_api::d3d12) return;
    lists_lock.unlock();
    {
      std::unique_lock insert_lock(session->lists_mutex);
      session->lists.try_emplace(handle);
    }
    lists_lock.lock();
    pair = session->lists.find(handle);
    if (pair == session->lists.end()) return;
  }
  auto* list = &pair->second;
  if (list->bundle) return;

  const bool list_start = list->stamps.empty();
  if (list_start) {
    list->instrumented = session->stamp_draws.load(std::memory_order_relaxed);
  } else if (!list->instrumented) {
    return;
  }
  if (list->chunk_next == list->chunk_end) {
    const auto chunk = (list->instrumented ? INSTRUMENTED_CHUNK : 1u);
    const auto first_slot = session->next_slot.fetch_add(chunk, std::memory_order_relaxed);
    if (first_slot + chunk > session->capacity) {
      session->exhausted.store(true, std::memory_order_relaxed);
      return;
    }
    list->chunk_next = first_slot;
    list->chunk_end = first_slot + chunk;
  }
  const auto slot = list->chunk_next++;
  cmd_list->end_query(session->heap, reshade::api::query_type::timestamp, slot);

  std::uint32_t shader_hash = 0u;
  const auto kind = (is_dispatch ? RunKind::DISPATCH : RunKind::DRAW);
  if (is_dispatch) {
    shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::compute_shader);
    if (list->runs.empty() || list->runs.back().kind != kind || list->runs.back().dispatch_shader_hash != shader_hash) {
      list->runs.push_back(Run{.kind = kind, .dispatch_shader_hash = shader_hash});
    }
    list->last_views.clear();
  } else {
    shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::pixel_shader);
    if (shader_hash == 0u) {
      shader_hash = renodx::utils::state::GetCurrentShaderHash(cmd_list, reshade::api::pipeline_stage::vertex_shader);
    }
    static thread_local std::vector<std::uint64_t> views;
    views.clear();
    if (const auto* render_target_data = renodx::utils::data::Get<renodx::utils::swapchain::CommandListData>(cmd_list);
        render_target_data != nullptr) {
      for (const auto& view : render_target_data->current_render_targets) {
        if (view.handle != 0u) {
          views.push_back(view.handle);
        }
      }
    }
    // The resources are looked up only when the bound views change, while they are bound.
    if (list->runs.empty() || list->runs.back().kind != kind || views != list->last_views) {
      Run run = {.kind = kind};
      for (const auto view : views) {
        const auto resource = renodx::utils::resource::GetResourceFromView(session->device, reshade::api::resource_view{view});
        const auto desc = renodx::utils::resource::GetResourceDesc(session->device, resource);
        run.targets.push_back(RunTarget{
            .resource_handle = resource.handle,
            .width = desc.texture.width,
            .height = desc.texture.height,
            .format = desc.texture.format,
        });
      }
      if (list->runs.empty() || list->runs.back() != run) {
        list->runs.push_back(std::move(run));
      }
      list->last_views = views;
    }
  }
  list->stamps.push_back(Stamp{
      .slot = slot,
      .shader_hash = shader_hash,
      .kind = kind,
      .run = static_cast<std::uint32_t>(list->runs.size() - 1u),
      .list_start = list_start,
  });
}

// D3D12: commits the list in submission order.
inline void OnExecuteCommandList(Timer* timer, reshade::api::command_queue* queue, reshade::api::command_list* cmd_list) {
  const auto session = internal::GetRunningSession(timer);
  if (session == nullptr || session->api != reshade::api::device_api::d3d12 || cmd_list->get_device() != session->device) return;

  std::shared_lock lists_lock(session->lists_mutex);
  const auto pair = session->lists.find(reinterpret_cast<std::uint64_t>(cmd_list));
  if (pair == session->lists.end()) {
    session->untracked_lists.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  auto* list = &pair->second;
  const auto presenting_queue = session->queue.load(std::memory_order_acquire);
  if (presenting_queue != nullptr && queue != presenting_queue) {
    if (!list->stamps.empty()) {
      session->other_queue_lists.fetch_add(1u, std::memory_order_relaxed);
    }
    return;
  }
  if (presenting_queue == nullptr) {
    // Before the first present there is no frame to put the list in.
    list->executed = true;
    list->stamps.clear();
    list->runs.clear();
    return;
  }
  internal::CommitList(session.get(), list);
}

// D3D12 bundles and D3D11 deferred command lists run inside a stamped list and are billed to the previous stamp.
inline void OnExecuteSecondaryCommandList(Timer* timer, reshade::api::command_list* cmd_list) {
  const auto session = internal::GetRunningSession(timer);
  if (session == nullptr || cmd_list->get_device() != session->device) return;
  if (session->api == reshade::api::device_api::d3d12) {
    session->bundle_executions.fetch_add(1u, std::memory_order_relaxed);
  } else {
    session->deferred_executions.fetch_add(1u, std::memory_order_relaxed);
  }
}

// Called at the start of the Devkit's present handler, after a mod's present work (the Devkit loads last), so the last
// interval covers that work. Ends the frame, resolves, reads back finished frames and decides the next phase.
inline void OnPresent(Timer* timer, reshade::api::command_queue* queue, reshade::api::swapchain* swapchain) {
  if (!timer->active.load(std::memory_order_acquire)) {
    if (timer->has_unreleased_sessions.load(std::memory_order_acquire)) {
      ReleaseFinishedSessions(timer);
    }
    return;
  }
  const auto session = internal::GetRunningSession(timer);
  if (session == nullptr || swapchain->get_device() != session->device) return;

  const auto now = std::chrono::steady_clock::now();
  auto* device = session->device;
  const bool is_d3d12 = (session->api == reshade::api::device_api::d3d12);
  auto* immediate_cmd_list = queue->get_immediate_command_list();
  auto* native_context = (is_d3d12 ? nullptr : reinterpret_cast<ID3D11DeviceContext*>(static_cast<std::uintptr_t>(queue->get_native())));
  auto fail = [&](const std::string& message) {
    FinishSession(timer, session, ToolResult{.text = message, .structured_content = json{{"error", message}}, .is_error = true});
  };

  if (session->queue.load(std::memory_order_acquire) == nullptr) {
    if (session->present_claimed.exchange(true)) return;
    if (is_d3d12) {
      session->frequency = queue->get_timestamp_frequency();
      if (session->frequency == 0u) {
        fail("The presenting queue does not support timestamp queries.");
        return;
      }
    } else {
      native_context->Begin(session->disjoint_queries[0].Get());
      std::unique_lock lock(session->lists_mutex);
      session->lists.clear();
    }
    session->swapchain = swapchain;
    session->presents = 1u;
    session->last_present_time = now;
    session->queue.store(queue, std::memory_order_release);
    {
      std::unique_lock request_lock(session->request->mutex);
      session->request->started = true;
    }
    session->request->cv.notify_all();
    return;
  }
  if (queue != session->queue.load(std::memory_order_acquire) || swapchain != session->swapchain) {
    // The presenting swap chain was replaced (a fullscreen switch, some resizes): no frame would ever complete the run.
    if (now - session->last_present_time.load() > SWAPCHAIN_LOST_TIMEOUT) {
      fail("The swap chain the timing runs on stopped presenting while another one still does (it was probably replaced). Try again.");
    }
    return;
  }

  ++session->presents;
  const auto present_interval_ms = std::chrono::duration<double, std::milli>(now - session->last_present_time.load()).count();
  session->last_present_time.store(now);

  // The previous present's resolves are in the immediate list, which ReShade flushed at the end of that present, so a
  // signal queued now follows them without a flush of ours (an extra flush would cycle the list's allocator ring faster).
  if (is_d3d12) {
    for (auto& frame : session->pending_frames) {
      if (!frame.needs_signal) continue;
      frame.needs_signal = false;
      try {
        frame.fence_value = renodx::utils::gpu_fence::QueueSignal(queue, session->fence.get());
      } catch (const std::exception& exception) {
        reshade::log::message(reshade::log::level::warning, std::format("devkit::gpu_timing(fence signal failed: {})", exception.what()).c_str());
      }
      frame.signal_failed = (frame.fence_value == 0u);
    }
  }

  // The frame's last stamp, then its record.
  if (!session->exhausted.load(std::memory_order_relaxed)) {
    const auto end_slot = session->next_slot.fetch_add(1u, std::memory_order_relaxed);
    if (end_slot >= session->capacity) {
      session->exhausted.store(true, std::memory_order_relaxed);
    } else {
      immediate_cmd_list->end_query(session->heap, reshade::api::query_type::timestamp, end_slot);

      std::uint32_t disjoint_index = 0u;
      if (!is_d3d12) {
        {
          std::unique_lock lists_lock(session->lists_mutex);
          auto& immediate_list_state = session->lists[reinterpret_cast<std::uint64_t>(immediate_cmd_list)];
          internal::CommitList(session.get(), &immediate_list_state);
          immediate_list_state = ListState{};
        }
        disjoint_index = (session->presents - 2u) % D3D11_DISJOINT_QUERIES;
        native_context->End(session->disjoint_queries[disjoint_index].Get());
        native_context->Begin(session->disjoint_queries[(session->presents - 1u) % D3D11_DISJOINT_QUERIES].Get());
      }

      FrameRecord frame = {.queued_at = now, .present_interval_ms = present_interval_ms, .disjoint_index = disjoint_index};
      std::vector<std::uint32_t> resolve_slots;
      {
        std::unique_lock lock(session->commit_mutex);
        session->committed.push_back(Stamp{.slot = end_slot, .end_of_frame = true});
        frame.first_stamp = session->frame_stamp_cursor;
        frame.stamp_end = session->committed.size();
        frame.first_list = session->frame_list_cursor;
        frame.list_end = session->committed_lists.size();
        session->frame_stamp_cursor = frame.stamp_end;
        session->frame_list_cursor = frame.list_end;
        if (is_d3d12) {
          for (auto index = frame.first_stamp; index < frame.stamp_end; ++index) {
            if (session->committed[index].valid) {
              resolve_slots.push_back(session->committed[index].slot);
            }
          }
        }
      }
      if (is_d3d12) {
        std::ranges::sort(resolve_slots);
        for (std::size_t first = 0u; first < resolve_slots.size();) {
          std::size_t last = first;
          while (last + 1u < resolve_slots.size() && resolve_slots[last + 1u] == resolve_slots[last] + 1u) {
            ++last;
          }
          const auto count = static_cast<std::uint32_t>(last - first + 1u);
          immediate_cmd_list->copy_query_heap_results(
              session->heap, reshade::api::query_type::timestamp, resolve_slots[first], count, session->readback,
              static_cast<std::uint64_t>(resolve_slots[first]) * sizeof(std::uint64_t), sizeof(std::uint64_t));
          first = last + 1u;
        }
        frame.needs_signal = true;
      }
      if (session->pending_frames.size() >= MAX_PENDING_FRAMES) {
        // Fewer in flight than there are disjoint queries, so a query is never reused while its frame is pending.
        session->pending_frames.pop_front();
        ++session->dropped_lost;
      }
      session->pending_frames.push_back(frame);
    }
  }

  // Frames whose GPU work is done.
  while (!session->pending_frames.empty()) {
    const auto frame = session->pending_frames.front();
    const bool timed_out = frame.signal_failed || (now - frame.queued_at) > PENDING_FRAME_TIMEOUT;

    std::vector<Stamp> stamps;
    std::vector<bool> lists_instrumented;
    std::unordered_map<std::uint32_t, Run> frame_runs;
    {
      std::shared_lock lock(session->commit_mutex);
      stamps.assign(session->committed.begin() + frame.first_stamp, session->committed.begin() + frame.stamp_end);
      for (auto index = frame.first_list; index < frame.list_end; ++index) {
        lists_instrumented.push_back(session->committed_lists[index].instrumented);
      }
      for (const auto& stamp : stamps) {
        if (!stamp.end_of_frame) {
          frame_runs.try_emplace(stamp.run, session->runs[stamp.run]);
        }
      }
    }
    std::vector<std::uint64_t> ticks(stamps.size(), 0u);

    bool ready = false;
    bool disjoint = false;
    if (is_d3d12) {
      ready = (frame.fence_value != 0u && session->fence->IsComplete(frame.fence_value));
    } else {
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint_data = {};
      std::uint64_t end_ticks = 0u;
      ready = (native_context->GetData(session->disjoint_queries[frame.disjoint_index].Get(), &disjoint_data, sizeof(disjoint_data), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
              && device->get_query_heap_results(session->heap, stamps.back().slot, 1u, &end_ticks, sizeof(std::uint64_t));
      if (ready) {
        disjoint = (disjoint_data.Disjoint != FALSE);
        session->frequency = disjoint_data.Frequency;
      }
    }
    if (!ready && !timed_out) break;
    session->pending_frames.pop_front();
    if (!ready) {
      ++session->dropped_lost;
      continue;
    }
    if (disjoint) {
      ++session->dropped_disjoint;
      continue;
    }

    if (is_d3d12) {
      std::uint32_t max_slot = 0u;
      for (const auto& stamp : stamps) {
        max_slot = std::max(max_slot, stamp.slot);
      }
      void* mapped = nullptr;
      if (!device->map_buffer_region(session->readback, 0u, (static_cast<std::uint64_t>(max_slot) + 1u) * sizeof(std::uint64_t), reshade::api::map_access::read_only, &mapped)) {
        ++session->dropped_lost;
        continue;
      }
      for (std::size_t index = 0u; index < stamps.size(); ++index) {
        ticks[index] = static_cast<const std::uint64_t*>(mapped)[stamps[index].slot];
      }
      device->unmap_buffer_region(session->readback);
    } else {
      for (std::size_t index = 0u; index < stamps.size(); ++index) {
        if (!device->get_query_heap_results(session->heap, stamps[index].slot, 1u, &ticks[index], sizeof(std::uint64_t))) {
          ticks[index] = 0u;
        }
      }
    }

    const auto instrumented_lists = static_cast<std::size_t>(std::ranges::count(lists_instrumented, true));
    if (lists_instrumented.empty()) {
      ++session->dropped_empty;
      continue;
    }
    if (instrumented_lists != 0u && instrumented_lists != lists_instrumented.size()) {
      ++session->dropped_mixed;
      continue;
    }

    std::vector<std::size_t> order;
    for (std::size_t index = 0u; index < stamps.size(); ++index) {
      if (stamps[index].valid && ticks[index] != 0u) {
        order.push_back(index);
      }
    }
    if (order.size() < 2u || !stamps[order.back()].end_of_frame) {
      ++session->dropped_empty;
      continue;
    }

    const double ms_per_tick = 1000.0 / static_cast<double>(session->frequency);
    FrameResult result = {.present_interval_ms = frame.present_interval_ms, .lists = lists_instrumented.size()};
    result.total_ms = static_cast<double>(ticks[order.back()] - std::min(ticks[order.back()], ticks[order.front()])) * ms_per_tick;
    for (std::size_t position = 0u; position + 1u < order.size(); ++position) {
      const auto& stamp = stamps[order[position]];
      const auto& next = stamps[order[position + 1u]];
      const auto start_ticks = ticks[order[position]];
      const auto end_ticks = ticks[order[position + 1u]];
      if (end_ticks < start_ticks) {
        ++session->non_monotonic_intervals;
      }
      const double interval_ms = static_cast<double>(end_ticks > start_ticks ? end_ticks - start_ticks : 0u) * ms_per_tick;
      // The time up to the next list's first stamp is idle time plus this draw: it goes to "between lists", but the draw
      // still belongs to its pass and shader (with no time), so that the pass layout matches the snapshot's.
      const double billed_ms = (next.list_start ? 0.0 : interval_ms);
      if (next.list_start) {
        result.between_lists_ms += interval_ms;
      }
      const auto& run = frame_runs.at(stamp.run);
      if (result.passes.empty() || result.passes.back().run != run) {
        result.passes.push_back(PassSegment{.run = run});
      }
      auto& pass = result.passes.back();
      pass.ms += billed_ms;
      ++pass.draws;
      if (pass.shader_hashes.size() < MAX_PASS_SHADERS && stamp.shader_hash != 0u
          && std::ranges::find(pass.shader_hashes, stamp.shader_hash) == pass.shader_hashes.end()) {
        pass.shader_hashes.push_back(stamp.shader_hash);
      }
      auto& shader = result.shaders[{stamp.shader_hash, stamp.kind}];
      shader.ms += billed_ms;
      ++shader.draws;
    }

    if (instrumented_lists == 0u) {
      session->plain_results.push_back(std::move(result));
    } else {
      session->instrumented_results.push_back(std::move(result));
    }
  }

  // The phase of the next frames, and whether the run is done.
  const auto instrumented_count = static_cast<std::uint32_t>(session->instrumented_results.size());
  const auto plain_count = static_cast<std::uint32_t>(session->plain_results.size());
  const bool instrumented_done = instrumented_count >= session->request->frames;
  const bool plain_done = plain_count >= PLAIN_FRAMES_NEEDED;
  const bool even_phase = (((session->presents - 1u) / PHASE_PRESENTS) % 2u) == 0u;
  session->stamp_draws.store(!instrumented_done && (plain_done || even_phase), std::memory_order_relaxed);

  const bool out_of_stamps = session->exhausted.load(std::memory_order_relaxed) && session->pending_frames.empty();
  if (!((instrumented_done && plain_done) || out_of_stamps || session->presents > MAX_PRESENTS)) return;

  if (instrumented_count == 0u) {
    fail(std::format(
        "No instrumented frame could be timed ({} presents; dropped: {} mixed, {} lost, {} disjoint, {} empty). {}",
        session->presents, session->dropped_mixed, session->dropped_lost, session->dropped_disjoint, session->dropped_empty,
        (out_of_stamps ? "The stamp budget ran out; raise maxStamps." : "Try again with the game rendering a steady scene.")));
    return;
  }

  // Passes are matched across frames by identity (kind, target resources or dispatch shader, and the nth pass with that
  // key in the frame), so that the few passes a busy scene adds or drops from one frame to the next (culling, particles)
  // do not discard a whole frame. The reference frame is the one with the most passes, and per-pass numbers use every
  // frame that has the pass (`frames` in the output).
  struct PassKey {
    RunKind kind = RunKind::DRAW;
    std::uint32_t dispatch_shader_hash = 0u;
    std::vector<std::uint64_t> targets;
    std::uint32_t occurrence = 0u;

    auto operator<=>(const PassKey&) const = default;
  };
  const auto pass_keys = [](const FrameResult& frame) {
    std::vector<PassKey> keys;
    std::map<PassKey, std::uint32_t> seen;
    for (const auto& pass : frame.passes) {
      PassKey key = {.kind = pass.run.kind, .dispatch_shader_hash = pass.run.dispatch_shader_hash};
      for (const auto& target : pass.run.targets) {
        key.targets.push_back(target.resource_handle);
      }
      key.occurrence = seen[key]++;
      keys.push_back(std::move(key));
    }
    return keys;
  };
  const auto reference_iterator = std::ranges::max_element(
      session->instrumented_results,
      [](const FrameResult& left, const FrameResult& right) { return left.passes.size() < right.passes.size(); });
  const auto reference_index = static_cast<std::size_t>(reference_iterator - session->instrumented_results.begin());
  const auto& reference = *reference_iterator;
  std::map<PassKey, std::size_t> reference_pass_index;
  const auto reference_keys = pass_keys(reference);
  for (std::size_t index = 0u; index < reference_keys.size(); ++index) {
    reference_pass_index.emplace(reference_keys[index], index);
  }
  std::vector<std::vector<double>> pass_ms(reference.passes.size());
  std::vector<std::size_t> matched_passes;
  std::vector<double> instrumented_totals;
  std::vector<double> between_lists;
  std::vector<double> instrumented_intervals;
  std::vector<double> plain_intervals;
  struct ShaderSeries {
    std::vector<double> ms;
    std::vector<double> draws;
  };
  std::map<std::pair<std::uint32_t, RunKind>, ShaderSeries> shader_series;
  for (const auto& frame : session->instrumented_results) {
    instrumented_totals.push_back(frame.total_ms);
    between_lists.push_back(frame.between_lists_ms);
    instrumented_intervals.push_back(frame.present_interval_ms);
    for (const auto& [key, segment] : frame.shaders) {
      auto& series = shader_series[key];
      series.ms.push_back(segment.ms);
      series.draws.push_back(static_cast<double>(segment.draws));
    }
    std::size_t matched = 0u;
    const auto frame_keys = pass_keys(frame);
    for (std::size_t index = 0u; index < frame_keys.size(); ++index) {
      if (const auto found = reference_pass_index.find(frame_keys[index]); found != reference_pass_index.end()) {
        pass_ms[found->second].push_back(frame.passes[index].ms);
        ++matched;
      }
    }
    matched_passes.push_back(matched);
  }
  std::vector<double> plain_totals;
  for (const auto& frame : session->plain_results) {
    plain_totals.push_back(frame.total_ms);
    plain_intervals.push_back(frame.present_interval_ms);
  }
  const auto median = [](std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::ranges::sort(values);
    const auto middle = values.size() / 2u;
    if ((values.size() % 2u) == 1u) return values[middle];
    return (values[middle - 1u] + values[middle]) * 0.5;
  };
  const auto instrumented_median = median(instrumented_totals);
  const auto plain_median = median(plain_totals);
  const auto plain_interval_median = median(plain_intervals);

  json frame_json = {
      {"instrumentedMedianMs", instrumented_median},
      {"instrumentedMinMs", *std::ranges::min_element(instrumented_totals)},
      {"instrumentedPresentIntervalMedianMs", median(instrumented_intervals)},
      {"betweenListsMedianMs", median(between_lists)},
  };
  std::vector<std::string> notes;
  if (plain_totals.empty()) {
    notes.emplace_back("No plain frame was collected, so the cost of the stamps themselves is not known.");
  } else {
    const auto difference = instrumented_median - plain_median;
    const auto difference_fraction = (plain_median > 0.0 ? difference / plain_median : 0.0);
    const bool cpu_bound = plain_median < CPU_BOUND_FRACTION * plain_interval_median;
    frame_json["plainMedianMs"] = plain_median;
    frame_json["plainPresentIntervalMedianMs"] = plain_interval_median;
    frame_json["instrumentedMinusPlainMs"] = difference;
    frame_json["instrumentedMinusPlainFraction"] = difference_fraction;
    frame_json["contaminated"] = std::abs(difference_fraction) > CONTAMINATED_FRACTION;
    frame_json["cpuBound"] = cpu_bound;
    if (std::abs(difference_fraction) > CONTAMINATED_FRACTION) {
      notes.emplace_back("The instrumented frame differs from the plain frame by more than 10%: the per-pass numbers include the cost of the stamps or of a scene change.");
    }
    if (cpu_bound) {
      notes.emplace_back("The plain frame's GPU span is well below the present-to-present interval: the game is CPU bound, so per-pass times include idle time while the GPU waits for the CPU.");
    }
  }

  struct PassStats {
    std::size_t index = 0u;
    double median_ms = 0.0;
    double min_ms = 0.0;
  };
  std::vector<PassStats> pass_stats;
  double sum_of_minima = 0.0;
  for (std::size_t index = 0u; index < pass_ms.size(); ++index) {
    if (pass_ms[index].empty()) continue;
    pass_stats.push_back({.index = index, .median_ms = median(pass_ms[index]), .min_ms = *std::ranges::min_element(pass_ms[index])});
    sum_of_minima += pass_stats.back().min_ms;
  }
  std::ranges::sort(pass_stats, [](const PassStats& left, const PassStats& right) { return left.median_ms > right.median_ms; });

  json passes = json::array();
  for (const auto& stats : pass_stats | std::views::take(session->request->max_listed)) {
    const auto& segment = reference.passes[stats.index];
    json targets = json::array();
    for (const auto& target : segment.run.targets) {
      targets.push_back({
          {"resourceHandle", formatting::FormatHandle(target.resource_handle)},
          {"width", target.width},
          {"height", target.height},
          {"format", formatting::StreamToString(target.format)},
      });
    }
    json shader_hashes = json::array();
    for (const auto shader_hash : segment.shader_hashes) {
      shader_hashes.push_back(formatting::FormatShaderHash(shader_hash));
    }
    passes.push_back({
        {"pass", stats.index},
        {"kind", segment.run.kind == RunKind::DRAW ? "draw" : "dispatch"},
        {"draws", segment.draws},
        {"medianMs", stats.median_ms},
        {"minMs", stats.min_ms},
        {"frames", pass_ms[stats.index].size()},
        {"shareOfFrame",instrumented_median > 0.0 ? stats.median_ms / instrumented_median : 0.0},
        {"shaderHashes", shader_hashes},
        {"targets", targets},
    });
  }

  struct ShaderStats {
    std::uint32_t shader_hash = 0u;
    RunKind kind = RunKind::DRAW;
    double median_ms = 0.0;
    double min_ms = 0.0;
    double median_draws = 0.0;
  };
  std::vector<ShaderStats> shader_stats;
  for (const auto& [key, series] : shader_series) {
    const auto& [shader_hash, kind] = key;
    shader_stats.push_back({
        .shader_hash = shader_hash,
        .kind = kind,
        .median_ms = median(series.ms),
        .min_ms = *std::ranges::min_element(series.ms),
        .median_draws = median(series.draws),
    });
  }
  std::ranges::sort(shader_stats, [](const ShaderStats& left, const ShaderStats& right) { return left.median_ms > right.median_ms; });
  json shaders = json::array();
  for (const auto& stats : shader_stats | std::views::take(session->request->max_listed)) {
    shaders.push_back({
        {"shaderHash", formatting::FormatShaderHash(stats.shader_hash)},
        {"kind", stats.kind == RunKind::DRAW ? "draw" : "dispatch"},
        {"medianMs", stats.median_ms},
        {"minMs", stats.min_ms},
        {"drawsPerFrame", stats.median_draws},
    });
  }

  json instrumented_frames_json = json::array();
  for (std::size_t index = 0u; index < session->instrumented_results.size(); ++index) {
    const auto& frame = session->instrumented_results[index];
    instrumented_frames_json.push_back({
        {"lists", frame.lists},
        {"passes", frame.passes.size()},
        {"totalMs", frame.total_ms},
        {"betweenListsMs", frame.between_lists_ms},
        {"presentIntervalMs", frame.present_interval_ms},
        {"passesMatchedToReference", matched_passes[index]},
        {"isReference", index == reference_index},
    });
  }

  json output = {
      {"api", is_d3d12 ? "d3d12" : "d3d11"},
      {"instrumentedFrameDetails", instrumented_frames_json},
      {"frequencyHz", session->frequency},
      {"instrumentedFrames", instrumented_count},
      {"plainFrames", plain_count},
      {"frame", frame_json},
      {"passCount", reference.passes.size()},
      {"sumOfPassMinimaMs", sum_of_minima},
      {"passes", passes},
      {"shaders", shaders},
      {"stablePowerState", session->stable_power_state},
      {"framesDropped", {
          {"mixed", session->dropped_mixed},
          {"lost", session->dropped_lost},
          {"disjoint", session->dropped_disjoint},
          {"empty", session->dropped_empty},
      }},
      {"untimed", {
          {"otherQueueLists", session->other_queue_lists.load()},
          {"resubmittedLists", session->resubmitted_lists.load()},
          {"listsNotTracked", session->untracked_lists.load()},
          {"bundleExecutions", session->bundle_executions.load()},
          {"deferredContextDraws", session->deferred_draws.load()},
          {"deferredContextExecutions", session->deferred_executions.load()},
          {"nonMonotonicIntervals", session->non_monotonic_intervals},
          {"stampBudgetExhausted", session->exhausted.load()},
      }},
  };
  if (session->exhausted.load()) {
    notes.emplace_back("The stamp budget ran out before every frame was collected (maxStamps).");
  }
  notes.emplace_back("sumOfPassMinimaMs adds each pass's best frame and is not a frame time; compare frame.instrumentedMedianMs.");
  output["notes"] = notes;

  auto& signatures = session->request->pass_signatures;
  for (const auto& segment : reference.passes) {
    PassSignature signature = {.kind = segment.run.kind, .dispatch_shader_hash = segment.run.dispatch_shader_hash};
    for (const auto& target : segment.run.targets) {
      signature.target_sizes.emplace_back(target.width, target.height);
    }
    signatures.push_back(std::move(signature));
  }

  FinishSession(timer, session, ToolResult{
      .text = std::format(
          "GPU timing ({}): {} instrumented and {} plain frames. Instrumented frame {:.3f} ms median{}; {} passes, {} shaders timed.",
          is_d3d12 ? "D3D12" : "D3D11", instrumented_count, plain_count, instrumented_median,
          plain_totals.empty() ? "" : std::format(", plain {:.3f} ms", plain_median), reference.passes.size(), shader_stats.size()),
      .structured_content = std::move(output),
  });

}

[[nodiscard]] inline ToolResult HandleGpuTimingTool(const json& arguments, const ToolContext& context) {
  if (!context.resolve_device_index || !context.enqueue_request || !context.cancel_request || !context.collect_snapshot_commands) {
    throw std::runtime_error("GpuTimingToolContext is not initialized.");
  }
  const auto device_index = context.resolve_device_index(arguments);
  const auto frames = mcp_arguments::GetOptional<std::uint32_t>(arguments, "frames").value_or(DEFAULT_FRAMES);
  if (frames == 0u || frames > MAX_FRAMES) {
    throw std::runtime_error(std::format("frames must be between 1 and {}.", MAX_FRAMES));
  }
  const auto max_listed = mcp_arguments::GetOptional<std::uint32_t>(arguments, "maxListed").value_or(DEFAULT_MAX_LISTED);
  if (max_listed == 0u || max_listed > MAX_MAX_LISTED) {
    throw std::runtime_error(std::format("maxListed must be between 1 and {}.", MAX_MAX_LISTED));
  }
  const auto max_stamps = mcp_arguments::GetOptional<std::uint32_t>(arguments, "maxStamps");
  if (max_stamps.has_value() && (max_stamps.value() < MIN_MAX_STAMPS || max_stamps.value() > MAX_MAX_STAMPS)) {
    throw std::runtime_error(std::format("maxStamps must be between {} and {}.", MIN_MAX_STAMPS, MAX_MAX_STAMPS));
  }

  auto request = std::make_shared<Request>();
  request->frames = frames;
  request->max_listed = max_listed;
  request->max_stamps = max_stamps;
  request->stable_power_state = mcp_arguments::GetOptional<bool>(arguments, "stablePowerState").value_or(false);
  if (const auto error = context.enqueue_request(device_index, request); error.has_value()) {
    return ToolResult{.text = error.value(), .structured_content = json{{"error", error.value()}}, .is_error = true};
  }

  std::unique_lock request_lock(request->mutex);
  if (!request->cv.wait_for(request_lock, START_TIMEOUT, [&request] { return request->completed || request->started; })) {
    request_lock.unlock();
    context.cancel_request(device_index, request);
    const auto error = std::string("Timed out waiting for the game to present a frame (paused, minimised or not rendering on this device).");
    return ToolResult{.text = error, .structured_content = json{{"error", error}}, .is_error = true};
  }
  if (!request->cv.wait_for(request_lock, TOOL_TIMEOUT, [&request] { return request->completed; })) {
    request_lock.unlock();
    context.cancel_request(device_index, request);
    const auto error = std::string("Timed out: the game stopped presenting frames on the device the timing runs on.");
    return ToolResult{.text = error, .structured_content = json{{"error", error}}, .is_error = true};
  }
  if (!request->result.has_value()) {
    return ToolResult{.text = "The GPU timing run completed without a result.", .structured_content = json{{"error", "The GPU timing run completed without a result."}}, .is_error = true};
  }
  auto result = request->result.value();
  const auto signatures = request->pass_signatures;
  request_lock.unlock();
  if (result.is_error || !result.structured_content.is_object()) return result;

  // The timed passes are matched to the latest snapshot's runs (draws grouped by render target, dispatches by shader) by
  // sequence and target sizes; matched passes get the snapshot draw range. A snapshot of another frame layout leaves them
  // unmatched, with the reason.
  struct SnapshotRun {
    PassSignature signature;
    std::vector<std::uint64_t> key;
    std::size_t first_draw = 0u;
    std::size_t last_draw = 0u;
  };
  std::vector<SnapshotRun> snapshot_runs;
  const auto snapshot = context.collect_snapshot_commands(device_index);
  for (const auto& command : snapshot.commands) {
    if (command.kind == frame_overview::CommandKind::COPY || command.order_unknown) continue;
    const bool is_draw = (command.kind == frame_overview::CommandKind::DRAW);
    const auto kind = (is_draw ? RunKind::DRAW : RunKind::DISPATCH);
    std::vector<std::uint64_t> key;
    if (is_draw) {
      for (const auto& target : command.targets) {
        key.push_back(target.resource_handle);
      }
    } else {
      key.push_back(command.shader_hash);
    }
    if (!snapshot_runs.empty() && snapshot_runs.back().signature.kind == kind && snapshot_runs.back().key == key) {
      snapshot_runs.back().last_draw = command.draw_index;
      continue;
    }
    SnapshotRun run = {
        .signature = {.kind = kind, .dispatch_shader_hash = (is_draw ? 0u : command.shader_hash)},
        .key = std::move(key),
        .first_draw = command.draw_index,
        .last_draw = command.draw_index,
    };
    if (is_draw) {
      for (const auto& target : command.targets) {
        run.signature.target_sizes.emplace_back(target.width, target.height);
      }
    }
    snapshot_runs.push_back(std::move(run));
  }

  std::size_t first_mismatch = 0u;
  while (first_mismatch < snapshot_runs.size() && first_mismatch < signatures.size()) {
    const auto& snapshot_signature = snapshot_runs[first_mismatch].signature;
    const auto& timed_signature = signatures[first_mismatch];
    if (snapshot_signature.kind != timed_signature.kind
        || snapshot_signature.dispatch_shader_hash != timed_signature.dispatch_shader_hash
        || snapshot_signature.target_sizes != timed_signature.target_sizes) break;
    ++first_mismatch;
  }
  const bool matched = (!snapshot_runs.empty() && first_mismatch == snapshot_runs.size() && first_mismatch == signatures.size());
  json link = {{"matched", matched}, {"snapshotPassCount", snapshot_runs.size()}, {"timedPassCount", signatures.size()}};
  auto& content = result.structured_content;
  if (matched) {
    for (auto& entry : content["passes"]) {
      const auto& run = snapshot_runs[entry["pass"].get<std::size_t>()];
      entry["snapshotFirstDraw"] = run.first_draw;
      entry["snapshotLastDraw"] = run.last_draw;
    }
    result.text += " Passes matched to the latest snapshot (snapshotFirstDraw/snapshotLastDraw).";
  } else {
    link["firstMismatchingPass"] = first_mismatch;
    link["reason"] = (snapshot_runs.empty() ? "No snapshot is available (devkit_queue_snapshot first)."
                                            : "The snapshot's pass sequence differs from the timed frame's: another scene, passes that merge differently, or work the timing leaves out (lists on other queues such as async compute, lists already recording when the run started; see untimed).");
    result.text += std::format(" Snapshot not matched: {}", link["reason"].get<std::string>());
  }
  content["snapshotLink"] = std::move(link);
  return result;
}

}  // namespace renodx::addons::devkit::mcp::gpu_timing
