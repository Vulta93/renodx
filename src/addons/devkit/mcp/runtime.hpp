/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "../../../utils/mcp/server.hpp"
#include "../../../utils/mcp/arguments.hpp"
#include "live_shaders.hpp"
#include "resource_analysis.hpp"
#include "resource_clone.hpp"
#include "shader_inspection.hpp"
#include "snapshot_tools.hpp"
#include "texture_replace.hpp"
#include "tool_catalog.hpp"
#include "watch_region.hpp"

namespace renodx::addons::devkit::mcp::runtime {

struct RegistrationContext {
  std::function<snapshot_tools::ToolContext()> build_snapshot_tools_context;
  std::function<shader_inspection::ToolContext()> build_shader_inspection_tool_context;
  std::function<live_shaders::ToolContext()> build_live_shaders_tool_context;
  std::function<resource_analysis::ToolContext()> build_analyze_resource_tool_context;
  std::function<watch_region::ToolContext()> build_watch_region_tool_context;
  std::function<resource_clone::ToolContext()> build_resource_clone_tool_context;
  std::function<texture_replace::ToolContext()> build_texture_replace_tool_context;
  std::function<renodx::utils::mcp::ToolResult(const std::optional<std::string>&)> set_tools_path;
  // Waits until the person presses the virtual key in the game, at most timeout; false on timeout.
  std::function<bool(std::uint32_t virtual_key, std::chrono::seconds timeout)> wait_for_capture_key;
};

inline void RegisterTools(renodx::utils::mcp::Server& server, const RegistrationContext& context) {
  server.ClearTools();

  auto register_tool = [&server](std::string_view tool_name, const auto& handler) {
    const auto metadata_iterator = tool_catalog::METADATA.find(tool_name);
    if (metadata_iterator == tool_catalog::METADATA.end()) {
      throw std::logic_error("Unknown devkit MCP tool metadata");
    }

    const auto& metadata = metadata_iterator->second;
    server.RegisterTool(renodx::utils::mcp::Tool{
        .name = std::string(tool_name),
        .title = std::string(metadata.title),
        .description = std::string(metadata.description),
        .input_schema = metadata.input_schema,
        .annotations = metadata.annotations,
        .handler = handler,
    });
  };

  // waitForKey and delaySeconds let a capture land on gameplay after the game is unpaused: first the wait for the key
  // the person presses in the game, then the delay. Both happen before the request is queued, so they do not count
  // against the tool's own wait for the next present. Returns the error result when the key was not pressed in time.
  const auto wait_for_capture_trigger = [wait_for_capture_key = context.wait_for_capture_key](
                                            const renodx::utils::mcp::json& arguments)
      -> std::optional<renodx::utils::mcp::ToolResult> {
    // Checked before the key wait, so a bad delay fails at once.
    auto delay_seconds = renodx::utils::mcp::arguments::GetOptional<std::uint32_t>(arguments, "delaySeconds");
    if (delay_seconds.has_value()) {
      delay_seconds = renodx::utils::mcp::arguments::ValidateRange<std::uint32_t>("delaySeconds", delay_seconds.value(), 0u, tool_catalog::MAX_CAPTURE_DELAY_SECONDS);
    }
    if (const auto key_name = renodx::utils::mcp::arguments::GetOptional<std::string>(arguments, "waitForKey"); key_name.has_value()) {
      // F1-F12 only: function keys are rarely game bindings that also change what is on screen.
      std::uint32_t function_key_number = 0u;
      if (key_name->size() >= 2u && (key_name->front() == 'F' || key_name->front() == 'f')) {
        const auto [end, error] = std::from_chars(key_name->data() + 1, key_name->data() + key_name->size(), function_key_number);
        if (error != std::errc() || end != key_name->data() + key_name->size()) {
          function_key_number = 0u;
        }
      }
      if (function_key_number < 1u || function_key_number > 12u) {
        throw std::runtime_error("waitForKey must be one of F1 to F12.");
      }
      if (!wait_for_capture_key) {
        throw std::runtime_error("waitForKey is not available in this build.");
      }
      constexpr std::uint32_t VIRTUAL_KEY_F1 = 0x70u;
      if (!wait_for_capture_key(VIRTUAL_KEY_F1 + function_key_number - 1u, std::chrono::seconds(tool_catalog::MAX_CAPTURE_KEY_WAIT_SECONDS))) {
        const auto error = std::format(
            "{} was not pressed in the game within {} s; nothing was captured. Press it with the game window focused.",
            key_name.value(),
            tool_catalog::MAX_CAPTURE_KEY_WAIT_SECONDS);
        return renodx::utils::mcp::ToolResult{
            .text = error,
            .structured_content = renodx::utils::mcp::json{{"error", error}},
            .is_error = true,
        };
      }
    }
    if (delay_seconds.has_value()) {
      std::this_thread::sleep_for(std::chrono::seconds(delay_seconds.value()));
    }
    return std::nullopt;
  };

  register_tool("devkit_status", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                      const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleStatusTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_select_device", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                             const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleSelectDeviceTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_list_shaders", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                            const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleListShadersTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_get_shader", [build_shader_inspection_tool_context = context.build_shader_inspection_tool_context](
                                         const renodx::utils::mcp::json& arguments) {
    return shader_inspection::HandleGetShaderTool(arguments, build_shader_inspection_tool_context());
  });
  register_tool("devkit_dump_shader", [build_live_shaders_tool_context = context.build_live_shaders_tool_context](
                                          const renodx::utils::mcp::json& arguments) {
    return live_shaders::HandleDumpShaderTool(arguments, build_live_shaders_tool_context());
  });
  register_tool("devkit_set_tools_path", [set_tools_path = context.set_tools_path](const renodx::utils::mcp::json& arguments) {
    const auto path = renodx::utils::mcp::arguments::GetOptional<std::string>(arguments, "path");
    return set_tools_path(path);
  });
  register_tool("devkit_set_live_shader_path", [build_live_shaders_tool_context = context.build_live_shaders_tool_context](
                                                    const renodx::utils::mcp::json& arguments) {
    return live_shaders::HandleSetLiveShaderPathTool(arguments, build_live_shaders_tool_context());
  });
  register_tool("devkit_load_live_shaders", [build_live_shaders_tool_context = context.build_live_shaders_tool_context](
                                                const renodx::utils::mcp::json& arguments) {
    return live_shaders::HandleLoadLiveShadersTool(arguments, build_live_shaders_tool_context());
  });
  register_tool("devkit_unload_live_shaders", [build_live_shaders_tool_context = context.build_live_shaders_tool_context](
                                                  const renodx::utils::mcp::json& arguments) {
    return live_shaders::HandleUnloadLiveShadersTool(arguments, build_live_shaders_tool_context());
  });
  register_tool("devkit_set_shader_source", [build_live_shaders_tool_context = context.build_live_shaders_tool_context](
                                                const renodx::utils::mcp::json& arguments) {
    return live_shaders::HandleSetShaderSourceTool(arguments, build_live_shaders_tool_context());
  });
  register_tool("devkit_list_draws", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                          const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleListDrawsTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_list_replacements", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                                 const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleListReplacementsTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_frame_overview", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                              const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleFrameOverviewTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_get_draw", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                        const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleGetDrawTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_analyze_resource", [wait_for_capture_trigger, build_analyze_resource_tool_context = context.build_analyze_resource_tool_context](
                                               const renodx::utils::mcp::json& arguments) {
    if (auto trigger_error = wait_for_capture_trigger(arguments)) return *trigger_error;
    return resource_analysis::HandleAnalyzeResourceTool(arguments, build_analyze_resource_tool_context());
  });
  register_tool("devkit_watch_region", [wait_for_capture_trigger, build_watch_region_tool_context = context.build_watch_region_tool_context](
                                           const renodx::utils::mcp::json& arguments) {
    if (auto trigger_error = wait_for_capture_trigger(arguments)) return *trigger_error;
    return watch_region::HandleWatchRegionTool(arguments, build_watch_region_tool_context());
  });
  register_tool("devkit_dump_resource_with_hash", [build_analyze_resource_tool_context = context.build_analyze_resource_tool_context](
                                                       const renodx::utils::mcp::json& arguments) {
    return resource_analysis::HandleDumpResourceWithHashTool(arguments, build_analyze_resource_tool_context());
  });
  register_tool("devkit_set_resource_clone", [build_resource_clone_tool_context = context.build_resource_clone_tool_context](
                                                 const renodx::utils::mcp::json& arguments) {
    return resource_clone::HandleSetResourceCloneTool(arguments, build_resource_clone_tool_context());
  });
  register_tool("devkit_replace_resource_with_file", [build_resource_clone_tool_context = context.build_resource_clone_tool_context](
                                                         const renodx::utils::mcp::json& arguments) {
    return resource_clone::HandleReplaceResourceWithFileTool(arguments, build_resource_clone_tool_context());
  });
  register_tool("devkit_set_texture_replace_enabled", [build_texture_replace_tool_context = context.build_texture_replace_tool_context](
                                                           const renodx::utils::mcp::json& arguments) {
    return texture_replace::HandleSetTextureReplaceEnabledTool(arguments, build_texture_replace_tool_context());
  });
  register_tool("devkit_reload_texture_replace_boot_cache", [build_texture_replace_tool_context = context.build_texture_replace_tool_context](
                                                                  const renodx::utils::mcp::json& arguments) {
    return texture_replace::HandleReloadBootTextureCacheTool(arguments, build_texture_replace_tool_context());
  });
  register_tool("devkit_set_texture_replace_rules", [build_texture_replace_tool_context = context.build_texture_replace_tool_context](
                                                         const renodx::utils::mcp::json& arguments) {
    return texture_replace::HandleSetTextureReplaceRulesTool(arguments, build_texture_replace_tool_context());
  });
  register_tool("devkit_get_texture_replace_state", [build_texture_replace_tool_context = context.build_texture_replace_tool_context](
                                                         const renodx::utils::mcp::json& arguments) {
    return texture_replace::HandleGetTextureReplaceStateTool(arguments, build_texture_replace_tool_context());
  });
  register_tool("devkit_list_texture_replace_observations", [build_texture_replace_tool_context = context.build_texture_replace_tool_context](
                                                                 const renodx::utils::mcp::json& arguments) {
    return texture_replace::HandleListTextureReplaceObservationsTool(arguments, build_texture_replace_tool_context());
  });
  register_tool("devkit_clear_texture_replace_observations", [build_texture_replace_tool_context = context.build_texture_replace_tool_context](
                                                                  const renodx::utils::mcp::json& arguments) {
    return texture_replace::HandleClearTextureReplaceObservationsTool(arguments, build_texture_replace_tool_context());
  });
  register_tool("devkit_dump_texture_replace_observation", [build_texture_replace_tool_context = context.build_texture_replace_tool_context](
                                                                const renodx::utils::mcp::json& arguments) {
    return texture_replace::HandleDumpTextureReplaceObservationTool(arguments, build_texture_replace_tool_context());
  });
  register_tool("devkit_snapshot_summary", [build_snapshot_tools_context = context.build_snapshot_tools_context](
                                               const renodx::utils::mcp::json& arguments) {
    return snapshot_tools::HandleSnapshotSummaryTool(arguments, build_snapshot_tools_context());
  });
  register_tool("devkit_queue_snapshot", [wait_for_capture_trigger, build_snapshot_tools_context = context.build_snapshot_tools_context](
                                             const renodx::utils::mcp::json& arguments) {
    if (auto trigger_error = wait_for_capture_trigger(arguments)) return *trigger_error;
    return snapshot_tools::HandleQueueSnapshotTool(arguments, build_snapshot_tools_context());
  });
}

}  // namespace renodx::addons::devkit::mcp::runtime
