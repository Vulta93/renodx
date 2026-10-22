/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
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

namespace renodx::addons::devkit::mcp::snapshot_tools {

using json = renodx::utils::mcp::json;
using ToolResult = renodx::utils::mcp::ToolResult;
namespace mcp_arguments = renodx::utils::mcp::arguments;

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

  return context.queue_snapshot(internal::ResolveDeviceIndex(arguments, context));
}

inline ToolResult HandleListReplacementsTool(const json& arguments, const ToolContext& context) {
  if (!context.list_replacements) {
    throw std::runtime_error("The snapshot tool context is not configured.");
  }

  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  const auto list = context.list_replacements(device_index);
  const auto drawn_count = std::ranges::count_if(list.replacements, [](const shader_summary::ReplacementSummary& replacement) {
    return replacement.snapshot_draws != 0u;
  });
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
  return ToolResult{
      .text = std::move(text),
      .structured_content = json{
          {"deviceIndex", device_index},
          {"capturedDraws", list.captured_draws},
          {"snapshotPending", list.snapshot_pending},
          {"replacements", list.replacements},
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
  const auto device_index = internal::ResolveDeviceIndex(arguments, context);
  const auto input = context.collect_frame_overview(device_index);

  using frame_overview::CommandKind;
  using frame_overview::TextureInfo;
  constexpr std::size_t MAX_LISTED = 8u;
  constexpr std::size_t MAX_LUT_TEXTURES = 16u;

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
      rule += " (blend state not recorded on this API)";
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
  // intermediate before that pass, while a vanilla game's tonemap into the back buffer stays the anchor.
  std::vector<std::size_t> ui_passes;
  if (const auto ui_anchor = std::ranges::find_if(
          tonemap_passes.rbegin(),
          tonemap_passes.rend(),
          [&](std::size_t pass_index) { return std::ranges::find(final_passes_from_mod_upgrade, pass_index) == final_passes_from_mod_upgrade.end(); });
      ui_anchor != tonemap_passes.rend()) {
    for (auto pass_index = *ui_anchor + 1u; pass_index < passes.size(); ++pass_index) {
      auto& pass = passes[pass_index];
      if (pass.kind != CommandKind::DRAW || !pass.blend_known || !pass.blend_enabled || pass.order_unknown || pass.targets.empty()) continue;
      const auto& target = pass.targets.front();
      if (!is_large(target) || !(target.is_swapchain || is_sdr_format(target.original_format))) continue;
      pass.hints.push_back({{"type", "ui"}, {"rule", "blended draws onto a large SDR or swap-chain target after the last tonemap candidate that is not a final pass from a mod-upgraded intermediate"}});
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
        "reader(s), {} LUT builder(s), {} UI, output pass {}.",
        device_index,
        input.commands.size(),
        passes.size(),
        listed_count,
        tonemap_passes.size(),
        lut_texture_count,
        lut_reader_passes.size(),
        lut_builder_passes.size(),
        ui_passes.size(),
        output_text);
  }

  return ToolResult{
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
          }},
          {"passes", std::move(pass_list)},
          {"writtenTextureCount", written.size()},
          {"writtenTextures", std::move(resource_list)},
          {"notes", {
              "Hints are candidates from formats, sizes, blend state and command order, not proof: confirm with devkit_get_draw, the shader and devkit_watch_region.",
              "Snapshots record no viewports, vertex counts or depth targets: a large target covers at least a quarter of the swap chain's back buffer, and depth-only passes into different depth targets merge.",
              "Blend state is only recorded from pipelines that carry one; on D3D9 and OpenGL blendEnabled is null and no UI candidates are found.",
              "Readers come from the shader resource views recorded per command (pixel stage for draws, compute for dispatches) and copy sources; reads through D3D12/Vulkan descriptor tables the snapshot did not walk, and bindless reads, are missing.",
              "With a mod loaded, every pass that renders into the mod's float clones or upgrades can match the tonemap rule; the first candidate after the scene passes is the likely one.",
              "orderUnknown passes (D3D12/Vulkan commands without a known submission order) are sorted after all others and are never UI or output candidates.",
          }},
      },
  };
}
}  // namespace renodx::addons::devkit::mcp::snapshot_tools
