/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <include/reshade.hpp>

#include "../formatting.hpp"
#include "../../../utils/mcp/types.hpp"

namespace renodx::addons::devkit::mcp::device_summary {

using json = renodx::utils::mcp::json;

struct PrimarySwapchainSummary {
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::string format;
  std::uint32_t buffer_count = 0u;
  std::uint32_t sample_count = 0u;
  std::uint32_t present_mode = 0u;
  std::uint32_t present_flags = 0u;
  bool is_flip = false;
  std::optional<std::string> window_handle = std::nullopt;

  PrimarySwapchainSummary() = default;

  PrimarySwapchainSummary(const reshade::api::swapchain_desc& desc, bool flip, const void* hwnd)
      : width(desc.back_buffer.texture.width),
        height(desc.back_buffer.texture.height),
        format(formatting::StreamToString(desc.back_buffer.texture.format)),
        buffer_count(desc.back_buffer_count),
        sample_count(desc.back_buffer.texture.samples),
        present_mode(static_cast<std::uint32_t>(desc.present_mode)),
        present_flags(desc.present_flags),
        is_flip(flip),
        window_handle(hwnd == nullptr ? std::nullopt : std::optional<std::string>(formatting::FormatPointer(hwnd))) {}
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const PrimarySwapchainSummary& value) {
  j = {
      {"width", value.width},
      {"height", value.height},
      {"format", value.format},
      {"bufferCount", value.buffer_count},
      {"sampleCount", value.sample_count},
      {"presentMode", value.present_mode},
      {"presentFlags", value.present_flags},
      {"isFlip", value.is_flip},
  };
  if (value.window_handle.has_value()) {
    j["windowHandle"] = value.window_handle.value();
  }
}

// A view the Devkit created on a Display Proxy back buffer, to pass as resourceViewHandle to the measuring tools.
struct BackBufferViewSummary {
  std::string resource_view_handle;
  std::string resource_handle;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::string format;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const BackBufferViewSummary& value) {
  j = {
      {"resourceViewHandle", value.resource_view_handle},
      {"resourceHandle", value.resource_handle},
      {"width", value.width},
      {"height", value.height},
      {"format", value.format},
  };
}

struct DeviceSummary {
  std::size_t index = 0u;
  bool is_selected = false;
  std::string device_handle;
  std::string api;
  bool is_d3d9_ex = false;
  bool is_d3d9_pure_device = false;
  bool is_display_proxy = false;
  std::size_t tracked_shaders = 0u;
  std::size_t captured_draws = 0u;
  std::size_t live_pipelines = 0u;
  std::size_t swapchains = 0u;
  std::size_t snapshot_rows = 0u;
  bool snapshot_rows_valid = false;
  std::size_t resource_usage_entries = 0u;
  bool snapshot_queued = false;
  bool snapshot_active = false;
  std::optional<PrimarySwapchainSummary> primary_swapchain = std::nullopt;
  std::vector<BackBufferViewSummary> back_buffer_views;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const DeviceSummary& value) {
  j = {
      {"index", value.index},
      {"isSelected", value.is_selected},
      {"deviceHandle", value.device_handle},
      {"api", value.api},
      {"isD3D9Ex", value.is_d3d9_ex},
      {"isProxy", value.is_display_proxy},
      {"trackedShaders", value.tracked_shaders},
      {"capturedDraws", value.captured_draws},
      {"livePipelines", value.live_pipelines},
      {"swapchains", value.swapchains},
      {"snapshotRows", value.snapshot_rows},
      {"snapshotRowsValid", value.snapshot_rows_valid},
      {"resourceUsageEntries", value.resource_usage_entries},
      {"snapshotQueued", value.snapshot_queued},
      {"snapshotActive", value.snapshot_active},
  };
  if (value.is_d3d9_pure_device) {
    j["isD3D9PureDevice"] = true;
  }
  if (value.primary_swapchain.has_value()) {
    j["primarySwapchain"] = value.primary_swapchain.value();
  }
  if (!value.back_buffer_views.empty()) {
    j["backBufferViews"] = value.back_buffer_views;
  }
}

}  // namespace renodx::addons::devkit::mcp::device_summary
