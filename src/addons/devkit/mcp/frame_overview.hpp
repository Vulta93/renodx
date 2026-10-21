/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <include/reshade.hpp>

#include "../formatting.hpp"
#include "../../../utils/mcp/types.hpp"

namespace renodx::addons::devkit::mcp::frame_overview {

using json = renodx::utils::mcp::json;

// A texture a snapshot command used, described by the resource the game created. With a mod loaded, the resource can be
// upgraded in place (format is the new one, original_format the game's) or redirected into an active clone.
struct TextureInfo {
  std::uint64_t resource_handle = 0u;
  reshade::api::resource_type type = reshade::api::resource_type::unknown;
  reshade::api::format format = reshade::api::format::unknown;
  reshade::api::format original_format = reshade::api::format::unknown;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint32_t depth_or_layers = 0u;
  bool is_swapchain = false;
  bool is_upgraded = false;
  bool clone_active = false;
  std::optional<reshade::api::format> clone_format = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const TextureInfo& value) {
  j = {
      {"resourceHandle", formatting::FormatHandle(value.resource_handle)},
      {"type", formatting::StreamToString(value.type)},
      {"format", formatting::StreamToString(value.format)},
      {"width", value.width},
      {"height", value.height},
  };
  if (value.type == reshade::api::resource_type::texture_3d) {
    j["depth"] = value.depth_or_layers;
  }
  if (value.is_swapchain) {
    j["isSwapchain"] = true;
  }
  if (value.is_upgraded) {
    j["upgradedFrom"] = formatting::StreamToString(value.original_format);
  }
  if (value.clone_format.has_value()) {
    j["cloneFormat"] = formatting::StreamToString(value.clone_format.value());
    j["cloneActive"] = value.clone_active;
  }
}

enum class CommandKind : std::uint8_t {
  DRAW,
  DISPATCH,
  COPY,
};

// One snapshot row reduced to what the overview needs. Draws list their render targets as targets, dispatches their
// UAVs, copies their destination (with the source as input).
struct Command {
  std::size_t draw_index = 0u;
  CommandKind kind = CommandKind::DRAW;
  std::vector<TextureInfo> targets;
  std::vector<TextureInfo> inputs;
  // Blend state is only recorded from pipelines that carry one (not D3D9 or OpenGL dynamic state).
  bool blend_known = false;
  bool blend_enabled = false;
  // D3D12/Vulkan commands whose submission order is unknown are sorted after every ordered command.
  bool order_unknown = false;
  std::uint32_t shader_hash = 0u;
};

// Read under one lock, so the commands match the captured draw count.
struct Input {
  bool snapshot_pending = false;
  std::optional<reshade::api::resource_desc> swapchain_back_buffer = std::nullopt;
  std::vector<Command> commands;
};

}  // namespace renodx::addons::devkit::mcp::frame_overview
