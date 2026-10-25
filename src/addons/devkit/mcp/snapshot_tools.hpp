/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../formatting.hpp"
#include "../../../utils/mcp/types.hpp"
#include "../../../utils/mcp/arguments.hpp"
#include "../../../utils/build_info.hpp"
#include "device_summary.hpp"
#include "device_selection.hpp"
#include "draw_summary.hpp"
#include "frame_overview.hpp"
#include "shader_summary.hpp"
#include "snapshot_summary.hpp"
#include "tool_catalog.hpp"

namespace renodx::addons::devkit::mcp::snapshot_tools {

using json = renodx::utils::mcp::json;
using ToolResult = renodx::utils::mcp::ToolResult;
namespace mcp_arguments = renodx::utils::mcp::arguments;

struct SnapshotCompletion {
  // Tells a device from the one that takes its index after it is destroyed.
  std::uintptr_t device_handle = 0u;
  std::uint64_t completed_snapshot_id = 0u;
  bool queued = false;
  bool active = false;
};

struct ToolContext {
  std::function<std::size_t()> get_device_count;
  std::function<std::uint32_t()> get_selected_device_index;
  std::function<void(std::uint32_t)> set_selected_device_index;
  std::function<std::string()> get_pipe_name;
  std::function<bool()> is_connected;
  std::function<bool()> has_active_snapshot;
  std::function<bool()> has_queued_snapshot;
  std::function<bool(std::uint32_t)> is_snapshot_active;
  std::function<bool(std::uint32_t)> is_snapshot_queued;
  std::function<std::optional<SnapshotCompletion>(std::uint32_t device_index)> get_snapshot_completion;
  std::function<device_summary::DeviceSummary(std::uint32_t index, bool is_selected)> build_device_summary;
  std::function<std::vector<shader_summary::TrackedShaderSummary>(
      std::uint32_t device_index,
      const std::optional<std::string>& stage_filter)>
      list_shaders;
  std::function<std::vector<draw_summary::DrawSummary>(std::uint32_t device_index)> list_draws;
  std::function<draw_summary::DrawSummary(std::uint32_t device_index, std::uint32_t draw_index, bool include_constant_values)> get_draw;
  std::function<snapshot_summary::SnapshotSummary(std::uint32_t device_index)> build_snapshot_summary;
  std::function<ToolResult(std::uint32_t device_index)> queue_snapshot;
  std::function<shader_summary::ReplacementListSummary(std::uint32_t device_index)> list_replacements;
  std::function<frame_overview::Input(std::uint32_t device_index)> collect_frame_overview;
};

namespace internal {

[[nodiscard]] inline std::string ToLowerAscii(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const auto character : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return lowered;
}

[[nodiscard]] inline bool EqualsInsensitive(std::string_view left, std::string_view right) {
  return ToLowerAscii(left) == ToLowerAscii(right);
}

struct DrawFilters {
  std::optional<std::string> method = std::nullopt;
  std::optional<std::string> shader_hash = std::nullopt;
  std::optional<std::string> active_shader_stage = std::nullopt;
  std::optional<std::uint32_t> min_srv_count = std::nullopt;
  std::optional<std::uint32_t> min_uav_count = std::nullopt;
  std::optional<std::uint32_t> min_constant_count = std::nullopt;
  std::optional<std::uint32_t> min_render_target_count = std::nullopt;
  std::optional<std::uint32_t> min_pipeline_count = std::nullopt;
};

[[nodiscard]] inline DrawFilters ParseDrawFilters(const json& arguments) {
  const auto parse_min_filter = [&arguments](std::string_view key) {
    const auto value = mcp_arguments::GetOptional<std::uint32_t>(arguments, key);
    if (!value.has_value()) return value;
    return std::optional<std::uint32_t>(mcp_arguments::ValidateRange(
        key,
        value.value(),
        0u,
        (std::numeric_limits<std::uint32_t>::max)()));
  };

  return DrawFilters{
      .method = mcp_arguments::GetOptional<std::string>(arguments, "method"),
      .shader_hash = mcp_arguments::GetOptional<std::string>(arguments, "shaderHash"),
      .active_shader_stage = mcp_arguments::GetOptional<std::string>(arguments, "activeShaderStage"),
      .min_srv_count = parse_min_filter("minSrvCount"),
      .min_uav_count = parse_min_filter("minUavCount"),
      .min_constant_count = parse_min_filter("minConstantCount"),
      .min_render_target_count = parse_min_filter("minRenderTargetCount"),
      .min_pipeline_count = parse_min_filter("minPipelineCount"),
  };
}

[[nodiscard]] inline bool MatchesDrawFilters(const draw_summary::DrawSummary& draw, const DrawFilters& filters) {
  if (filters.method.has_value() && !EqualsInsensitive(draw.method, filters.method.value())) {
    return false;
  }

  if (filters.shader_hash.has_value()) {
    const auto has_shader_hash = std::any_of(
        draw.shader_hashes.begin(),
        draw.shader_hashes.end(),
        [&filters](const std::string& shader_hash) {
          return EqualsInsensitive(shader_hash, filters.shader_hash.value());
        });
    if (!has_shader_hash) {
      return false;
    }
  }

  if (filters.active_shader_stage.has_value()) {
    if (!draw.active_shader_stage.has_value()) {
      return false;
    }
    if (!EqualsInsensitive(draw.active_shader_stage.value(), filters.active_shader_stage.value())) {
      return false;
    }
  }

  if (filters.min_srv_count.has_value() && draw.srv_count < filters.min_srv_count.value()) {
    return false;
  }
  if (filters.min_uav_count.has_value() && draw.uav_count < filters.min_uav_count.value()) {
    return false;
  }
  if (filters.min_constant_count.has_value() && draw.constant_count < filters.min_constant_count.value()) {
    return false;
  }
  if (filters.min_render_target_count.has_value() && draw.render_target_count < filters.min_render_target_count.value()) {
    return false;
  }
  if (filters.min_pipeline_count.has_value() && draw.pipeline_count < filters.min_pipeline_count.value()) {
    return false;
  }

  return true;
}

[[nodiscard]] inline std::uint32_t ResolveDeviceIndex(const json& arguments, const ToolContext& context) {
  if (!context.get_device_count || !context.get_selected_device_index) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto device_count = context.get_device_count();
  const auto selected_index = device_count == 0u
                                  ? 0u
                                  : std::min<std::uint32_t>(
                                        context.get_selected_device_index(),
                                        static_cast<std::uint32_t>(device_count - 1u));
  return device_selection::ResolveRequestedDeviceIndex(arguments, device_count, selected_index);
}

}  // namespace internal

inline ToolResult HandleStatusTool([[maybe_unused]] const json& arguments, const ToolContext& context) {
  if (!context.get_device_count || !context.get_selected_device_index || !context.build_device_summary) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto device_count = context.get_device_count();
  const auto selected_index = device_count == 0u
                                  ? 0u
                                  : std::min<std::uint32_t>(
                                        context.get_selected_device_index(),
                                        static_cast<std::uint32_t>(device_count - 1u));

  std::vector<device_summary::DeviceSummary> devices;
  devices.reserve(device_count);
  for (std::uint32_t index = 0u; index < device_count; ++index) {
    devices.emplace_back(context.build_device_summary(index, index == selected_index));
  }

  json result = {
      {"pipeName", context.get_pipe_name ? json(context.get_pipe_name()) : json(nullptr)},
      {"serverInfo", {
                 {"name", "renodx-devkit"},
                 {"title", "RenoDX DevKit"},
                 {"version", std::string(renodx::build_info::kBuildVersion)},
               }},
      {"connected", context.is_connected ? context.is_connected() : false},
      {"deviceCount", device_count},
      {"devices", devices},
      {"selectedDeviceIndex", device_count == 0u ? json(nullptr) : json(selected_index)},
      {"snapshot", {
                       {"active", context.has_active_snapshot ? context.has_active_snapshot() : false},
                       {"queued", context.has_queued_snapshot ? context.has_queued_snapshot() : false},
                   }},
  };

  if (renodx::build_info::HasKnownBuildTimestamp()) {
    result["serverInfo"]["buildTimestampUtc"] = renodx::build_info::kBuildTimestampUtc;
  }
  if (renodx::build_info::HasKnownSourceDateEpoch()) {
    result["serverInfo"]["sourceDateEpoch"] = renodx::build_info::kSourceDateEpoch;
  }

  if (device_count == 0u) {
    return ToolResult{
        .text = "No devices are currently tracked by devkit.",
        .structured_content = result,
    };
  }

  const auto& selected_device = devices[selected_index];
  auto text = std::format(
      "Tracking {} device(s). Selected device #{} ({}) has {} tracked shaders and {} captured draws.",
      device_count,
      selected_index,
      selected_device.api,
      selected_device.tracked_shaders,
      selected_device.captured_draws);
  return ToolResult{
      .text = text,
      .structured_content = result,
  };
}

inline ToolResult HandleSelectDeviceTool(const json& arguments, const ToolContext& context) {
  if (!context.set_selected_device_index || !context.build_device_summary) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }
  if (!arguments.contains("deviceIndex")) {
    throw std::runtime_error("deviceIndex is required.");
  }

  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  context.set_selected_device_index(device_index);
  const auto result = context.build_device_summary(device_index, true);
  auto text = std::format(
      "Selected device #{} ({}) for subsequent devkit tools.",
      device_index,
      result.api);
  return ToolResult{
      .text = text,
      .structured_content = result,
  };
}

inline ToolResult HandleListShadersTool(const json& arguments, const ToolContext& context) {
  if (!context.list_shaders) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto limit = mcp_arguments::ValidateRange(
      "limit",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "limit").value_or(200u),
      1u,
      5000u);
  const auto offset = mcp_arguments::ValidateRange(
      "offset",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "offset").value_or(0u),
      0u,
      (std::numeric_limits<std::uint32_t>::max)());

  const auto stage_filter = mcp_arguments::GetOptional<std::string>(arguments, "stage");

  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  auto shaders = context.list_shaders(device_index, stage_filter);
  const auto total_count = shaders.size();
  const auto clamped_offset = std::min<std::size_t>(offset, total_count);
  const auto end_index = std::min<std::size_t>(clamped_offset + limit, total_count);

  std::vector<shader_summary::TrackedShaderSummary> paged_shaders;
  paged_shaders.reserve(end_index - clamped_offset);
  for (std::size_t index = clamped_offset; index < end_index; ++index) {
    paged_shaders.push_back(shaders[index]);
  }

  json result = {
      {"deviceIndex", device_index},
      {"totalTracked", total_count},
      {"offset", clamped_offset},
      {"limit", limit},
      {"returned", paged_shaders.size()},
      {"shaders", paged_shaders},
  };
  if (stage_filter.has_value()) {
    result["stage"] = stage_filter.value();
  }

  auto text = std::format(
      "Returned {} shader(s) from device #{} (offset {} of {}).",
      paged_shaders.size(),
      device_index,
      clamped_offset,
      total_count);
  return ToolResult{
      .text = text,
      .structured_content = result,
  };
}

inline ToolResult HandleListDrawsTool(const json& arguments, const ToolContext& context) {
  if (!context.list_draws) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto limit = mcp_arguments::ValidateRange(
      "limit",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "limit").value_or(100u),
      1u,
      1000u);
  const auto offset = mcp_arguments::ValidateRange(
      "offset",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "offset").value_or(0u),
      0u,
      (std::numeric_limits<std::uint32_t>::max)());
  const auto filters = internal::ParseDrawFilters(arguments);
  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  auto draws = context.list_draws(device_index);
  const auto total_draws = draws.size();

  std::vector<draw_summary::DrawSummary> filtered_draws;
  filtered_draws.reserve(draws.size());
  std::copy_if(
      draws.begin(),
      draws.end(),
      std::back_inserter(filtered_draws),
      [&filters](const draw_summary::DrawSummary& draw) {
        return internal::MatchesDrawFilters(draw, filters);
      });

  const auto total_filtered_draws = filtered_draws.size();
  const auto clamped_offset = std::min<std::size_t>(offset, total_filtered_draws);
  const auto end_index = std::min<std::size_t>(clamped_offset + limit, total_filtered_draws);

  std::vector<draw_summary::DrawSummary> paged_draws;
  paged_draws.reserve(end_index - clamped_offset);
  for (std::size_t index = clamped_offset; index < end_index; ++index) {
    paged_draws.push_back(filtered_draws[index]);
  }

  json result = {
      {"deviceIndex", device_index},
      {"totalDraws", total_draws},
      {"totalFilteredDraws", total_filtered_draws},
      {"offset", clamped_offset},
      {"limit", limit},
      {"returned", paged_draws.size()},
      {"draws", paged_draws},
  };
  if (filters.method.has_value()
      || filters.shader_hash.has_value()
      || filters.active_shader_stage.has_value()
      || filters.min_srv_count.has_value()
      || filters.min_uav_count.has_value()
      || filters.min_constant_count.has_value()
      || filters.min_render_target_count.has_value()
      || filters.min_pipeline_count.has_value()) {
    json filters_json = json::object();
    if (filters.method.has_value()) {
      filters_json["method"] = filters.method.value();
    }
    if (filters.shader_hash.has_value()) {
      filters_json["shaderHash"] = filters.shader_hash.value();
    }
    if (filters.active_shader_stage.has_value()) {
      filters_json["activeShaderStage"] = filters.active_shader_stage.value();
    }
    if (filters.min_srv_count.has_value()) {
      filters_json["minSrvCount"] = filters.min_srv_count.value();
    }
    if (filters.min_uav_count.has_value()) {
      filters_json["minUavCount"] = filters.min_uav_count.value();
    }
    if (filters.min_constant_count.has_value()) {
      filters_json["minConstantCount"] = filters.min_constant_count.value();
    }
    if (filters.min_render_target_count.has_value()) {
      filters_json["minRenderTargetCount"] = filters.min_render_target_count.value();
    }
    if (filters.min_pipeline_count.has_value()) {
      filters_json["minPipelineCount"] = filters.min_pipeline_count.value();
    }
    result["filters"] = filters_json;
  }

  auto text = std::format(
      "Returned {} draw(s) from device #{} after filtering (offset {} of {}; snapshot had {}).",
      paged_draws.size(),
      device_index,
      clamped_offset,
      total_filtered_draws,
      total_draws);
  return ToolResult{
      .text = text,
      .structured_content = result,
  };
}

inline ToolResult HandleGetDrawTool(const json& arguments, const ToolContext& context) {
  if (!context.get_draw) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }
  const auto draw_index = mcp_arguments::GetRequired<std::uint32_t>(arguments, "drawIndex");
  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  const auto draw = context.get_draw(
      device_index,
      draw_index,
      mcp_arguments::GetOptional<bool>(arguments, "includeConstantValues").value_or(false));
  json result = draw;
  result["deviceIndex"] = device_index;

  auto text = std::format(
      "Returned draw #{} from device #{} with {} render target(s), {} SRV bind(s), and {} UAV bind(s).",
      draw_index,
      device_index,
      draw.render_target_count,
      draw.srv_count,
      draw.uav_count);
  return ToolResult{
      .text = text,
      .structured_content = result,
  };
}

inline ToolResult HandleSnapshotSummaryTool(const json& arguments, const ToolContext& context) {
  if (!context.build_snapshot_summary) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  const auto result = context.build_snapshot_summary(device_index);
  auto text = std::format(
      "Device #{} currently has {} captured draw(s) and {} tracked shader(s) in the active snapshot cache.",
      device_index,
      result.captured_draws,
      result.tracked_shaders);
  return ToolResult{
      .text = text,
      .structured_content = result,
  };
}

inline ToolResult HandleQueueSnapshotTool(const json& arguments, const ToolContext& context) {
  if (!context.queue_snapshot) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto wait_for_completion = mcp_arguments::GetOptional<bool>(arguments, "waitForCompletion").value_or(false);
  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  // Read before queueing, so a capture that finishes quickly is not missed. A capture that was already running when
  // the call came finishes the wait too.
  const auto completion_before = (wait_for_completion ? context.get_snapshot_completion(device_index) : std::nullopt);
  auto queue_result = context.queue_snapshot(device_index);
  if (queue_result.is_error || !wait_for_completion) return queue_result;
  if (!completion_before.has_value()) {
    throw std::runtime_error(std::format("Device #{} is not available.", device_index));
  }

  const auto make_error = [](std::string error) {
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  };
  const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(tool_catalog::MAX_SNAPSHOT_COMPLETION_WAIT_SECONDS);
  while (true) {
    const auto completion = context.get_snapshot_completion(device_index);
    if (!completion.has_value() || completion->device_handle != completion_before->device_handle) {
      return make_error(std::format("Device #{} was destroyed or replaced while the snapshot was pending.", device_index));
    }
    if (completion->completed_snapshot_id != completion_before->completed_snapshot_id) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (completion->queued) {
        return make_error(std::format(
            "The snapshot of device #{} did not start within {} s: the game presented no frame (paused in the background, minimized or loading?). The request stays queued.",
            device_index,
            tool_catalog::MAX_SNAPSHOT_COMPLETION_WAIT_SECONDS));
      }
      if (completion->active) {
        return make_error(std::format(
            "The snapshot of device #{} was not finished within {} s; it keeps capturing. Check devkit_snapshot_summary later.",
            device_index,
            tool_catalog::MAX_SNAPSHOT_COMPLETION_WAIT_SECONDS));
      }
      // Seen when the request was dropped without a capture, e.g. a device destroyed and recreated at the same address.
      return make_error(std::format(
          "No snapshot of device #{} finished within {} s and none is queued or capturing; queue a new one.",
          device_index,
          tool_catalog::MAX_SNAPSHOT_COMPLETION_WAIT_SECONDS));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  // The summary of this device, not of the selected one, which the overlay can change during the wait.
  auto summary_arguments = arguments;
  summary_arguments["deviceIndex"] = device_index;
  return HandleSnapshotSummaryTool(summary_arguments, context);
}

inline ToolResult HandleListReplacementsTool(const json& arguments, const ToolContext& context) {
  if (!context.list_replacements) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto drawn_only = mcp_arguments::GetOptional<bool>(arguments, "drawnOnly").value_or(false);
  const auto limit = mcp_arguments::ValidateRange(
      "limit",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "limit").value_or(100u),
      1u,
      5000u);
  const auto offset = mcp_arguments::ValidateRange(
      "offset",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "offset").value_or(0u),
      0u,
      (std::numeric_limits<std::uint32_t>::max)());
  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  const auto list = context.list_replacements(device_index);
  const auto drawn_count = std::ranges::count_if(list.replacements, [](const shader_summary::ReplacementSummary& replacement) {
    return replacement.snapshot_draws != 0u;
  });
  // A generic mod registers hundreds of replacements, so the list is paged; drawnOnly keeps the ones the snapshot drew.
  std::vector<shader_summary::ReplacementSummary> filtered_replacements;
  for (const auto& replacement : list.replacements) {
    if (drawn_only && replacement.snapshot_draws == 0u) continue;
    filtered_replacements.push_back(replacement);
  }
  const auto clamped_offset = std::min<std::size_t>(offset, filtered_replacements.size());
  const auto end_index = std::min<std::size_t>(clamped_offset + limit, filtered_replacements.size());
  const std::vector<shader_summary::ReplacementSummary> paged_replacements(
      filtered_replacements.begin() + clamped_offset,
      filtered_replacements.begin() + end_index);
  const auto fully_replaced_count = std::ranges::count_if(list.replacements, [](const shader_summary::ReplacementSummary& replacement) {
    return replacement.snapshot_draws != 0u && replacement.snapshot_draws_replaced == replacement.snapshot_draws;
  });

  std::string text;
  if (list.snapshot_pending) {
    text = std::format(
        "Device #{} has {} registered shader replacement(s). A snapshot is queued or still capturing: call again once it has finished for draw counts.",
        device_index,
        list.replacements.size());
  } else if (list.captured_draws == 0u) {
    text = std::format(
        "Device #{} has {} registered shader replacement(s). There is no snapshot, so draw counts are 0: queue a snapshot to see which replaced shaders draw.",
        device_index,
        list.replacements.size());
  } else {
    text = std::format(
        "Device #{} has {} registered shader replacement(s); {} drew in the snapshot ({} captured draws), {} of them replaced in every draw.",
        device_index,
        list.replacements.size(),
        drawn_count,
        list.captured_draws,
        fully_replaced_count);
  }
  text += std::format(
      " Returned {} of {} ({}offset {}).",
      paged_replacements.size(),
      filtered_replacements.size(),
      (drawn_only ? "drawn only, " : ""),
      clamped_offset);
  return ToolResult{
      .text = std::move(text),
      .structured_content = json{
          {"deviceIndex", device_index},
          {"capturedDraws", list.captured_draws},
          {"snapshotPending", list.snapshot_pending},
          {"totalRegistered", list.replacements.size()},
          {"totalDrawn", drawn_count},
          {"totalFiltered", filtered_replacements.size()},
          {"drawnOnly", drawn_only},
          {"offset", clamped_offset},
          {"limit", limit},
          {"returned", paged_replacements.size()},
          {"replacements", paged_replacements},
      },
  };
}

// Groups the snapshot into passes (consecutive commands writing the same targets with the same blend state) and marks
// candidate tonemap, LUT, UI and output passes with the rule that matched. The rules follow the order the
// swapchain-resource-analysis and handle-sdr-tonemap-lut skills describe (scene HDR, tonemap, LUT, UI, output); they
// are hints to verify with devkit_get_draw, the shader and devkit_watch_region, never proof.
inline ToolResult HandleFrameOverviewTool(const json& arguments, const ToolContext& context) {
  if (!context.collect_frame_overview) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto max_passes = mcp_arguments::ValidateRange(
      "maxPasses",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "maxPasses").value_or(40u),
      1u,
      2000u);
  const auto max_resources = mcp_arguments::ValidateRange(
      "maxResources",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "maxResources").value_or(32u),
      0u,
      2000u);
  const auto save_baseline_path = mcp_arguments::GetOptional<std::string>(arguments, "saveBaseline");
  const auto compare_with_path = mcp_arguments::GetOptional<std::string>(arguments, "compareWith");
  for (const auto* path : {&save_baseline_path, &compare_with_path}) {
    if (path->has_value() && !std::filesystem::path(path->value()).is_absolute()) {
      throw std::runtime_error("saveBaseline and compareWith must be absolute paths.");
    }
  }
  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  const auto input = context.collect_frame_overview(device_index);

  using frame_overview::CommandKind;
  using frame_overview::TextureInfo;
  constexpr std::size_t MAX_LISTED = 8u;
  constexpr std::size_t MAX_LUT_TEXTURES = 16u;
  // compareWith: the alignment table holds one word per pass pair (4M pairs = 16 MB, e.g. 2000 x 2000 passes).
  constexpr std::size_t MAX_COMPARED_PASS_PAIRS = 4'000'000u;
  constexpr std::size_t MAX_COMPARED_SHADERS = 4u;
  constexpr std::size_t MAX_COMPARED_PASSES_LISTED = 32u;

  const auto is_float_format = [](reshade::api::format format) {
    switch (reshade::api::format_to_default_typed(format)) {
      case reshade::api::format::r16g16b16a16_float:
      case reshade::api::format::r11g11b10_float:
      case reshade::api::format::r32g32b32a32_float:
      case reshade::api::format::r32g32b32_float:
      case reshade::api::format::r16g16_float:
      case reshade::api::format::r9g9b9e5:
        return true;
      default:
        return false;
    }
  };
  const auto is_sdr_format = [](reshade::api::format format) {
    switch (reshade::api::format_to_default_typed(format)) {
      case reshade::api::format::r8g8b8a8_unorm:
      case reshade::api::format::r8g8b8a8_unorm_srgb:
      case reshade::api::format::b8g8r8a8_unorm:
      case reshade::api::format::b8g8r8a8_unorm_srgb:
      case reshade::api::format::b8g8r8x8_unorm:
      case reshade::api::format::b8g8r8x8_unorm_srgb:
      case reshade::api::format::r10g10b10a2_unorm:
      case reshade::api::format::b10g10r10a2_unorm:
        return true;
      default:
        return false;
    }
  };
  // The format the GPU renders: a mod's active clone replaces the game's texture.
  const auto rendered_format = [](const TextureInfo& texture) {
    return (texture.clone_active && texture.clone_format.has_value() ? texture.clone_format.value() : texture.format);
  };
  // LUT geometry src/shaders/lut.hlsl samples: a 3D cube N x N x N, or a 2D strip N*N x N. The N values are the usual
  // LUT sizes, not a rule of that file; noise volumes and ramp strips of the same shape also match.
  const auto is_lut_shaped = [](const TextureInfo& texture) {
    if (texture.type == reshade::api::resource_type::texture_3d) {
      return texture.width == texture.height && texture.height == texture.depth_or_layers && texture.width >= 16u && texture.width <= 65u;
    }
    if (texture.type != reshade::api::resource_type::texture_2d) return false;
    switch (texture.height) {
      case 16u:
      case 32u:
      case 33u:
      case 48u:
      case 64u:
        return texture.width == texture.height * texture.height;
      default:
        return false;
    }
  };

  // Snapshots record no viewports or vertex counts. A "large" target covers at least a quarter of the swap chain's back
  // buffer (without a swap chain, of the largest target written), so passes at a lower render resolution still count.
  std::uint32_t screen_width = 0u;
  std::uint32_t screen_height = 0u;
  if (input.swapchain_back_buffer.has_value()) {
    screen_width = input.swapchain_back_buffer->texture.width;
    screen_height = input.swapchain_back_buffer->texture.height;
  } else {
    for (const auto& command : input.commands) {
      for (const auto& target : command.targets) {
        if (static_cast<std::uint64_t>(target.width) * target.height > static_cast<std::uint64_t>(screen_width) * screen_height) {
          screen_width = target.width;
          screen_height = target.height;
        }
      }
    }
  }
  const auto screen_area = static_cast<std::uint64_t>(screen_width) * screen_height;
  const auto is_large = [&](const TextureInfo& texture) {
    return screen_area != 0u && static_cast<std::uint64_t>(texture.width) * texture.height * 4u >= screen_area;
  };

  struct Pass {
    CommandKind kind = CommandKind::DRAW;
    std::size_t first_draw = 0u;
    std::size_t last_draw = 0u;
    std::size_t command_count = 0u;
    std::vector<TextureInfo> targets;
    std::vector<TextureInfo> inputs;
    std::vector<std::uint32_t> shader_hashes;
    // The first draw of each of shader_hashes, at the same index.
    std::vector<std::size_t> shader_first_draws;
    bool blend_known = false;
    bool blend_enabled = false;
    bool order_unknown = false;
    json hints = json::array();
  };
  struct ResourceUse {
    TextureInfo texture;
    std::vector<std::size_t> writer_passes;
    std::size_t reader_command_count = 0u;
    std::vector<std::size_t> reader_draws;
  };

  // Commands are in frame order, so pass indexes, writer lists and reader lists come out in frame order too.
  std::vector<Pass> passes;
  std::map<std::uint64_t, ResourceUse> resources;
  for (const auto& command : input.commands) {
    auto starts_pass = (passes.empty()
                        || command.kind == CommandKind::COPY
                        || passes.back().kind != command.kind
                        || passes.back().blend_known != command.blend_known
                        || passes.back().blend_enabled != command.blend_enabled
                        || passes.back().order_unknown != command.order_unknown
                        || passes.back().targets.size() != command.targets.size());
    for (std::size_t target_index = 0u; !starts_pass && target_index < command.targets.size(); ++target_index) {
      starts_pass = passes.back().targets[target_index].resource_handle != command.targets[target_index].resource_handle;
    }
    if (starts_pass) {
      passes.push_back(Pass{
          .kind = command.kind,
          .first_draw = command.draw_index,
          .targets = command.targets,
          .blend_known = command.blend_known,
          .blend_enabled = command.blend_enabled,
          .order_unknown = command.order_unknown,
      });
      for (const auto& texture : command.targets) {
        auto& use = resources.try_emplace(texture.resource_handle, ResourceUse{.texture = texture}).first->second;
        use.writer_passes.push_back(passes.size() - 1u);
      }
    }
    auto& pass = passes.back();
    pass.last_draw = command.draw_index;
    ++pass.command_count;
    if (command.shader_hash != 0u && std::ranges::find(pass.shader_hashes, command.shader_hash) == pass.shader_hashes.end()) {
      pass.shader_hashes.push_back(command.shader_hash);
      pass.shader_first_draws.push_back(command.draw_index);
    }
    for (const auto& texture : command.inputs) {
      if (std::ranges::none_of(pass.inputs, [&](const TextureInfo& known) { return known.resource_handle == texture.resource_handle; })) {
        pass.inputs.push_back(texture);
      }
      auto& use = resources.try_emplace(texture.resource_handle, ResourceUse{.texture = texture}).first->second;
      ++use.reader_command_count;
      if (use.reader_draws.size() < MAX_LISTED && std::ranges::find(use.reader_draws, command.draw_index) == use.reader_draws.end()) {
        use.reader_draws.push_back(command.draw_index);
      }
    }
  }

  // Candidates, in frame order.
  std::vector<std::size_t> tonemap_passes;
  std::vector<std::size_t> final_passes_from_mod_upgrade;
  std::vector<std::size_t> lut_builder_passes;
  std::vector<std::size_t> lut_reader_passes;
  for (std::size_t pass_index = 0u; pass_index < passes.size(); ++pass_index) {
    auto& pass = passes[pass_index];
    if (pass.kind == CommandKind::COPY || pass.targets.empty()) continue;

    const auto lut_target = std::ranges::find_if(pass.targets, is_lut_shaped);
    if (lut_target != pass.targets.end()) {
      pass.hints.push_back({
          {"type", "lutBuilder"},
          {"rule", std::format("writes a LUT-shaped {} {}x{}", formatting::StreamToString(lut_target->type), lut_target->width, lut_target->height)},
      });
      lut_builder_passes.push_back(pass_index);
    }
    // Only post-processing passes count as LUT readers: scene, shadow and probe draws also sample LUT-shaped volumes
    // (noise, lighting), and their targets are square. A large target that is not square passes, so pillarboxed and
    // letterboxed post chains still count. Without a swap chain the screen size is a guess, so only the size is checked.
    // Non-square shadow atlases (e.g. 4096x2048) still pass.
    const auto lut_input = std::ranges::find_if(pass.inputs, is_lut_shaped);
    const auto& target = pass.targets.front();
    const bool square_target = input.swapchain_back_buffer.has_value()
                               && screen_width != screen_height
                               && std::abs(static_cast<double>(target.width) - target.height) <= target.height * 0.05;
    if (lut_input != pass.inputs.end() && is_large(target) && !square_target) {
      pass.hints.push_back({
          {"type", "lutReader"},
          {"rule", std::format(
                       "reads a LUT-shaped {} {} {}x{}",
                       formatting::StreamToString(lut_input->type),
                       formatting::StreamToString(rendered_format(*lut_input)),
                       lut_input->width,
                       lut_input->height)},
      });
      lut_reader_passes.push_back(pass_index);
    }

    if (pass.blend_known && pass.blend_enabled) continue;
    if (!is_large(target)) continue;
    const auto float_input = std::ranges::find_if(pass.inputs, [&](const TextureInfo& texture) {
      return is_float_format(rendered_format(texture)) && texture.width == target.width && texture.height == target.height;
    });
    if (float_input == pass.inputs.end()) continue;
    std::string rule;
    if (is_sdr_format(rendered_format(target))) {
      rule = std::format(
          "reads {} and writes SDR {} of the same size",
          formatting::StreamToString(rendered_format(*float_input)),
          formatting::StreamToString(rendered_format(target)));
    } else if (is_sdr_format(target.original_format)) {
      rule = std::format(
          "reads {} and writes a target of the same size the game created as SDR {} (rendered as {} by the mod)",
          formatting::StreamToString(rendered_format(*float_input)),
          formatting::StreamToString(target.original_format),
          formatting::StreamToString(rendered_format(target)));
    } else if (target.is_swapchain && is_float_format(rendered_format(target))) {
      // A mod that upgrades the swap chain when the game creates it leaves no SDR format to compare with, and an
      // HDR-native game presents a float swap chain too; the pass that fills it from the scene is still the candidate.
      rule = std::format(
          "reads {} and writes a target of the same size that is float ({}) from its creation, as after a mod's swap-chain upgrade or in an HDR-native game",
          formatting::StreamToString(rendered_format(*float_input)),
          formatting::StreamToString(rendered_format(target)));
    } else {
      continue;
    }
    if (pass.kind == CommandKind::DISPATCH) {
      rule += " (compute)";
    } else if (pass.blend_known) {
      rule += " without blending";
    } else {
      rule += " (blend state not recorded for this pipeline)";
    }
    // Swap-chain writers stay candidates (games often tonemap straight into the back buffer), labelled: an r10g10b10a2
    // swap chain can also be an HDR10 output. A swap-chain writer whose float input the game created as non-float (float only
    // through a mod's clone or in-place upgrade) is the game's final pass (AA, upscale, composite) reading an intermediate
    // the mod upgraded; the snapshot never records passes an add-on draws through the ReShade API.
    const bool float_only_through_mod = !is_float_format(float_input->original_format);
    if (target.is_swapchain) {
      rule += "; writes the swap chain, so it may be the output pass instead";
      if (float_only_through_mod) {
        rule += "; its float input is a non-float texture a mod upgraded to float, so this is likely the game's final AA/upscale/composite pass after the mod's upgrade";
        final_passes_from_mod_upgrade.push_back(pass_index);
      }
    } else if (float_only_through_mod) {
      rule += "; its float input is float only through a mod's upgrade or clone";
    }
    pass.hints.push_back({{"type", "tonemap"}, {"rule", std::move(rule)}});
    tonemap_passes.push_back(pass_index);
  }

  // UI: blended draws onto large SDR (as the game created them) or swap-chain targets after the tonemap. The scan starts
  // after the last tonemap candidate that is not such a final pass from a mod-upgraded intermediate: UI can be drawn onto the
  // intermediate before that pass, while a vanilla game's tonemap into the back buffer stays the anchor. When every
  // candidate is such a final pass (e.g. the mod tonemaps in its swap-chain proxy pass, which an add-on draws through the
  // ReShade API and a snapshot never records), the scan starts after the last one, so only UI drawn after it is found.
  std::optional<std::size_t> ui_anchor = std::nullopt;
  if (const auto non_final_tonemap = std::ranges::find_if(
          tonemap_passes.rbegin(),
          tonemap_passes.rend(),
          [&](std::size_t pass_index) { return std::ranges::find(final_passes_from_mod_upgrade, pass_index) == final_passes_from_mod_upgrade.end(); });
      non_final_tonemap != tonemap_passes.rend()) {
    ui_anchor = *non_final_tonemap;
  } else if (!tonemap_passes.empty()) {
    ui_anchor = tonemap_passes.back();
  }
  std::vector<std::size_t> ui_passes;
  if (ui_anchor.has_value()) {
    for (auto pass_index = *ui_anchor + 1u; pass_index < passes.size(); ++pass_index) {
      auto& pass = passes[pass_index];
      if (pass.kind != CommandKind::DRAW || !pass.blend_known || !pass.blend_enabled || pass.order_unknown || pass.targets.empty()) continue;
      const auto& target = pass.targets.front();
      if (!is_large(target) || !(target.is_swapchain || is_sdr_format(target.original_format))) continue;
      std::string rule = "blended draws onto a large SDR or swap-chain target after the last tonemap candidate that is not a final pass from a mod-upgraded intermediate (or after the last candidate when all are)";
      // UI samples its own art (atlases, fonts); a blended pass that only reads what the frame wrote before it is more
      // likely a composite. Copies count as writes.
      if (!pass.inputs.empty() && std::ranges::all_of(pass.inputs, [&](const TextureInfo& texture) {
            const auto found = resources.find(texture.resource_handle);
            return found != resources.end() && !found->second.writer_passes.empty() && found->second.writer_passes.front() < pass_index;
          })) {
        rule += "; every input was written earlier in this frame, so this may be a post-processing composite (bloom, glow, blur) or a composite of UI drawn into an earlier target, rather than UI drawn from its own art";
      }
      pass.hints.push_back({{"type", "ui"}, {"rule", std::move(rule)}});
      ui_passes.push_back(pass_index);
    }
  }

  // Output: the last pass in known order that writes a swap-chain back buffer, by draw, dispatch or copy.
  std::optional<std::size_t> output_pass = std::nullopt;
  for (auto pass_index = passes.size(); pass_index-- > 0u;) {
    auto& pass = passes[pass_index];
    if (pass.order_unknown || std::ranges::none_of(pass.targets, [](const TextureInfo& texture) { return texture.is_swapchain; })) continue;
    output_pass = pass_index;
    pass.hints.push_back({{"type", "output"}, {"rule", "last pass of the frame that writes a swap-chain back buffer"}});
    break;
  }

  // Shared shaders: a candidate pass's shader that also draws in a pass of another role (for example UI and scene or
  // post-processing). A replacement of that hash changes every one of those passes, so its callback has to tell them apart.
  const auto pass_role = [&](std::size_t pass_index) -> std::string_view {
    if (std::ranges::find(ui_passes, pass_index) != ui_passes.end()) return "ui";
    if (std::ranges::find(tonemap_passes, pass_index) != tonemap_passes.end()) return "tonemap";
    if (output_pass == pass_index) return "output";
    return "other";
  };
  struct ShaderPassUse {
    std::size_t pass_index = 0u;
    std::size_t first_draw = 0u;
  };
  std::map<std::uint32_t, std::vector<ShaderPassUse>> uses_by_shader;
  for (std::size_t pass_index = 0u; pass_index < passes.size(); ++pass_index) {
    const auto& pass = passes[pass_index];
    for (std::size_t shader_index = 0u; shader_index < pass.shader_hashes.size(); ++shader_index) {
      uses_by_shader[pass.shader_hashes[shader_index]].push_back({
          .pass_index = pass_index,
          .first_draw = pass.shader_first_draws[shader_index],
      });
    }
  }
  constexpr std::size_t MAX_SHARED_SHADERS = 16u;
  struct SharedShader {
    std::uint32_t shader_hash = 0u;
    std::size_t first_candidate_pass = 0u;
    // The first pass of each role, so the listed passes show why the shader is shared, then the rest in frame order.
    std::vector<ShaderPassUse> uses;
  };
  std::vector<SharedShader> shared_shader_list;
  for (const auto& [shader_hash, uses] : uses_by_shader) {
    const auto first_candidate = std::ranges::find_if(uses, [&](const ShaderPassUse& use) { return pass_role(use.pass_index) != "other"; });
    if (first_candidate == uses.end()) continue;
    const auto first_role = pass_role(uses.front().pass_index);
    if (std::ranges::none_of(uses, [&](const ShaderPassUse& use) { return pass_role(use.pass_index) != first_role; })) continue;
    auto& shared_shader = shared_shader_list.emplace_back(SharedShader{
        .shader_hash = shader_hash,
        .first_candidate_pass = first_candidate->pass_index,
    });
    for (const auto& use : uses) {
      if (std::ranges::none_of(shared_shader.uses, [&](const ShaderPassUse& listed) { return pass_role(listed.pass_index) == pass_role(use.pass_index); })) {
        shared_shader.uses.push_back(use);
      }
    }
    for (const auto& use : uses) {
      if (std::ranges::none_of(shared_shader.uses, [&](const ShaderPassUse& listed) { return listed.pass_index == use.pass_index; })) {
        shared_shader.uses.push_back(use);
      }
    }
  }
  // Latest first candidate pass first, since the end of the frame (output, UI) matters most when the list is cut.
  std::ranges::sort(shared_shader_list, [](const SharedShader& left, const SharedShader& right) {
    return left.first_candidate_pass > right.first_candidate_pass;
  });
  const auto shared_shader_count = shared_shader_list.size();
  json shared_shaders = json::array();
  for (const auto& shared_shader : shared_shader_list | std::views::take(MAX_SHARED_SHADERS)) {
    json pass_entries = json::array();
    for (const auto& use : shared_shader.uses | std::views::take(MAX_LISTED)) {
      pass_entries.push_back({
          {"pass", use.pass_index},
          {"firstDrawOfShader", use.first_draw},
          {"role", pass_role(use.pass_index)},
      });
    }
    shared_shaders.push_back({
        {"shaderHash", formatting::FormatShaderHash(shared_shader.shader_hash)},
        {"passCount", shared_shader.uses.size()},
        {"passes", std::move(pass_entries)},
    });
  }

  // Baseline and comparison: a structural description of every pass, saved to a file so it survives a game restart
  // (upgrades and mod changes usually need one, and resource handles change with it). Passes are aligned by kind, the
  // targets as the game created them (format, size, swap chain) and blend, so a pass whose targets a mod upgraded or
  // cloned still aligns and shows its new rendered format as the difference.
  json comparison = nullptr;
  std::string comparison_text;
  if (save_baseline_path.has_value() || compare_with_path.has_value()) {
    if (input.snapshot_pending || input.commands.empty()) {
      throw std::runtime_error("saveBaseline and compareWith need a finished snapshot (devkit_queue_snapshot).");
    }
    const auto game_format = [](const TextureInfo& texture) {
      return (texture.is_upgraded ? texture.original_format : texture.format);
    };
    const auto texture_signature = [&](const TextureInfo& texture) {
      return json{
          {"gameFormat", formatting::StreamToString(game_format(texture))},
          {"renderedFormat", formatting::StreamToString(rendered_format(texture))},
          {"width", texture.width},
          {"height", texture.height},
          {"swapchain", texture.is_swapchain},
      };
    };
    json pass_signatures = json::array();
    for (std::size_t pass_index = 0u; pass_index < passes.size(); ++pass_index) {
      const auto& pass = passes[pass_index];
      std::string kind_name = "draw";
      if (pass.kind == CommandKind::DISPATCH) {
        kind_name = "dispatch";
      } else if (pass.kind == CommandKind::COPY) {
        kind_name = "copy";
      }
      std::string key = kind_name;
      json targets = json::array();
      for (const auto& target : pass.targets) {
        key += std::format(
            "|{} {}x{}{}",
            formatting::StreamToString(game_format(target)),
            target.width,
            target.height,
            (target.is_swapchain ? " swapchain" : ""));
        targets.push_back(texture_signature(target));
      }
      if (pass.blend_known) {
        key += (pass.blend_enabled ? "|blend" : "|opaque");
      }
      json inputs = json::array();
      for (const auto& texture : pass.inputs) {
        inputs.push_back(texture_signature(texture));
      }
      json shader_hashes = json::array();
      for (const auto shader_hash : pass.shader_hashes) {
        shader_hashes.push_back(formatting::FormatShaderHash(shader_hash));
      }
      json hint_types = json::array();
      for (const auto& hint : pass.hints) {
        hint_types.push_back(hint["type"]);
      }
      pass_signatures.push_back({
          {"pass", pass_index},
          {"firstDraw", pass.first_draw},
          {"commandCount", pass.command_count},
          {"key", std::move(key)},
          {"targets", std::move(targets)},
          {"inputs", std::move(inputs)},
          {"shaderHashes", std::move(shader_hashes)},
          {"hints", std::move(hint_types)},
      });
    }

    if (compare_with_path.has_value()) {
      std::ifstream baseline_file(std::filesystem::path(compare_with_path.value()));
      if (!baseline_file) {
        throw std::runtime_error(std::format("compareWith: cannot open '{}'.", compare_with_path.value()));
      }
      const auto baseline = json::parse(baseline_file, nullptr, false);
      if (baseline.is_discarded() || !baseline.contains("passes") || !baseline["passes"].is_array()) {
        throw std::runtime_error(std::format("compareWith: '{}' is not a saveBaseline file.", compare_with_path.value()));
      }
      const auto& baseline_passes = baseline["passes"];
      const auto baseline_count = baseline_passes.size();
      const auto current_count = pass_signatures.size();
      if (baseline_count * current_count > MAX_COMPARED_PASS_PAIRS) {
        throw std::runtime_error(std::format("compareWith: {} x {} passes is too many to align.", baseline_count, current_count));
      }

      // Longest common subsequence of the pass keys, in frame order.
      std::vector<std::uint32_t> common_lengths((baseline_count + 1u) * (current_count + 1u), 0u);
      const auto common_length = [&](std::size_t baseline_index, std::size_t current_index) -> std::uint32_t& {
        return common_lengths[(baseline_index * (current_count + 1u)) + current_index];
      };
      for (auto baseline_index = baseline_count; baseline_index-- > 0u;) {
        for (auto current_index = current_count; current_index-- > 0u;) {
          if (baseline_passes[baseline_index]["key"] == pass_signatures[current_index]["key"]) {
            common_length(baseline_index, current_index) = common_length(baseline_index + 1u, current_index + 1u) + 1u;
          } else {
            common_length(baseline_index, current_index) = std::max(
                common_length(baseline_index + 1u, current_index),
                common_length(baseline_index, current_index + 1u));
          }
        }
      }

      const auto rendered_inputs = [](const json& pass_signature) {
        std::vector<std::string> formats;
        for (const auto& texture : pass_signature["inputs"]) {
          formats.push_back(std::format(
              "{} {}x{}",
              texture["renderedFormat"].get<std::string>(),
              texture["width"].get<std::uint32_t>(),
              texture["height"].get<std::uint32_t>()));
        }
        std::ranges::sort(formats);
        return formats;
      };
      const auto pass_brief = [&](const json& pass_signature) {
        json shader_hashes = json::array();
        for (const auto& shader_hash : pass_signature["shaderHashes"] | std::views::take(MAX_LISTED)) {
          shader_hashes.push_back(shader_hash);
        }
        return json{
            {"pass", pass_signature["pass"]},
            {"firstDraw", pass_signature["firstDraw"]},
            {"key", pass_signature["key"]},
            {"shaderHashes", std::move(shader_hashes)},
        };
      };

      json changed_passes = json::array();
      json only_in_baseline = json::array();
      json only_in_current = json::array();
      std::size_t matched_count = 0u;
      std::size_t changed_count = 0u;
      std::size_t only_in_baseline_count = 0u;
      std::size_t only_in_current_count = 0u;
      std::size_t baseline_index = 0u;
      std::size_t current_index = 0u;
      while (baseline_index < baseline_count || current_index < current_count) {
        const bool both_left = (baseline_index < baseline_count && current_index < current_count);
        if (both_left && baseline_passes[baseline_index]["key"] == pass_signatures[current_index]["key"]) {
          const auto& baseline_pass = baseline_passes[baseline_index];
          const auto& current_pass = pass_signatures[current_index];
          json changes = json::array();
          for (std::size_t target_index = 0u; target_index < current_pass["targets"].size(); ++target_index) {
            const auto& baseline_target = baseline_pass["targets"][target_index];
            const auto& current_target = current_pass["targets"][target_index];
            if (baseline_target["renderedFormat"] != current_target["renderedFormat"]) {
              changes.push_back({
                  {"what", "targetRenderedFormat"},
                  {"target", target_index},
                  {"from", baseline_target["renderedFormat"]},
                  {"to", current_target["renderedFormat"]},
              });
            }
          }
          const auto baseline_inputs = rendered_inputs(baseline_pass);
          const auto current_inputs = rendered_inputs(current_pass);
          if (baseline_inputs != current_inputs) {
            std::vector<std::string> removed_inputs;
            std::vector<std::string> added_inputs;
            std::ranges::set_difference(baseline_inputs, current_inputs, std::back_inserter(removed_inputs));
            std::ranges::set_difference(current_inputs, baseline_inputs, std::back_inserter(added_inputs));
            changes.push_back({
                {"what", "inputRenderedFormats"},
                {"removed", removed_inputs},
                {"added", added_inputs},
            });
          }
          // Scene passes draw a different set of objects each frame; only short passes (post-processing) compare shaders.
          const auto& baseline_shaders = baseline_pass["shaderHashes"];
          const auto& current_shaders = current_pass["shaderHashes"];
          if (baseline_shaders.size() <= MAX_COMPARED_SHADERS && current_shaders.size() <= MAX_COMPARED_SHADERS
              && baseline_shaders != current_shaders) {
            changes.push_back({{"what", "shaderHashes"}, {"from", baseline_shaders}, {"to", current_shaders}});
          }
          const auto baseline_commands = baseline_pass["commandCount"].get<std::size_t>();
          const auto current_commands = current_pass["commandCount"].get<std::size_t>();
          const auto fewer_commands = std::min(baseline_commands, current_commands);
          const auto more_commands = std::max(baseline_commands, current_commands);
          if (more_commands - fewer_commands >= 4u && more_commands * 2u >= fewer_commands * 3u) {
            changes.push_back({{"what", "commandCount"}, {"from", baseline_commands}, {"to", current_commands}});
          }
          if (baseline_pass["hints"] != current_pass["hints"]) {
            changes.push_back({{"what", "hints"}, {"from", baseline_pass["hints"]}, {"to", current_pass["hints"]}});
          }
          ++matched_count;
          if (!changes.empty()) {
            ++changed_count;
            if (changed_passes.size() < MAX_COMPARED_PASSES_LISTED) {
              changed_passes.push_back({
                  {"baselinePass", baseline_pass["pass"]},
                  {"currentPass", current_pass["pass"]},
                  {"currentFirstDraw", current_pass["firstDraw"]},
                  {"key", current_pass["key"]},
                  {"changes", std::move(changes)},
              });
            }
          }
          ++baseline_index;
          ++current_index;
        } else if (current_index >= current_count
                   || (both_left && common_length(baseline_index + 1u, current_index) >= common_length(baseline_index, current_index + 1u))) {
          ++only_in_baseline_count;
          if (only_in_baseline.size() < MAX_COMPARED_PASSES_LISTED) {
            only_in_baseline.push_back(pass_brief(baseline_passes[baseline_index]));
          }
          ++baseline_index;
        } else {
          ++only_in_current_count;
          if (only_in_current.size() < MAX_COMPARED_PASSES_LISTED) {
            only_in_current.push_back(pass_brief(pass_signatures[current_index]));
          }
          ++current_index;
        }
      }
      comparison = {
          {"baselinePath", compare_with_path.value()},
          {"baselinePassCount", baseline_count},
          {"currentPassCount", current_count},
          {"matchedPassCount", matched_count},
          {"changedPassCount", changed_count},
          {"onlyInBaselineCount", only_in_baseline_count},
          {"onlyInCurrentCount", only_in_current_count},
          {"changedPasses", std::move(changed_passes)},
          {"onlyInBaseline", std::move(only_in_baseline)},
          {"onlyInCurrent", std::move(only_in_current)},
      };
      comparison_text = std::format(
          " Compared with '{}': {} of {} passes aligned, {} changed, {} only in the baseline, {} only now.",
          compare_with_path.value(),
          matched_count,
          current_count,
          changed_count,
          only_in_baseline_count,
          only_in_current_count);
    }

    if (save_baseline_path.has_value()) {
      std::ofstream baseline_file(std::filesystem::path(save_baseline_path.value()), std::ios::trunc);
      if (!baseline_file) {
        throw std::runtime_error(std::format("saveBaseline: cannot write '{}'.", save_baseline_path.value()));
      }
      baseline_file << json{
          {"version", 1},
          {"capturedCommands", input.commands.size()},
          {"passes", std::move(pass_signatures)},
      }.dump();
      comparison_text += std::format(" Saved the pass list as a baseline to '{}'.", save_baseline_path.value());
    }
  }

  // The listing budget goes to candidates first (tonemap, output, UI, LUT builders, LUT readers), then to the latest
  // passes, where HDR work happens.
  std::vector<bool> listed(passes.size(), false);
  std::size_t listed_count = 0u;
  const auto list_pass = [&](std::size_t pass_index) {
    if (listed_count < max_passes && !listed[pass_index]) {
      listed[pass_index] = true;
      ++listed_count;
    }
  };
  for (const auto pass_index : tonemap_passes) {
    list_pass(pass_index);
  }
  if (output_pass.has_value()) {
    list_pass(output_pass.value());
  }
  for (const auto* candidate_passes : {&ui_passes, &lut_builder_passes, &lut_reader_passes}) {
    for (const auto pass_index : *candidate_passes) {
      list_pass(pass_index);
    }
  }
  for (auto pass_index = passes.size(); pass_index-- > 0u;) {
    list_pass(pass_index);
  }
  std::size_t hinted_count = 0u;
  std::size_t hinted_listed_count = 0u;
  json pass_list = json::array();
  for (std::size_t pass_index = 0u; pass_index < passes.size(); ++pass_index) {
    const auto& pass = passes[pass_index];
    if (!pass.hints.empty()) {
      ++hinted_count;
    }
    if (!listed[pass_index]) continue;
    if (!pass.hints.empty()) {
      ++hinted_listed_count;
    }
    json entry = {
        {"pass", pass_index},
        {"firstDraw", pass.first_draw},
        {"lastDraw", pass.last_draw},
        {"commandCount", pass.command_count},
        {"targets", pass.targets},
        {"inputCount", pass.inputs.size()},
    };
    switch (pass.kind) {
      case CommandKind::DRAW:
        entry["kind"] = "draw";
        entry["blendEnabled"] = (pass.blend_known ? json(pass.blend_enabled) : json(nullptr));
        break;
      case CommandKind::DISPATCH:
        entry["kind"] = "dispatch";
        break;
      case CommandKind::COPY:
        entry["kind"] = "copy";
        break;
    }
    if (pass.order_unknown) {
      entry["orderUnknown"] = true;
    }
    // Inputs are listed for candidates only, to keep the result small.
    if (!pass.hints.empty()) {
      entry["inputs"] = std::vector<TextureInfo>(pass.inputs.begin(), pass.inputs.begin() + std::min(pass.inputs.size(), MAX_LISTED));
      entry["hints"] = pass.hints;
    }
    json shader_hashes = json::array();
    for (const auto shader_hash : pass.shader_hashes | std::views::take(MAX_LISTED)) {
      shader_hashes.push_back(formatting::FormatShaderHash(shader_hash));
    }
    entry["shaderCount"] = pass.shader_hashes.size();
    entry["shaderHashes"] = std::move(shader_hashes);
    pass_list.push_back(std::move(entry));
  }

  json lut_textures = json::array();
  std::size_t lut_texture_count = 0u;
  for (const auto& [handle, use] : resources) {
    if (!is_lut_shaped(use.texture) || use.reader_command_count == 0u) continue;
    ++lut_texture_count;
    if (lut_textures.size() < MAX_LUT_TEXTURES) {
      lut_textures.push_back({
          {"texture", use.texture},
          {"readerCommandCount", use.reader_command_count},
          {"readByDraws", use.reader_draws},
      });
    }
  }

  // Written textures, latest writer first, since the end of the frame matters most.
  std::vector<const ResourceUse*> written;
  for (const auto& [handle, use] : resources) {
    if (!use.writer_passes.empty()) {
      written.push_back(&use);
    }
  }
  std::ranges::sort(written, [](const ResourceUse* left, const ResourceUse* right) {
    return left->writer_passes.back() > right->writer_passes.back();
  });
  json resource_list = json::array();
  for (const auto* use : written | std::views::take(max_resources)) {
    resource_list.push_back({
        {"texture", use->texture},
        {"writerPassCount", use->writer_passes.size()},
        {"lastWrittenByPasses", std::vector<std::size_t>(use->writer_passes.end() - std::min(use->writer_passes.size(), MAX_LISTED), use->writer_passes.end())},
        {"readerCommandCount", use->reader_command_count},
        {"readByDraws", use->reader_draws},
    });
  }

  json screen = nullptr;
  if (input.swapchain_back_buffer.has_value()) {
    screen = {
        {"width", input.swapchain_back_buffer->texture.width},
        {"height", input.swapchain_back_buffer->texture.height},
        {"format", formatting::StreamToString(input.swapchain_back_buffer->texture.format)},
    };
  }
  json output_value = nullptr;
  if (output_pass.has_value()) {
    output_value = output_pass.value();
  }

  std::string text;
  if (input.snapshot_pending) {
    text = std::format("Device #{}: a snapshot is queued or still capturing; call again once it has finished.", device_index);
  } else if (input.commands.empty()) {
    text = std::format("Device #{} has no snapshot: queue one with devkit_queue_snapshot first.", device_index);
  } else {
    std::string output_text = "none";
    if (output_pass.has_value()) {
      output_text = std::to_string(output_pass.value());
    }
    text = std::format(
        "Device #{}: {} commands in {} passes ({} listed). Candidates to verify: {} tonemap, {} LUT texture(s), {} LUT "
        "reader(s), {} LUT builder(s), {} UI, output pass {}, {} shared shader(s).",
        device_index,
        input.commands.size(),
        passes.size(),
        listed_count,
        tonemap_passes.size(),
        lut_texture_count,
        lut_reader_passes.size(),
        lut_builder_passes.size(),
        ui_passes.size(),
        output_text,
        shared_shader_count);
  }
  text += comparison_text;

  auto result = ToolResult{
      .text = std::move(text),
      .structured_content = json{
          {"deviceIndex", device_index},
          {"snapshotPending", input.snapshot_pending},
          {"capturedCommands", input.commands.size()},
          {"screen", screen},
          {"passCount", passes.size()},
          {"passesListed", listed_count},
          {"candidatePassCount", hinted_count},
          {"candidatePassesListed", hinted_listed_count},
          {"candidates", {
              {"tonemapPasses", tonemap_passes},
              {"outputPass", output_value},
              {"uiPasses", ui_passes},
              {"lutBuilderPasses", lut_builder_passes},
              {"lutReaderPasses", lut_reader_passes},
              {"lutTextureCount", lut_texture_count},
              {"lutTextures", std::move(lut_textures)},
              {"sharedShaderCount", shared_shader_count},
              {"sharedShaders", std::move(shared_shaders)},
          }},
          {"passes", std::move(pass_list)},
          {"writtenTextureCount", written.size()},
          {"writtenTextures", std::move(resource_list)},
          {"notes", {
              "Hints are candidates from formats, sizes, blend state and command order, not proof: confirm with devkit_get_draw, the shader and devkit_watch_region.",
              "Snapshots record no viewports, vertex counts or depth targets: a large target covers at least a quarter of the swap chain's back buffer, and depth-only passes into different depth targets merge.",
              "Blend state is recorded from pipelines that carry one and, on D3D9 and OpenGL, read from the device or context state at each draw; blendEnabled is null where none could be recorded.",
              "Readers come from the shader resource views recorded per command (pixel stage for draws, compute for dispatches) and copy sources; reads through D3D12/Vulkan descriptor tables the snapshot did not walk, and bindless reads, are missing.",
              "With a mod loaded, every pass that renders into the mod's float clones or upgrades can match the tonemap rule; the first candidate after the scene passes is the likely one.",
              "orderUnknown passes (D3D12/Vulkan commands without a known submission order) are sorted after all others and are never UI or output candidates.",
          }},
      },
  };
  if (!comparison.is_null()) {
    result.structured_content["comparison"] = std::move(comparison);
  }
  return result;
}
}  // namespace renodx::addons::devkit::mcp::snapshot_tools
