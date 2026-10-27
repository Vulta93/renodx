/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <include/reshade.hpp>

#include "../../../utils/bitwise.hpp"
#include "../../../utils/exr.hpp"
#include "../../../utils/mcp/types.hpp"
#include "../../../utils/mcp/arguments.hpp"
#include "../../../utils/png.hpp"
#include "../../../utils/resource.hpp"
#include "resource_handle.hpp"
#include "resource_view_summary.hpp"
#include "shader_hash.hpp"

namespace renodx::addons::devkit::mcp::resource_analysis {

using json = renodx::utils::mcp::json;
using ToolResult = renodx::utils::mcp::ToolResult;
namespace mcp_arguments = renodx::utils::mcp::arguments;

enum class PreviewMode : std::uint8_t {
  CLAMPED = 0,
  NEUTWO = 1,
};

enum class OutputFormat : std::uint8_t {
  PNG = 0,
  EXR = 1,
};

// How decoded values map to display light for the optional nits summary.
// RAW keeps the analysis on decoded values only.
enum class Signal : std::uint8_t {
  RAW = 0,
  PQ = 1,
  SCRGB = 2,
};

struct LutOptions {
  // "auto", "3d" (an N x N x N volume), "strip_x" (N*N x N: N slices side by side) or "strip_y" (N x N*N, stacked).
  std::string layout = "auto";
  // Green increases from the bottom row up instead of from the top.
  bool flip_y = false;
  std::optional<std::filesystem::path> csv_path = std::nullopt;
};

struct PendingRequest {
  reshade::api::resource_view resource_view = {0u};
  std::optional<std::filesystem::path> output_path = std::nullopt;
  std::optional<std::filesystem::path> output_directory = std::nullopt;
  std::optional<std::string> name_prefix = std::nullopt;
  PreviewMode preview_mode = PreviewMode::CLAMPED;
  std::string preview_mode_name = "clamped";
  bool prefer_clone = false;
  std::optional<reshade::api::subresource_box> region = std::nullopt;
  Signal signal = Signal::RAW;
  // measurePoint 'frame_end': read in reshade_present, after every add-on's present handler and ReShade's effects and
  // overlay, instead of in the Devkit's own present handler.
  bool at_frame_end = false;
  // Vulkan frame_end: a Devkit-owned copy of the back buffer recorded in reshade_present, read after finish_present
  // instead of the swap-chain image (0 otherwise), and the effect runtime's queue it was recorded on, which the analysis
  // uses (the presenting queue can be another one).
  reshade::api::resource frame_end_copy = {0u};
  reshade::api::command_queue* frame_end_queue = nullptr;
  // frames > 1: one measurement per present on consecutive frames. Only the present thread touches samples and
  // presents_seen while the request is queued again between presents; the MCP thread reads them after completion.
  std::uint32_t frames = 1u;
  std::vector<json> samples;
  std::uint32_t presents_seen = 0u;
  // A/B: frames alternate between shader sources ab_sources[0] (A) and [1] (B) of ab_shader_hash, A, B, B, A, ...; the
  // present thread switches the source between frames and puts ab_restore_source back when the request ends.
  std::optional<std::uint32_t> ab_shader_hash = std::nullopt;
  std::array<std::string, 2> ab_sources = {"original", "addon"};
  std::optional<std::string> ab_restore_source = std::nullopt;
  std::size_t ab_current_side = 0u;
  // The source the shader has after the A/B, and why it is not ab_restore_source when restoring it failed.
  std::string ab_restored_source;
  std::string ab_restore_note;
  // Tone curve: the pass's input is read in the same present as its output (resource_view), so both come from one frame;
  // both keep one luminance per pixel. Only the present thread writes them while the request runs.
  std::optional<reshade::api::resource_view> curve_input_view = std::nullopt;
  Signal curve_input_signal = Signal::RAW;
  std::vector<float> curve_input_luminance;
  std::vector<float> curve_output_luminance;
  json curve_input_result;
  // devkit_analyze_lut: read the texture as a colour lookup table instead of an image.
  std::optional<LutOptions> lut = std::nullopt;
  // banding: also measure the steps between neighbouring pixels (devkit_analyze_resource banding: true).
  bool banding = false;
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false;
  bool canceled = false;
  bool completed = false;
  std::optional<ToolResult> result = std::nullopt;
};

// A snapshot draw's input and output for devkit_measure_tone_curve.
struct DrawCurveViews {
  reshade::api::resource_view input_view = {0u};
  reshade::api::resource_view output_view = {0u};
  std::uint32_t input_slot = 0u;
  // Later commands of the snapshot frame that write the input or the output again; the end-of-frame contents are then
  // no longer the ones the pass read or wrote.
  std::vector<std::size_t> later_input_writers;
  std::vector<std::size_t> later_output_writers;
};

struct ToolContext {
  std::function<std::uint32_t(const json& arguments)> resolve_device_index;
  // Throws why the draw has no usable output or input.
  std::function<DrawCurveViews(std::uint32_t device_index, std::size_t draw_index, std::optional<std::uint32_t> input_slot)> resolve_draw_curve_views;
  std::function<bool(std::uint32_t device_index, const std::shared_ptr<PendingRequest>& request)> enqueue_request;
  std::function<void(std::uint32_t device_index, const std::shared_ptr<PendingRequest>& request)> cancel_request;
};

struct ProcessingContext {
  std::function<std::string(std::uint64_t value)> format_handle;
  std::function<std::string(reshade::api::format format)> format_format;
  std::function<resource_view_summary::ResourceViewSummary(reshade::api::resource_view resource_view, reshade::api::device* device)> build_resource_view_summary;
  // Makes a shader draw from 'original', 'addon' or 'file' from its next bind and returns its previous source; throws
  // why it cannot. restoring marks the switch back at the end of an A/B, which only a missing source can refuse. Called
  // on the present thread.
  std::function<std::string(reshade::api::device* device, std::uint32_t shader_hash, std::string_view source, bool restoring)> set_shader_source;
};

namespace internal {

[[nodiscard]] inline std::string FormatHandle(const ProcessingContext& context, std::uint64_t value) {
  if (context.format_handle) {
    return context.format_handle(value);
  }
  return std::format("0x{:016X}", value);
}

[[nodiscard]] inline std::string FormatFormat(const ProcessingContext& context, reshade::api::format format) {
  if (context.format_format) {
    return context.format_format(format);
  }
  return std::format("{}", static_cast<std::uint32_t>(format));
}

[[nodiscard]] inline std::optional<renodx::utils::resource::ResourceViewInfo> FindTrackedResourceViewInfo(reshade::api::resource_view resource_view) {
  if (resource_view.handle == 0u) return std::nullopt;
  std::optional<renodx::utils::resource::ResourceViewInfo> result = std::nullopt;
  renodx::utils::resource::GetResourceViewInfo(resource_view, [&result](const renodx::utils::resource::ResourceViewInfo& info) {
    result = info;
  });
  return result;
}

struct AnalysisTarget {
  reshade::api::resource_view requested_view = {0u};
  reshade::api::resource_view resolved_view = {0u};
  reshade::api::resource_view_desc resolved_view_desc;
  reshade::api::resource resolved_resource = {0u};
  reshade::api::resource_desc resolved_resource_desc;
  bool used_clone = false;
};

struct ScalarStats {
  std::optional<double> minimum = std::nullopt;
  std::optional<double> maximum = std::nullopt;
  std::optional<double> mean = std::nullopt;

  ScalarStats() = default;

  ScalarStats(double minimum_value, double maximum_value, double sum, std::uint64_t count) {
    if (count == 0u) {
      return;
    }

    minimum = minimum_value;
    maximum = maximum_value;
    mean = sum / static_cast<double>(count);
  }
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const ScalarStats& value) {
  j = {
      {"min", value.minimum.has_value() ? json(value.minimum.value()) : json(nullptr)},
      {"max", value.maximum.has_value() ? json(value.maximum.value()) : json(nullptr)},
      {"mean", value.mean.has_value() ? json(value.mean.value()) : json(nullptr)},
  };
}

struct RatioStats {
  std::uint64_t count = 0u;
  double ratio = 0.0;

  RatioStats() = default;

  RatioStats(std::uint64_t matching_count, std::uint64_t total_count)
      : count(matching_count),
        ratio(total_count == 0u ? 0.0 : static_cast<double>(matching_count) / static_cast<double>(total_count)) {}
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const RatioStats& value) {
  j = {
      {"count", value.count},
      {"ratio", value.ratio},
  };
}

// Box copies and resolves of a depth-stencil resource are dropped by D3D10/11 and invalid on D3D12. The tracked description
// matches the device's own for resources created while the Devkit runs (ReShade converts both the same way); the device is
// asked as well for a record filled some other way, such as a resource that existed before the Devkit loaded.
[[nodiscard]] inline bool IsDepthStencilResource(reshade::api::device* device, reshade::api::resource resource, const reshade::api::resource_desc& tracked_desc) {
  return renodx::utils::bitwise::HasAnyFlag(
      tracked_desc.usage | device->get_resource_desc(resource).usage,
      reshade::api::resource_usage::depth_stencil);
}

[[nodiscard]] inline AnalysisTarget ResolveAnalysisTarget(
    reshade::api::resource_view resource_view,
    reshade::api::device* device,
    bool prefer_clone) {
  const auto resource_view_info = FindTrackedResourceViewInfo(resource_view);
  if (!resource_view_info.has_value() || resource_view_info->destroyed) {
    throw std::runtime_error(std::format("resourceViewHandle {} is not currently tracked.", resource_view.handle));
  }
  if (resource_view_info->device != device) {
    throw std::runtime_error("resourceViewHandle does not belong to the selected device.");
  }

  reshade::api::resource resolved_resource = {0u};
  reshade::api::resource_desc resolved_resource_desc = {};
  bool clone_enabled = false;
  reshade::api::resource clone_resource = {0u};
  reshade::api::resource_desc clone_resource_desc = {};
  bool resource_destroyed = false;
  const auto found_resource_info = renodx::utils::resource::GetResourceInfo(resource_view_info->original_resource, [&](const renodx::utils::resource::ResourceInfo& info) {
    resolved_resource = info.resource;
    resolved_resource_desc = info.desc;
    clone_enabled = info.clone_enabled;
    clone_resource = info.clone;
    clone_resource_desc = info.clone_desc;
    resource_destroyed = info.destroyed;
  });
  if (!found_resource_info || resource_destroyed) {
    throw std::runtime_error("The selected resource view points to a destroyed resource.");
  }

  AnalysisTarget target = {
      .requested_view = resource_view,
      .resolved_view = resource_view,
      .resolved_view_desc = resource_view_info->desc,
      .resolved_resource = resolved_resource,
      .resolved_resource_desc = resolved_resource_desc,
      .used_clone = false,
  };

  if (prefer_clone) {
    if (!clone_enabled) {
      throw std::runtime_error("preferClone was requested, but clone hotswap is not enabled for the backing resource.");
    }
    if (resource_view_info->clone.handle == 0u || clone_resource.handle == 0u) {
      throw std::runtime_error("preferClone was requested, but the clone view/resource is not currently instantiated. Render a frame after enabling clone hotswap and try again.");
    }

    target.resolved_view = resource_view_info->clone;
    target.resolved_view_desc = resource_view_info->clone_desc;
    target.resolved_resource = clone_resource;
    target.resolved_resource_desc = clone_resource_desc;
    target.used_clone = true;
  }

  return target;
}

[[nodiscard]] inline float DecodeFloat16(uint16_t value) {
  const auto sign = static_cast<int>((value >> 15) & 0x1u);
  const auto exponent = static_cast<int>((value >> 10) & 0x1Fu);
  const auto mantissa = static_cast<uint32_t>(value & 0x03FFu);

  float decoded = 0.f;
  if (exponent == 0) {
    if (mantissa != 0u) {
      decoded = std::ldexp(static_cast<float>(mantissa), -24);
    }
  } else if (exponent == 0x1F) {
    decoded = mantissa == 0u
                  ? std::numeric_limits<float>::infinity()
                  : std::numeric_limits<float>::quiet_NaN();
  } else {
    decoded = std::ldexp(1.f + (static_cast<float>(mantissa) / 1024.f), exponent - 15);
  }

  return sign != 0 ? -decoded : decoded;
}

[[nodiscard]] inline float DecodeUnsignedFloat(uint32_t value, uint32_t mantissa_bits) {
  const auto exponent = static_cast<int>(value >> mantissa_bits);
  const auto mantissa = value & ((1u << mantissa_bits) - 1u);

  if (exponent == 0) {
    if (mantissa == 0u) return 0.f;
    return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
  }
  if (exponent == 0x1F) {
    return mantissa == 0u
               ? std::numeric_limits<float>::infinity()
               : std::numeric_limits<float>::quiet_NaN();
  }

  return std::ldexp(1.f + (static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits)), exponent - 15);
}

[[nodiscard]] inline float DecodeR11Component(uint32_t packed) {
  return DecodeUnsignedFloat(packed & 0x7FFu, 6u);
}

[[nodiscard]] inline float DecodeR10Component(uint32_t packed) {
  return DecodeUnsignedFloat(packed & 0x3FFu, 5u);
}

[[nodiscard]] inline float SrgbToLinear(float value) {
  if (value <= 0.04045f) {
    return value / 12.92f;
  }
  return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] inline float LinearToSrgb(float value) {
  if (value <= 0.0031308f) {
    return value * 12.92f;
  }
  return (1.055f * std::pow(value, 1.f / 2.4f)) - 0.055f;
}

[[nodiscard]] inline std::string_view GetPreviewModeName(PreviewMode preview_mode) {
  switch (preview_mode) {
    case PreviewMode::CLAMPED: return "clamped";
    case PreviewMode::NEUTWO:  return "neutwo";
    default:                   return "clamped";
  }
}

[[nodiscard]] inline std::string_view GetSignalName(Signal signal) {
  switch (signal) {
    case Signal::PQ:    return "pq";
    case Signal::SCRGB: return "scrgb";
    default:            return "raw";
  }
}

[[nodiscard]] inline OutputFormat GetOutputFormat(const std::filesystem::path& output_path) {
  return output_path.extension() == L".exr" ? OutputFormat::EXR : OutputFormat::PNG;
}

[[nodiscard]] inline std::string FormatCrc32(uint32_t crc32) {
  return std::format("0x{:08X}", crc32);
}

[[nodiscard]] inline std::string SanitizeFileStem(std::string_view value) {
  std::string result = {};
  result.reserve(value.size());

  bool previous_was_separator = false;
  for (const char character : value) {
    const bool is_alphanumeric = (character >= '0' && character <= '9')
                                 || (character >= 'A' && character <= 'Z')
                                 || (character >= 'a' && character <= 'z');
    const bool keep_character = is_alphanumeric || character == '-' || character == '.';
    if (keep_character) {
      result.push_back(character);
      previous_was_separator = false;
      continue;
    }

    if (!previous_was_separator) {
      result.push_back('_');
      previous_was_separator = true;
    }
  }

  while (!result.empty() && result.front() == '_') {
    result.erase(result.begin());
  }
  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }

  if (result.empty()) {
    return "resource";
  }
  return result;
}

[[nodiscard]] inline std::string MakeSuggestedFileName(
    std::string_view prefix,
    std::string_view format_name,
    uint32_t width,
    uint32_t height,
    uint32_t crc32,
    std::string_view extension) {
  return std::format(
      "{}_{}_{}x{}_{}.{}",
      SanitizeFileStem(prefix),
      SanitizeFileStem(format_name),
      width,
      height,
      FormatCrc32(crc32),
      extension);
}

[[nodiscard]] inline uint32_t ComputeMappedTextureCrc32(
    reshade::api::format format,
    const reshade::api::subresource_data& mapped_data,
    uint32_t width,
    uint32_t height) {
  const auto row_pitch = reshade::api::format_row_pitch(format, width);
  const auto slice_pitch = reshade::api::format_slice_pitch(format, row_pitch, height);
  if (row_pitch == 0u || slice_pitch == 0u || mapped_data.data == nullptr) {
    return 0u;
  }

  if (mapped_data.row_pitch == row_pitch) {
    return renodx::utils::hash::ComputeCRC32(static_cast<const std::uint8_t*>(mapped_data.data), slice_pitch);
  }

  const auto row_count = std::max<std::size_t>(1u, slice_pitch / row_pitch);
  auto crc = 0xFFFFFFFFu;
  const auto* bytes = static_cast<const std::uint8_t*>(mapped_data.data);
  for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
    crc = renodx::utils::hash::UpdateCRC32(
        crc,
        bytes + (static_cast<std::size_t>(mapped_data.row_pitch) * row_index),
        row_pitch);
  }
  return renodx::utils::hash::FinalizeCRC32(crc);
}

[[nodiscard]] inline bool TryBuildRawPngPixels(
    reshade::api::format format,
    const reshade::api::subresource_data& mapped_data,
    uint32_t width,
    uint32_t height,
    std::vector<std::uint8_t>& rgba_pixels) {
  if (mapped_data.data == nullptr || width == 0u || height == 0u) return false;

  rgba_pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
  for (uint32_t y = 0; y < height; ++y) {
    const auto* row = static_cast<const std::uint8_t*>(mapped_data.data)
                      + (static_cast<std::size_t>(mapped_data.row_pitch) * y);
    auto* out_row = rgba_pixels.data() + (static_cast<std::size_t>(width) * static_cast<std::size_t>(y) * 4u);
    switch (format) {
      case reshade::api::format::r8g8b8a8_unorm:
      case reshade::api::format::r8g8b8a8_unorm_srgb:
        std::memcpy(out_row, row, static_cast<std::size_t>(width) * 4u);
        break;
      case reshade::api::format::b8g8r8a8_unorm:
      case reshade::api::format::b8g8r8a8_unorm_srgb:
        for (uint32_t x = 0; x < width; ++x) {
          const auto* pixel = row + (static_cast<std::size_t>(x) * 4u);
          auto* out_pixel = out_row + (static_cast<std::size_t>(x) * 4u);
          out_pixel[0] = pixel[2];
          out_pixel[1] = pixel[1];
          out_pixel[2] = pixel[0];
          out_pixel[3] = pixel[3];
        }
        break;
      case reshade::api::format::r8g8b8x8_unorm:
      case reshade::api::format::r8g8b8x8_unorm_srgb:
        for (uint32_t x = 0; x < width; ++x) {
          const auto* pixel = row + (static_cast<std::size_t>(x) * 4u);
          auto* out_pixel = out_row + (static_cast<std::size_t>(x) * 4u);
          out_pixel[0] = pixel[0];
          out_pixel[1] = pixel[1];
          out_pixel[2] = pixel[2];
          out_pixel[3] = 255u;
        }
        break;
      case reshade::api::format::b8g8r8x8_unorm:
      case reshade::api::format::b8g8r8x8_unorm_srgb:
        for (uint32_t x = 0; x < width; ++x) {
          const auto* pixel = row + (static_cast<std::size_t>(x) * 4u);
          auto* out_pixel = out_row + (static_cast<std::size_t>(x) * 4u);
          out_pixel[0] = pixel[2];
          out_pixel[1] = pixel[1];
          out_pixel[2] = pixel[0];
          out_pixel[3] = 255u;
        }
        break;
      default:
        rgba_pixels.clear();
        return false;
    }
  }

  return true;
}

[[nodiscard]] inline json UploadSignatureToJson(const renodx::utils::resource::ResourceUploadSignature& value) {
  return json{
      {"source", renodx::utils::resource::ResourceUploadSourceName(value.source)},
      {"subresource", value.subresource},
      {"crc32", FormatCrc32(value.crc32)},
      {"crc32Value", value.crc32},
      {"format", std::format("{}", static_cast<std::uint32_t>(value.format))},
      {"width", value.width},
      {"height", value.height},
      {"depthOrLayers", value.depth_or_layers},
      {"rowPitch", value.row_pitch},
      {"slicePitch", value.slice_pitch},
      {"sourceSize", value.source_size},
      {"fullUpdate", value.full_update},
  };
}

[[nodiscard]] inline PreviewMode ParsePreviewMode(std::string_view value) {
  if (value == "clamped") return PreviewMode::CLAMPED;
  if (value == "neutwo") return PreviewMode::NEUTWO;

  throw std::runtime_error(std::format("previewMode '{}' is invalid. Expected 'clamped' or 'neutwo'.", value));
}

[[nodiscard]] inline Signal ParseSignal(std::string_view value) {
  if (value == "raw") return Signal::RAW;
  if (value == "pq") return Signal::PQ;
  if (value == "scrgb") return Signal::SCRGB;

  throw std::runtime_error(std::format("signal '{}' is invalid. Expected 'raw', 'pq' or 'scrgb'.", value));
}

// Optional pixel region from regionX/regionY/regionWidth/regionHeight, which must be given together.
[[nodiscard]] inline std::optional<reshade::api::subresource_box> ParseRegion(const json& arguments) {
  const std::array<std::optional<std::uint32_t>, 4> region_values = {
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "regionX"),
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "regionY"),
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "regionWidth"),
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "regionHeight"),
  };
  const auto region_value_count = std::ranges::count_if(region_values, [](const auto& value) { return value.has_value(); });
  if (region_value_count != 0 && region_value_count != 4) {
    throw std::runtime_error("regionX, regionY, regionWidth and regionHeight must be given together.");
  }
  std::optional<reshade::api::subresource_box> region = std::nullopt;
  if (region_value_count == 4) {
    if (*region_values[2] == 0u || *region_values[3] == 0u) {
      throw std::runtime_error("regionWidth and regionHeight must be greater than 0.");
    }
    if (*region_values[2] > (std::numeric_limits<std::uint32_t>::max)() - *region_values[0]
        || *region_values[3] > (std::numeric_limits<std::uint32_t>::max)() - *region_values[1]) {
      throw std::runtime_error("The region is out of range.");
    }
    region = reshade::api::subresource_box{
        .left = *region_values[0],
        .top = *region_values[1],
        .front = 0u,
        .right = *region_values[0] + *region_values[2],
        .bottom = *region_values[1] + *region_values[3],
        .back = 1u,
    };
  }
  return region;
}

// Display light of a decoded pixel for a PQ or scRGB signal. PQ: decoded values are the ST 2084 encoding of BT.2020
// light. scRGB: 1.0 = 80 nits, BT.709 primaries. Shared by the CPU analysis and the watch_region verification.
struct DisplayLight {
  std::array<double, 3> nits = {};
  double luminance = 0.0;
};

[[nodiscard]] inline DisplayLight DecodeDisplayLight(Signal signal, const float rgba[4]) {
  DisplayLight light = {};
  if (signal == Signal::PQ) {
    for (uint32_t channel = 0; channel < 3u; ++channel) {
      const auto encoded_power = std::pow(std::clamp(static_cast<double>(rgba[channel]), 0.0, 1.0), 1.0 / 78.84375);
      const auto linear_ratio = std::max(encoded_power - 0.8359375, 0.0) / (18.8515625 - (18.6875 * encoded_power));
      light.nits[channel] = 10000.0 * std::pow(linear_ratio, 1.0 / 0.1593017578125);
    }
    light.luminance = (0.2627002 * light.nits[0]) + (0.6779981 * light.nits[1]) + (0.0593017 * light.nits[2]);
    return light;
  }
  for (uint32_t channel = 0; channel < 3u; ++channel) {
    light.nits[channel] = 80.0 * rgba[channel];
  }
  light.luminance = (0.2126 * light.nits[0]) + (0.7152 * light.nits[1]) + (0.0722 * light.nits[2]);
  return light;
}

[[nodiscard]] inline float ToneMapNeutwo(float value) {
  value = std::max(value, 0.f);
  return value / std::sqrt(std::fma(value, value, 1.f));
}

[[nodiscard]] inline float ComputeNeutwoMaxChannelScale(const float rgb[3]) {
  const auto max_channel = std::max(std::abs(rgb[0]), std::max(std::abs(rgb[1]), std::abs(rgb[2])));
  if (max_channel <= 0.f) return 1.f;

  const auto new_max = ToneMapNeutwo(max_channel);
  return new_max / max_channel;
}

[[nodiscard]] inline std::uint8_t EncodePreviewChannel(float value) {
  value = std::clamp(LinearToSrgb(value), 0.f, 1.f);
  return static_cast<uint8_t>(std::lround(value * 255.f));
}

inline void EncodePreviewPixel(
    const float rgba[4],
    PreviewMode preview_mode,
    std::uint8_t out_rgba[4]) {
  if (!std::isfinite(rgba[0]) || !std::isfinite(rgba[1]) || !std::isfinite(rgba[2])) {
    out_rgba[0] = 255u;
    out_rgba[1] = 0u;
    out_rgba[2] = 255u;
    out_rgba[3] = 255u;
    return;
  }

  float rgb[3] = {rgba[0], rgba[1], rgba[2]};
  if (preview_mode == PreviewMode::NEUTWO) {
    const auto scale = ComputeNeutwoMaxChannelScale(rgb);
    rgb[0] *= scale;
    rgb[1] *= scale;
    rgb[2] *= scale;
  } else {
    rgb[0] = std::clamp(rgb[0], 0.f, 1.f);
    rgb[1] = std::clamp(rgb[1], 0.f, 1.f);
    rgb[2] = std::clamp(rgb[2], 0.f, 1.f);
  }

  out_rgba[0] = EncodePreviewChannel(std::max(rgb[0], 0.f));
  out_rgba[1] = EncodePreviewChannel(std::max(rgb[1], 0.f));
  out_rgba[2] = EncodePreviewChannel(std::max(rgb[2], 0.f));

  const auto alpha = std::isfinite(rgba[3]) ? std::clamp(rgba[3], 0.f, 1.f) : 1.f;
  out_rgba[3] = static_cast<uint8_t>(std::lround(alpha * 255.f));
}

[[nodiscard]] inline bool WritePngPreview(
    const std::filesystem::path& output_path,
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& rgba_pixels) {
  return renodx::utils::png::WriteRgba8(output_path, width, height, rgba_pixels);
}

[[nodiscard]] inline bool DecodePixelToLinear(
    reshade::api::format format,
    const uint8_t* pixel,
    float rgba[4],
    uint32_t& channel_count) {
  channel_count = 4u;

  switch (format) {
    case reshade::api::format::r8g8b8a8_unorm: {
      rgba[0] = pixel[0] / 255.f;
      rgba[1] = pixel[1] / 255.f;
      rgba[2] = pixel[2] / 255.f;
      rgba[3] = pixel[3] / 255.f;
      return true;
    }
    case reshade::api::format::r8g8b8a8_unorm_srgb: {
      rgba[0] = SrgbToLinear(pixel[0] / 255.f);
      rgba[1] = SrgbToLinear(pixel[1] / 255.f);
      rgba[2] = SrgbToLinear(pixel[2] / 255.f);
      rgba[3] = pixel[3] / 255.f;
      return true;
    }
    case reshade::api::format::b8g8r8a8_unorm: {
      rgba[0] = pixel[2] / 255.f;
      rgba[1] = pixel[1] / 255.f;
      rgba[2] = pixel[0] / 255.f;
      rgba[3] = pixel[3] / 255.f;
      return true;
    }
    case reshade::api::format::b8g8r8a8_unorm_srgb: {
      rgba[0] = SrgbToLinear(pixel[2] / 255.f);
      rgba[1] = SrgbToLinear(pixel[1] / 255.f);
      rgba[2] = SrgbToLinear(pixel[0] / 255.f);
      rgba[3] = pixel[3] / 255.f;
      return true;
    }
    case reshade::api::format::r8g8b8x8_unorm: {
      rgba[0] = pixel[0] / 255.f;
      rgba[1] = pixel[1] / 255.f;
      rgba[2] = pixel[2] / 255.f;
      rgba[3] = 1.f;
      return true;
    }
    case reshade::api::format::r8g8b8x8_unorm_srgb: {
      rgba[0] = SrgbToLinear(pixel[0] / 255.f);
      rgba[1] = SrgbToLinear(pixel[1] / 255.f);
      rgba[2] = SrgbToLinear(pixel[2] / 255.f);
      rgba[3] = 1.f;
      return true;
    }
    case reshade::api::format::b8g8r8x8_unorm: {
      rgba[0] = pixel[2] / 255.f;
      rgba[1] = pixel[1] / 255.f;
      rgba[2] = pixel[0] / 255.f;
      rgba[3] = 1.f;
      return true;
    }
    case reshade::api::format::b8g8r8x8_unorm_srgb: {
      rgba[0] = SrgbToLinear(pixel[2] / 255.f);
      rgba[1] = SrgbToLinear(pixel[1] / 255.f);
      rgba[2] = SrgbToLinear(pixel[0] / 255.f);
      rgba[3] = 1.f;
      return true;
    }
    case reshade::api::format::r10g10b10a2_unorm: {
      const auto packed = *reinterpret_cast<const uint32_t*>(pixel);
      rgba[0] = ((packed >> 0) & 0x3FFu) / 1023.f;
      rgba[1] = ((packed >> 10) & 0x3FFu) / 1023.f;
      rgba[2] = ((packed >> 20) & 0x3FFu) / 1023.f;
      rgba[3] = ((packed >> 30) & 0x3u) / 3.f;
      return true;
    }
    case reshade::api::format::b10g10r10a2_unorm: {
      const auto packed = *reinterpret_cast<const uint32_t*>(pixel);
      rgba[2] = ((packed >> 0) & 0x3FFu) / 1023.f;
      rgba[1] = ((packed >> 10) & 0x3FFu) / 1023.f;
      rgba[0] = ((packed >> 20) & 0x3FFu) / 1023.f;
      rgba[3] = ((packed >> 30) & 0x3u) / 3.f;
      return true;
    }
    case reshade::api::format::r11g11b10_float: {
      const auto packed = *reinterpret_cast<const uint32_t*>(pixel);
      rgba[0] = DecodeR11Component((packed >> 0) & 0x7FFu);
      rgba[1] = DecodeR11Component((packed >> 11) & 0x7FFu);
      rgba[2] = DecodeR10Component((packed >> 22) & 0x3FFu);
      rgba[3] = 1.f;
      return true;
    }
    case reshade::api::format::r16_float: {
      rgba[0] = DecodeFloat16(*reinterpret_cast<const uint16_t*>(pixel));
      rgba[1] = 0.f;
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 1u;
      return true;
    }
    case reshade::api::format::r8_unorm: {
      rgba[0] = pixel[0] / 255.f;
      rgba[1] = 0.f;
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 1u;
      return true;
    }
    case reshade::api::format::r16_unorm: {
      rgba[0] = *reinterpret_cast<const uint16_t*>(pixel) / 65535.f;
      rgba[1] = 0.f;
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 1u;
      return true;
    }
    case reshade::api::format::r32_float: {
      rgba[0] = *reinterpret_cast<const float*>(pixel);
      rgba[1] = 0.f;
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 1u;
      return true;
    }
    case reshade::api::format::r8g8_unorm: {
      rgba[0] = pixel[0] / 255.f;
      rgba[1] = pixel[1] / 255.f;
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 2u;
      return true;
    }
    case reshade::api::format::r16g16_unorm: {
      const auto* packed = reinterpret_cast<const uint16_t*>(pixel);
      rgba[0] = packed[0] / 65535.f;
      rgba[1] = packed[1] / 65535.f;
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 2u;
      return true;
    }
    case reshade::api::format::r16g16_float: {
      const auto* packed = reinterpret_cast<const uint16_t*>(pixel);
      rgba[0] = DecodeFloat16(packed[0]);
      rgba[1] = DecodeFloat16(packed[1]);
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 2u;
      return true;
    }
    case reshade::api::format::r32g32_float: {
      const auto* packed = reinterpret_cast<const float*>(pixel);
      rgba[0] = packed[0];
      rgba[1] = packed[1];
      rgba[2] = 0.f;
      rgba[3] = 1.f;
      channel_count = 2u;
      return true;
    }
    case reshade::api::format::r9g9b9e5: {
      // Three 9-bit mantissas sharing one 5-bit exponent (bias 15), no implicit leading one.
      const auto packed = *reinterpret_cast<const uint32_t*>(pixel);
      const auto exponent = static_cast<int>(packed >> 27u) - 15 - 9;
      rgba[0] = std::ldexp(static_cast<float>((packed >> 0u) & 0x1FFu), exponent);
      rgba[1] = std::ldexp(static_cast<float>((packed >> 9u) & 0x1FFu), exponent);
      rgba[2] = std::ldexp(static_cast<float>((packed >> 18u) & 0x1FFu), exponent);
      rgba[3] = 1.f;
      return true;
    }
    case reshade::api::format::r16g16b16a16_float: {
      const auto* packed = reinterpret_cast<const uint16_t*>(pixel);
      rgba[0] = DecodeFloat16(packed[0]);
      rgba[1] = DecodeFloat16(packed[1]);
      rgba[2] = DecodeFloat16(packed[2]);
      rgba[3] = DecodeFloat16(packed[3]);
      return true;
    }
    case reshade::api::format::r16g16b16a16_unorm: {
      const auto* packed = reinterpret_cast<const uint16_t*>(pixel);
      rgba[0] = packed[0] / 65535.f;
      rgba[1] = packed[1] / 65535.f;
      rgba[2] = packed[2] / 65535.f;
      rgba[3] = packed[3] / 65535.f;
      return true;
    }
    case reshade::api::format::r32g32b32a32_float: {
      const auto* packed = reinterpret_cast<const float*>(pixel);
      rgba[0] = packed[0];
      rgba[1] = packed[1];
      rgba[2] = packed[2];
      rgba[3] = packed[3];
      return true;
    }
    default:
      return false;
  }
}

[[nodiscard]] inline ToolResult PerformResourceAnalysis(
    const ProcessingContext& context,
    std::uint32_t device_index,
    reshade::api::device* device,
    reshade::api::command_queue* queue,
    reshade::api::resource_view resource_view,
    const std::optional<std::filesystem::path>& output_path,
    const std::optional<std::filesystem::path>& output_directory,
    const std::optional<std::string>& name_prefix,
    PreviewMode preview_mode,
    std::string_view preview_mode_name,
    bool prefer_clone,
    const std::optional<reshade::api::subresource_box>& region,
    Signal signal,
    reshade::api::resource frame_end_copy,
    // When set, receives one luminance per pixel in row order (nits with a signal, else BT.709 luminance of the decoded
    // values; NaN for a non-finite pixel), for the tone curve's input/output pairs.
    std::vector<float>* pixel_luminance = nullptr,
    // When set, the texture is read as a colour lookup table (devkit_analyze_lut) and LUT statistics are returned.
    const LutOptions* lut = nullptr,
    bool measure_banding = false) {
  AnalysisTarget target = {};
  try {
    target = ResolveAnalysisTarget(resource_view, device, prefer_clone);
  } catch (const std::exception& exception) {
    auto error = std::string(exception.what());
    if (error.starts_with("resourceViewHandle ")) {
      error = std::format("resourceViewHandle {} is not currently tracked.", FormatHandle(context, resource_view.handle));
    }
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }

  // ReShade reads R9G9B9E5 back on OpenGL as GL_RGBA with GL_UNSIGNED_INT_5_9_9_9_REV, a pairing the GL spec only
  // allows with GL_RGB, so the readback would hold whatever memory the failed glGetTexImage left behind.
  if (device->get_api() == reshade::api::device_api::opengl
      && reshade::api::format_to_default_typed(target.resolved_resource_desc.texture.format) == reshade::api::format::r9g9b9e5) {
    return ToolResult{
        .text = "R9G9B9E5 textures cannot be read back on OpenGL.",
        .structured_content = json{{"error", "R9G9B9E5 textures cannot be read back on OpenGL."}},
        .is_error = true,
    };
  }
  if (lut == nullptr
      && target.resolved_resource_desc.type != reshade::api::resource_type::texture_2d
      && target.resolved_resource_desc.type != reshade::api::resource_type::surface) {
    return ToolResult{
        .text = "Only 2D texture resource views are currently supported.",
        .structured_content = json{{"error", "Only 2D texture resource views are currently supported."}},
        .is_error = true,
    };
  }
  // Copying a Vulkan image without TRANSFER_SRC usage is invalid and can lose the device; same rule as
  // devkit_watch_region.
  if (device->get_api() == reshade::api::device_api::vulkan
      && !renodx::utils::bitwise::HasAnyFlag(
          target.resolved_resource_desc.usage, reshade::api::resource_usage::copy_source)) {
    const auto error = std::format(
        "The {} was created without VK_IMAGE_USAGE_TRANSFER_SRC_BIT, so it cannot be copied for analysis. The Devkit "
        "setting \"Watch Region: Copyable Vulkan Images\" (Settings > Other) adds it and is on by default; if it was "
        "turned off, turn it on and restart the game{}. The setting skips shared and sparse images and formats without "
        "copy support, and cannot reach images created before the Devkit loaded or fallback copies a mod creates from a "
        "desc recorded before the Devkit's handler ran.",
        (target.used_clone ? "clone" : "image"),
        (target.used_clone ? "" : ", or analyze its clone (preferClone: true) when a mod clones it"));
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }
  const auto sample_count = target.resolved_resource_desc.texture.samples;
  // The resolve keeps the source's own format (an sRGB target stays sRGB) because D3D10/11 drop a resolve whose typed
  // formats differ. A typeless source is resolved the way the view reads it (sRGB or not), else as its default typed
  // format.
  const auto resource_format = target.resolved_resource_desc.texture.format;
  const auto view_format = target.resolved_view_desc.format;
  const bool view_types_resource = view_format != reshade::api::format::unknown
                                   && !reshade::api::format_is_typeless(view_format)
                                   && reshade::api::format_to_typeless(view_format) == reshade::api::format_to_typeless(resource_format);
  const auto resolve_format = (reshade::api::format_is_typeless(resource_format) && view_types_resource
                                   ? view_format
                                   : reshade::api::format_to_default_typed(resource_format));
  if (sample_count != 1u) {
    std::optional<std::string> multisample_error = std::nullopt;
    switch (device->get_api()) {
      case reshade::api::device_api::d3d12:
      case reshade::api::device_api::vulkan:
        // A resolve needs the source in its resolve-source state, which the tool does not know on these APIs.
        multisample_error = "Multisampled resources can only be analyzed on D3D9, D3D10/11 and OpenGL.";
        break;
      default:
        if (IsDepthStencilResource(device, target.resolved_resource, target.resolved_resource_desc)) {
          multisample_error = "Multisampled depth-stencil resources cannot be resolved for analysis.";
        } else if (device->get_api() == reshade::api::device_api::opengl
                   && static_cast<uint32_t>(target.resolved_resource.handle >> 40u) == 0x8218u /* GL_FRAMEBUFFER_DEFAULT */) {
          // ReShade's GL resolve is a framebuffer blit that flips rows of the default framebuffer, and a blit from a
          // multisampled framebuffer must keep identical source and destination bounds.
          multisample_error = "A multisampled OpenGL default framebuffer cannot be resolved for analysis; analyze the texture the game renders into before presenting.";
        } else if (!device->check_format_support(resolve_format, reshade::api::resource_usage::resolve_dest)) {
          // Integer formats cannot be resolved; the runtime would drop the call and the readback would be empty.
          multisample_error = "The format of this multisampled resource cannot be resolved for analysis.";
        }
        break;
    }
    if (multisample_error.has_value()) {
      return ToolResult{
          .text = multisample_error.value(),
          .structured_content = json{{"error", multisample_error.value()}},
          .is_error = true,
      };
    }
  }

  if (lut != nullptr) {
    const auto make_error = [](const std::string& error) {
      return ToolResult{
          .text = error,
          .structured_content = json{{"error", error}},
          .is_error = true,
      };
    };
    const auto& lut_desc = target.resolved_resource_desc;
    const bool is_volume = (lut_desc.type == reshade::api::resource_type::texture_3d);
    if (!is_volume && lut_desc.type != reshade::api::resource_type::texture_2d) {
      return make_error("A LUT is a 3D texture or a 2D strip.");
    }
    if (lut_desc.texture.samples != 1u) return make_error("A multisampled texture is not a LUT.");
    if (is_volume && device->get_api() == reshade::api::device_api::d3d9) {
      return make_error("3D LUTs are not read on D3D9 (ReShade does not copy volume textures there); 2D strip LUTs are.");
    }
    // ReShade reads textures back on Vulkan through LINEAR-tiled images, which Vulkan guarantees only for 2D.
    if (is_volume && device->get_api() == reshade::api::device_api::vulkan) {
      return make_error("3D LUTs are not read on Vulkan (a 3D readback image needs LINEAR tiling, which drivers need not support); 2D strip LUTs are.");
    }
    if (!is_volume && lut_desc.texture.depth_or_layers > 1u) return make_error("Array and cube textures are not read as LUTs.");
    // A 3D view's first_layer is its first depth slice (a per-slice render target of a LUT built slice by slice); the
    // whole volume is read whatever slice the view selects.
    if (target.resolved_view_desc.texture.first_level != 0u || (!is_volume && target.resolved_view_desc.texture.first_layer != 0u)) {
      return make_error("Only the top mip level is read as a LUT: this view selects another mip level or array layer.");
    }
    const auto lut_width = lut_desc.texture.width;
    const auto lut_height = lut_desc.texture.height;
    const auto lut_depth = (is_volume ? static_cast<uint32_t>(lut_desc.texture.depth_or_layers) : 1u);
    auto layout = lut->layout;
    if (layout == "auto") {
      if (is_volume) {
        layout = "3d";
      } else if (lut_width == lut_height * lut_height) {
        layout = "strip_x";
      } else if (lut_height == lut_width * lut_width) {
        layout = "strip_y";
      } else {
        return make_error(std::format(
            "A {}x{} 2D texture is neither N*N x N nor N x N*N; pass layout if it is a LUT anyway.",
            lut_width,
            lut_height));
      }
    }
    uint32_t lut_size = 0u;
    if (layout == "3d" && is_volume && lut_width == lut_height && lut_height == lut_depth) {
      lut_size = lut_width;
    } else if (layout == "strip_x" && !is_volume && lut_width == lut_height * lut_height) {
      lut_size = lut_height;
    } else if (layout == "strip_y" && !is_volume && lut_height == lut_width * lut_width) {
      lut_size = lut_width;
    }
    if (lut_size < 2u || lut_size > 256u) {
      return make_error(std::format(
          "Layout '{}' does not fit this {} texture of {}x{}x{}.",
          layout,
          (is_volume ? "3D" : "2D"),
          lut_width,
          lut_height,
          lut_depth));
    }
    const bool layout_volume = (layout == "3d");
    const bool layout_strip_x = (layout == "strip_x");

    // lattice[(b * N + g) * N + r] = the RGB the shader reads for input (r, g, b) / (N - 1). Allocated before the
    // readback exists, so a failed allocation cannot leave it mapped.
    const auto lattice_index = [lut_size](uint32_t r, uint32_t g, uint32_t b) {
      return ((static_cast<std::size_t>(b) * lut_size) + g) * lut_size + r;
    };
    std::vector<std::array<float, 3>> lattice(static_cast<std::size_t>(lut_size) * lut_size * lut_size);
    std::vector<float> point_deviation;
    point_deviation.reserve(lattice.size());

    // The readback keeps the resource's own format (an sRGB resource stays sRGB): Vulkan copies only between equal
    // formats and blits otherwise, which would decode sRGB once before DecodePixelToLinear decodes it again. The bits
    // are then decoded the way the view reads them when its format shares the resource's typeless family (a typeless
    // resource, or an sRGB view of a mutable UNORM image).
    const auto readback_format = reshade::api::format_to_default_typed(lut_desc.texture.format);
    const auto view_format = target.resolved_view_desc.format;
    const auto decode_format = (view_format != reshade::api::format::unknown
                                        && reshade::api::format_to_typeless(view_format) == reshade::api::format_to_typeless(readback_format)
                                    ? view_format
                                    : readback_format);
    reshade::api::resource readback = {0u};
    if (!device->create_resource(
            reshade::api::resource_desc(
                lut_desc.type,
                lut_width,
                lut_height,
                static_cast<uint16_t>(lut_depth),
                1,
                readback_format,
                1,
                reshade::api::memory_heap::gpu_to_cpu,
                reshade::api::resource_usage::copy_dest),
            nullptr,
            reshade::api::resource_usage::copy_dest,
            &readback)) {
      return make_error("Failed to create a GPU-to-CPU readback texture for the LUT.");
    }
    queue->get_immediate_command_list()->copy_texture_region(target.resolved_resource, 0, nullptr, readback, 0, nullptr);
    queue->flush_immediate_command_list();
    queue->wait_idle();

    reshade::api::subresource_data mapped = {};
    if (!device->map_texture_region(readback, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
      device->destroy_resource(readback);
      return make_error("Failed to map the LUT readback texture.");
    }
    const auto texel_stride = static_cast<std::size_t>(reshade::api::format_row_pitch(readback_format, 1));
    uint32_t channel_count = 4u;
    bool decoded = true;
    for (uint32_t b = 0u; b < lut_size && decoded; ++b) {
      for (uint32_t g = 0u; g < lut_size && decoded; ++g) {
        for (uint32_t r = 0u; r < lut_size; ++r) {
          uint32_t x = r;
          uint32_t y = g;
          uint32_t z = 0u;
          if (layout_volume) {
            z = b;
          } else if (layout_strip_x) {
            x = (b * lut_size) + r;
          } else {
            y = (b * lut_size) + g;
          }
          // flipY: the whole image is stored bottom-up (for strip_y that also reverses the slice order).
          if (lut->flip_y) {
            y = (layout_volume || layout_strip_x ? lut_size : lut_size * lut_size) - 1u - y;
          }
          const auto* texel = static_cast<const uint8_t*>(mapped.data)
                              + (static_cast<std::size_t>(z) * mapped.slice_pitch)
                              + (static_cast<std::size_t>(y) * mapped.row_pitch)
                              + (static_cast<std::size_t>(x) * texel_stride);
          float rgba[4] = {0.f, 0.f, 0.f, 1.f};
          if (!DecodePixelToLinear(decode_format, texel, rgba, channel_count)) {
            decoded = false;
            break;
          }
          lattice[lattice_index(r, g, b)] = {rgba[0], rgba[1], rgba[2]};
        }
      }
    }
    device->unmap_texture_region(readback, 0);
    device->destroy_resource(readback);
    if (!decoded) {
      return make_error(std::format("Readback is not implemented for format {}.", FormatFormat(context, decode_format)));
    }
    if (channel_count < 3u) {
      return make_error(std::format(
          "Format {} has fewer than three channels, so it is not a colour LUT.",
          FormatFormat(context, decode_format)));
    }

    // An sRGB view decodes texels to linear; the identity of such a LUT holds in the stored (encoded) values.
    const bool srgb_format = (decode_format == reshade::api::format_to_default_typed(decode_format, 1)
                              && decode_format != reshade::api::format_to_default_typed(decode_format, 0));
    const auto lattice_step = static_cast<float>(lut_size - 1u);
    std::array<double, 3> deviation_sum = {};
    std::array<double, 3> deviation_max = {};
    std::array<float, 3> output_min = {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
    };
    std::array<float, 3> output_max = {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    std::array<std::size_t, 3> finite_values = {};
    std::size_t worst_index = 0u;
    float worst_deviation = -1.f;
    std::size_t values_above_one = 0u;
    std::size_t non_finite_values = 0u;
    bool any_nonzero = false;
    for (uint32_t b = 0u; b < lut_size; ++b) {
      for (uint32_t g = 0u; g < lut_size; ++g) {
        for (uint32_t r = 0u; r < lut_size; ++r) {
          const auto index = lattice_index(r, g, b);
          const auto& output = lattice[index];
          const std::array<float, 3> input = {r / lattice_step, g / lattice_step, b / lattice_step};
          float point_max = 0.f;
          bool point_finite = false;
          for (std::size_t channel = 0u; channel < 3u; ++channel) {
            const auto value = output[channel];
            if (!std::isfinite(value)) {
              ++non_finite_values;
              continue;
            }
            ++finite_values[channel];
            point_finite = true;
            any_nonzero |= (value != 0.f);
            if (value > 1.f) ++values_above_one;
            output_min[channel] = std::min(output_min[channel], value);
            output_max[channel] = std::max(output_max[channel], value);
            const auto stored = (srgb_format ? LinearToSrgb(value) : value);
            const auto deviation = std::abs(stored - input[channel]);
            deviation_sum[channel] += deviation;
            deviation_max[channel] = std::max<double>(deviation_max[channel], deviation);
            point_max = std::max(point_max, deviation);
          }
          // A point without any finite channel has no deviation; it is counted in nonFiniteValues instead.
          if (!point_finite) continue;
          point_deviation.push_back(point_max);
          if (point_max > worst_deviation) {
            worst_deviation = point_max;
            worst_index = index;
          }
        }
      }
    }
    std::ranges::sort(point_deviation);
    const auto deviation_p99 = (point_deviation.empty()
                                    ? 0.f
                                    : point_deviation[std::min(point_deviation.size() - 1u, (point_deviation.size() * 99u) / 100u)]);
    const auto lattice_point_json = [&](uint32_t r, uint32_t g, uint32_t b) {
      const auto& output = lattice[lattice_index(r, g, b)];
      return json{
          {"index", {r, g, b}},
          {"input", {r / lattice_step, g / lattice_step, b / lattice_step}},
          {"output", {output[0], output[1], output[2]}},
      };
    };
    const auto last = lut_size - 1u;
    json neutral_axis = json::array();
    std::array<bool, 3> neutral_monotonic = {true, true, true};
    for (uint32_t i = 0u; i < lut_size; ++i) {
      const auto& output = lattice[lattice_index(i, i, i)];
      neutral_axis.push_back(json{{"input", i / lattice_step}, {"output", {output[0], output[1], output[2]}}});
      if (i == 0u) continue;
      const auto& previous = lattice[lattice_index(i - 1u, i - 1u, i - 1u)];
      for (std::size_t channel = 0u; channel < 3u; ++channel) {
        neutral_monotonic[channel] = neutral_monotonic[channel] && (output[channel] >= previous[channel] - 1e-4f);
      }
    }
    const bool all_zero = (!any_nonzero && non_finite_values == 0u);
    // Within about 1.5 steps of an 8-bit value: an identity LUT stored in 8 bits. A non-finite value never is.
    constexpr double IDENTITY_TOLERANCE = 1.5 / 255.0;
    const bool is_identity = (non_finite_values == 0u && std::ranges::max(deviation_max) <= IDENTITY_TOLERANCE);
    const auto mean_deviation = [&](std::size_t channel) {
      return (finite_values[channel] == 0u ? 0.0 : deviation_sum[channel] / static_cast<double>(finite_values[channel]));
    };

    // The CSV is written on the game's thread: above 65^3 points it would stall the game for long, so it is skipped.
    constexpr uint32_t MAX_CSV_LUT_SIZE = 65u;
    json csv_json = nullptr;
    json csv_error = nullptr;
    if (lut->csv_path.has_value() && lut_size > MAX_CSV_LUT_SIZE) {
      csv_error = std::format("Not written: N = {} is above {}.", lut_size, MAX_CSV_LUT_SIZE);
    } else if (lut->csv_path.has_value()) {
      std::ofstream csv_file(lut->csv_path.value(), std::ios::trunc);
      csv_file << "r_index,g_index,b_index,in_r,in_g,in_b,out_r,out_g,out_b\n";
      for (uint32_t b = 0u; b < lut_size; ++b) {
        for (uint32_t g = 0u; g < lut_size; ++g) {
          for (uint32_t r = 0u; r < lut_size; ++r) {
            const auto& output = lattice[lattice_index(r, g, b)];
            csv_file << std::format(
                "{},{},{},{:.6g},{:.6g},{:.6g},{:.9g},{:.9g},{:.9g}\n",
                r,
                g,
                b,
                r / lattice_step,
                g / lattice_step,
                b / lattice_step,
                output[0],
                output[1],
                output[2]);
          }
        }
      }
      csv_file.close();
      if (csv_file) {
        csv_json = lut->csv_path->string();
      } else {
        csv_error = std::format("Failed to write '{}'.", lut->csv_path->string());
      }
    }

    json result = {
        {"deviceIndex", device_index},
        {"resourceViewHandle", FormatHandle(context, resource_view.handle)},
        {"usedClone", target.used_clone},
        {"layout", layout},
        {"size", lut_size},
        {"flipY", lut->flip_y},
        {"format", FormatFormat(context, decode_format)},
        {"srgbFormat", srgb_format},
        {"identityDomain", (srgb_format ? "stored (sRGB-encoded) values" : "values as read")},
        {"isIdentity", is_identity},
        {"identityDeviation",
         {
             {"mean", {mean_deviation(0u), mean_deviation(1u), mean_deviation(2u)}},
             {"max", {deviation_max[0], deviation_max[1], deviation_max[2]}},
             {"p99", deviation_p99},
             {"worst",
              lattice_point_json(
                  static_cast<uint32_t>(worst_index % lut_size),
                  static_cast<uint32_t>((worst_index / lut_size) % lut_size),
                  static_cast<uint32_t>(worst_index / (static_cast<std::size_t>(lut_size) * lut_size)))},
         }},
        // The eight cube corners show at a glance whether the axis mapping is right: a swapped axis or a flipped green
        // moves a primary to another corner.
        {"corners",
         {
             {"black", lattice_point_json(0u, 0u, 0u)},
             {"red", lattice_point_json(last, 0u, 0u)},
             {"green", lattice_point_json(0u, last, 0u)},
             {"blue", lattice_point_json(0u, 0u, last)},
             {"yellow", lattice_point_json(last, last, 0u)},
             {"magenta", lattice_point_json(last, 0u, last)},
             {"cyan", lattice_point_json(0u, last, last)},
             {"white", lattice_point_json(last, last, last)},
         }},
        {"neutralAxis", neutral_axis},
        {"neutralAxisMonotonic", {neutral_monotonic[0], neutral_monotonic[1], neutral_monotonic[2]}},
        {"outputRange",
         {
             {"min", {output_min[0], output_min[1], output_min[2]}},
             {"max", {output_max[0], output_max[1], output_max[2]}},
         }},
        {"valuesAboveOne", values_above_one},
        {"nonFiniteValues", non_finite_values},
        {"allZero", all_zero},
        {"csvPath", csv_json},
        {"csvError", csv_error},
    };
    auto text = std::format(
        "{} LUT, N = {}, {}: {}. Identity deviation max (r, g, b) = ({:.4f}, {:.4f}, {:.4f}), p99 {:.4f}. Output range "
        "[{:.4g}, {:.4g}]; neutral axis {}.",
        layout,
        lut_size,
        FormatFormat(context, decode_format),
        (is_identity ? "identity" : "not identity"),
        deviation_max[0],
        deviation_max[1],
        deviation_max[2],
        deviation_p99,
        std::ranges::min(output_min),
        std::ranges::max(output_max),
        ((neutral_monotonic[0] && neutral_monotonic[1] && neutral_monotonic[2]) ? "monotonic" : "NOT monotonic"));
    if (all_zero) {
      text += " Every value is zero: the LUT may be built later in the frame, or the texture reused after the LUT pass.";
    }
    if (non_finite_values != 0u) {
      text += std::format(" {} values are NaN or infinite.", non_finite_values);
    }
    if (!csv_error.is_null()) {
      text += std::format(" CSV: {}", csv_error.get<std::string>());
    }
    return ToolResult{
        .text = std::move(text),
        .structured_content = std::move(result),
    };
  }

  const auto summary_view = target.used_clone ? target.resolved_view : resource_view;
  const auto resource_view_summary_value = context.build_resource_view_summary
                                               ? std::optional<resource_view_summary::ResourceViewSummary>(
                                                     context.build_resource_view_summary(summary_view, device))
                                               : std::nullopt;
  const auto upload_resource = [&]() {
    if (const auto resource_from_view = renodx::utils::resource::GetResourceFromView(resource_view);
        resource_from_view.handle != 0u) {
      return resource_from_view;
    }
    return target.resolved_resource;
  }();

  const auto first_level = target.resolved_view_desc.texture.first_level;
  const auto first_layer = target.resolved_view_desc.texture.first_layer;
  const auto levels = std::max<uint32_t>(target.resolved_resource_desc.texture.levels, 1u);
  const auto subresource = first_level + (first_layer * levels);
  const auto texture_width = std::max<uint32_t>(target.resolved_resource_desc.texture.width >> first_level, 1u);
  const auto texture_height = std::max<uint32_t>(target.resolved_resource_desc.texture.height >> first_level, 1u);
  // D3D10/11 drop and D3D12 rejects a box copy of a depth-stencil resource: only the whole subresource is read.
  if (region.has_value() && IsDepthStencilResource(device, target.resolved_resource, target.resolved_resource_desc)) {
    return ToolResult{
        .text = "A depth-stencil resource is read whole: leave the region out.",
        .structured_content = json{{"error", "A depth-stencil resource is read whole: leave the region out."}},
        .is_error = true,
    };
  }
  if (region.has_value() && (region->right > texture_width || region->bottom > texture_height)) {
    const auto error = std::format(
        "The region {}x{} at ({}, {}) does not fit the {}x{} texture.",
        region->width(),
        region->height(),
        region->left,
        region->top,
        texture_width,
        texture_height);
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }
  // With a region, only that box is copied, so every statistic, preview and dump below describes the region.
  const auto width = (region.has_value() ? region->width() : texture_width);
  const auto height = (region.has_value() ? region->height() : texture_height);
  const auto intermediate_format = reshade::api::format_to_default_typed(target.resolved_resource_desc.texture.format, 0);
  const auto analysis_format = target.resolved_view_desc.format != reshade::api::format::unknown
                                   ? target.resolved_view_desc.format
                                   : intermediate_format;

  reshade::api::resource intermediate = {0u};
  if (!device->create_resource(
          reshade::api::resource_desc(
              width,
              height,
              1,
              1,
              intermediate_format,
              1,
              reshade::api::memory_heap::gpu_to_cpu,
              reshade::api::resource_usage::copy_dest),
          nullptr,
          reshade::api::resource_usage::copy_dest,
          &intermediate)) {
    return ToolResult{
        .text = "Failed to create a GPU-to-CPU readback resource.",
        .structured_content = json{{"error", "Failed to create a GPU-to-CPU readback resource."}},
        .is_error = true,
    };
  }

  auto* cmd_list = queue->get_immediate_command_list();
  const auto* source_box = (region.has_value() ? &region.value() : nullptr);
  // A multisampled texture cannot be copied into the single-sample readback texture: it is first resolved (averaged)
  // into a single-sample texture, which is then read like any other. The whole subresource is resolved (D3D10/11 cannot
  // resolve a box, and an OpenGL multisample blit must keep identical bounds), then the region is copied out of it.
  reshade::api::resource copy_source = target.resolved_resource;
  uint32_t copy_source_subresource = subresource;
  if (frame_end_copy.handle != 0u && !target.used_clone) {
    // Vulkan frame_end: the subresource was copied at reshade_present; the swap-chain image itself is presented by now.
    // A resolved clone is read as it is: the copy holds the original back buffer, possibly in another format.
    copy_source = frame_end_copy;
    copy_source_subresource = 0u;
  }
  reshade::api::resource resolved_multisample = {0u};
  if (sample_count != 1u) {
    if (!device->create_resource(
            reshade::api::resource_desc(
                texture_width,
                texture_height,
                1,
                1,
                target.resolved_resource_desc.texture.format,
                1,
                reshade::api::memory_heap::gpu_only,
                reshade::api::resource_usage::resolve_dest | reshade::api::resource_usage::copy_source),
            nullptr,
            reshade::api::resource_usage::resolve_dest,
            &resolved_multisample)) {
      device->destroy_resource(intermediate);
      return ToolResult{
          .text = "Failed to create the single-sample texture to resolve the multisampled resource into.",
          .structured_content = json{{"error", "Failed to create the single-sample texture to resolve the multisampled resource into."}},
          .is_error = true,
      };
    }
    cmd_list->resolve_texture_region(target.resolved_resource, subresource, nullptr, resolved_multisample, 0, 0, 0, 0, resolve_format);
    cmd_list->barrier(resolved_multisample, reshade::api::resource_usage::resolve_dest, reshade::api::resource_usage::copy_source);
    copy_source = resolved_multisample;
    copy_source_subresource = 0u;
  }
  cmd_list->copy_texture_region(copy_source, copy_source_subresource, source_box, intermediate, 0, nullptr);
  queue->flush_immediate_command_list();
  queue->wait_idle();
  if (device->get_api() == reshade::api::device_api::opengl) {
    // ReShade 6.8's OpenGL backend caches the framebuffer of every texture it blits into (the multisample resolve, and
    // the readback copy of a default-framebuffer source), keyed by name, size and format, and drops that cache only in
    // destroy_resource_view (_fbo_lookup_version). The driver reuses the names of the textures destroyed below, so without
    // a flush the next analysis blits into an orphaned texture and reads back zeros. A view with the texture's own format
    // is usually the texture itself; either way destroying it deletes at most the view and flushes the cache.
    reshade::api::resource_view flush_view = {0u};
    if (device->create_resource_view(
            intermediate,
            reshade::api::resource_usage::copy_dest,
            reshade::api::resource_view_desc(intermediate_format),
            &flush_view)) {
      device->destroy_resource_view(flush_view);
    } else {
      reshade::log::message(
          reshade::log::level::warning,
          "devkit::analyze_resource(could not flush ReShade's OpenGL framebuffer cache; the next read may return zeros)");
    }
  }
  if (resolved_multisample.handle != 0u) {
    device->destroy_resource(resolved_multisample);
  }

  ToolResult tool_result = {
      .text = "Failed to map the readback resource.",
      .structured_content = json{{"error", "Failed to map the readback resource."}},
      .is_error = true,
  };
  reshade::api::subresource_data mapped_data = {};
  if (device->map_texture_region(intermediate, 0, nullptr, reshade::api::map_access::read_only, &mapped_data)) {
    double channel_min[4] = {
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
    };
    double channel_max[4] = {
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    double channel_sum[4] = {0.0, 0.0, 0.0, 0.0};
    double luminance_min = std::numeric_limits<double>::infinity();
    double luminance_max = -std::numeric_limits<double>::infinity();
    double luminance_sum = 0.0;
    // Display-light summary, filled only when a signal other than RAW is requested.
    std::vector<float> nits_luminance_values;
    if (signal != Signal::RAW) {
      nits_luminance_values.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
    }
    double nits_luminance_min = std::numeric_limits<double>::infinity();
    double nits_luminance_max = -std::numeric_limits<double>::infinity();
    double nits_luminance_sum = 0.0;
    double nits_max_channel_min = std::numeric_limits<double>::infinity();
    double nits_max_channel_max = -std::numeric_limits<double>::infinity();
    double nits_max_channel_sum = 0.0;
    std::array<uint64_t, 4> nits_pixels_above = {};
    constexpr std::array<double, 4> NITS_THRESHOLDS = {203.0, 400.0, 1000.0, 4000.0};
    // Colour statistics from display light in CIE 1976 u'v': saturation s_uv = 13 x the distance from the D65 white
    // point, which does not change with brightness, and the hue angle around it. Pixels below MIN_COLOR_NITS, whose
    // chromaticity is mostly noise, are left out; the hue histogram counts pixels at or above NEUTRAL_SATURATION.
    constexpr double MIN_COLOR_NITS = 1.0;
    constexpr double NEUTRAL_SATURATION = 0.05;
    constexpr size_t HUE_BIN_COUNT = 12u;
    std::vector<float> saturation_values;
    if (signal != Signal::RAW) {
      saturation_values.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
    }
    double saturation_min = std::numeric_limits<double>::infinity();
    double saturation_max = -std::numeric_limits<double>::infinity();
    double saturation_sum = 0.0;
    uint64_t chromatic_pixel_count = 0u;
    std::array<uint64_t, HUE_BIN_COUNT> hue_bin_pixels = {};
    uint32_t channel_count = 4u;
    uint64_t pixel_count = 0u;
    uint64_t finite_pixel_count = 0u;
    uint64_t nan_pixel_count = 0u;
    uint64_t inf_pixel_count = 0u;
    uint64_t pixels_all_rgb_zero = 0u;
    uint64_t pixels_any_negative = 0u;
    uint64_t pixels_any_at_or_above_one = 0u;
    uint64_t pixels_any_above_one = 0u;
    uint64_t pixels_any_above_two = 0u;
    uint64_t pixels_any_above_four = 0u;
    uint64_t pixels_any_exactly_one = 0u;
    bool supported_format = true;
    // One- and two-channel formats decode into R (and G) with B = 0, so display light, colour and the tone curve's
    // luminance make no sense there.
    bool signal_without_rgb = false;
    std::vector<std::uint8_t> preview_pixels;
    std::vector<std::uint8_t> raw_png_pixels;
    std::vector<float> exr_pixels;
    const auto output_format = output_path.has_value()
                                   ? GetOutputFormat(output_path.value())
                                   : OutputFormat::PNG;
    const auto resource_format_name = FormatFormat(context, intermediate_format);
    const auto live_raw_crc32 = ComputeMappedTextureCrc32(intermediate_format, mapped_data, width, height);
    const auto initial_upload = renodx::utils::resource::GetInitialUploadSignature(upload_resource);
    const auto latest_upload = renodx::utils::resource::GetLatestUploadSignature(upload_resource);
    std::string preferred_crc32_source = "live_readback";
    uint32_t preferred_crc32 = live_raw_crc32;
    if (initial_upload.has_value()
        && initial_upload->subresource == subresource
        && initial_upload->format == target.resolved_resource_desc.texture.format
        && initial_upload->width == width
        && initial_upload->height == height
        && initial_upload->full_update) {
      preferred_crc32 = initial_upload->crc32;
      preferred_crc32_source = "initial_upload";
    } else if (latest_upload.has_value()
               && latest_upload->subresource == subresource
               && latest_upload->format == target.resolved_resource_desc.texture.format
               && latest_upload->width == width
               && latest_upload->height == height
               && latest_upload->full_update) {
      preferred_crc32 = latest_upload->crc32;
      preferred_crc32_source = "latest_upload";
    }
    if (output_path.has_value() && output_format == OutputFormat::PNG) {
      preview_pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    }
    if (output_path.has_value() && output_format == OutputFormat::EXR) {
      if (analysis_format != reshade::api::format::r16g16b16a16_float) {
        exr_pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
      }
    }

    if (pixel_luminance != nullptr) {
      pixel_luminance->reserve(static_cast<std::size_t>(width) * height);
    }
    // Banding (opt-in): steps between neighbouring pixels in the values as stored (an sRGB format re-encoded), per
    // channel. UNORM formats are measured in codes; a float step is smooth when it is at most BANDING_SMOOTH_RELATIVE of
    // the value or at most BANDING_SMOOTH_CODES 8-bit codes (so 8-bit steps carried in a float are smooth in dark areas
    // too). A horizontal run of at least two equal values bounded by smooth non-zero steps on both sides is a band. Rows
    // are streamed: only the previous row is kept.
    constexpr std::int64_t BANDING_SMOOTH_CODES = 4;
    constexpr double BANDING_SMOOTH_RELATIVE = 0.02;
    constexpr std::size_t BANDING_LOG_BINS = 64u;
    constexpr double BANDING_RELATIVE_LOG_MIN = 1e-7;
    // |to - from| / max(|from|, |to|) is at most 2 (opposite signs).
    constexpr double BANDING_RELATIVE_LOG_MAX = 2.0;
    // HDR and scRGB values reach thousands, so absolute steps get a wider range.
    constexpr double BANDING_ABSOLUTE_LOG_MIN = 1e-7;
    constexpr double BANDING_ABSOLUTE_LOG_MAX = 1e4;
    constexpr std::uint32_t BANDING_LONG_RUN = 8u;
    enum class BandingStep : std::uint8_t {
      EQUAL,
      SMOOTH,
      LARGE,
    };
    const double unorm_max_code = [&]() {
      switch (analysis_format) {
        case reshade::api::format::r8g8b8a8_unorm:
        case reshade::api::format::r8g8b8a8_unorm_srgb:
        case reshade::api::format::b8g8r8a8_unorm:
        case reshade::api::format::b8g8r8a8_unorm_srgb:
        case reshade::api::format::r8g8b8x8_unorm:
        case reshade::api::format::r8g8b8x8_unorm_srgb:
        case reshade::api::format::b8g8r8x8_unorm:
        case reshade::api::format::b8g8r8x8_unorm_srgb:
        case reshade::api::format::r8_unorm:
        case reshade::api::format::r8g8_unorm:
          return 255.0;
        case reshade::api::format::r10g10b10a2_unorm:
        case reshade::api::format::b10g10r10a2_unorm:
          return 1023.0;
        case reshade::api::format::r16g16b16a16_unorm:
        case reshade::api::format::r16_unorm:
        case reshade::api::format::r16g16_unorm:
          return 65535.0;
        default:
          return 0.0;
      }
    }();
    const bool banding_srgb = (analysis_format == reshade::api::format_to_default_typed(analysis_format, 1)
                               && analysis_format != reshade::api::format_to_default_typed(analysis_format, 0));
    std::vector<float> banding_previous_row;
    std::vector<std::uint8_t> banding_previous_row_valid;
    if (measure_banding) {
      banding_previous_row.resize(static_cast<std::size_t>(width) * 3u);
      banding_previous_row_valid.resize(width, 0u);
    }
    std::array<float, 3> banding_left = {};
    bool banding_left_valid = false;
    std::array<std::uint32_t, 3> banding_run_length = {};
    std::array<bool, 3> banding_run_bounded = {};
    std::array<std::uint64_t, 3> banding_pairs = {};
    std::array<std::uint64_t, 3> banding_zero_pairs = {};
    std::array<std::uint64_t, 3> banding_smooth_steps = {};
    std::array<std::array<std::uint64_t, BANDING_SMOOTH_CODES + 1>, 3> banding_code_histogram = {};
    std::array<std::array<std::uint64_t, BANDING_LOG_BINS>, 3> banding_relative_histogram = {};
    std::array<std::array<std::uint64_t, BANDING_LOG_BINS>, 3> banding_absolute_histogram = {};
    std::array<std::uint64_t, 3> banding_band_count = {};
    std::array<std::uint64_t, 3> banding_band_pixels = {};
    std::array<std::uint64_t, 3> banding_long_bands = {};
    std::array<std::uint32_t, 3> banding_longest_band = {};
    // Log-spaced bins over [log_min, log_max] for float steps.
    const auto banding_log_bin = [&](double value, double log_min, double log_max) {
      const auto position = std::log10(std::max(value, log_min) / log_min) / std::log10(log_max / log_min);
      return std::min(static_cast<std::size_t>(std::max(position, 0.0) * BANDING_LOG_BINS), BANDING_LOG_BINS - 1u);
    };
    // Classifies one neighbour step and counts it.
    const auto banding_measure_step = [&](std::size_t channel, float from, float to) {
      ++banding_pairs[channel];
      if (unorm_max_code > 0.0) {
        const auto code_step = std::llabs(std::llround(to * unorm_max_code) - std::llround(from * unorm_max_code));
        if (code_step == 0) {
          ++banding_zero_pairs[channel];
          return BandingStep::EQUAL;
        }
        if (code_step > BANDING_SMOOTH_CODES) return BandingStep::LARGE;
        ++banding_code_histogram[channel][static_cast<std::size_t>(code_step)];
        ++banding_smooth_steps[channel];
        return BandingStep::SMOOTH;
      }
      const auto step = std::abs(static_cast<double>(to) - static_cast<double>(from));
      if (step == 0.0) {
        ++banding_zero_pairs[channel];
        return BandingStep::EQUAL;
      }
      const auto relative_step = step / std::max({std::abs(static_cast<double>(from)), std::abs(static_cast<double>(to)), 1e-6});
      if (relative_step > BANDING_SMOOTH_RELATIVE && step > (BANDING_SMOOTH_CODES / 255.0)) return BandingStep::LARGE;
      ++banding_relative_histogram[channel][banding_log_bin(relative_step, BANDING_RELATIVE_LOG_MIN, BANDING_RELATIVE_LOG_MAX)];
      ++banding_absolute_histogram[channel][banding_log_bin(step, BANDING_ABSOLUTE_LOG_MIN, BANDING_ABSOLUTE_LOG_MAX)];
      ++banding_smooth_steps[channel];
      return BandingStep::SMOOTH;
    };
    for (uint32_t y = 0; y < height && supported_format && !signal_without_rgb; ++y) {
      banding_left_valid = false;
      const auto* row = static_cast<const std::uint8_t*>(mapped_data.data) + (static_cast<size_t>(mapped_data.row_pitch) * y);
      const auto pixel_stride = static_cast<size_t>(reshade::api::format_row_pitch(intermediate_format, 1));
      for (uint32_t x = 0; x < width; ++x) {
        float rgba[4] = {0.f, 0.f, 0.f, 1.f};
        uint32_t decoded_channel_count = 4u;
        const auto* pixel = row + (pixel_stride * x);
        if (!DecodePixelToLinear(analysis_format, pixel, rgba, decoded_channel_count)) {
          supported_format = false;
          break;
        }
        if ((signal != Signal::RAW || pixel_luminance != nullptr) && decoded_channel_count < 3u) {
          signal_without_rgb = true;
          break;
        }
        if (!preview_pixels.empty()) {
          EncodePreviewPixel(
              rgba,
              preview_mode,
              preview_pixels.data() + ((static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u));
        }
        if (!exr_pixels.empty()) {
          auto* out_pixel = exr_pixels.data() + ((static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u);
          out_pixel[0] = rgba[0];
          out_pixel[1] = rgba[1];
          out_pixel[2] = rgba[2];
          out_pixel[3] = rgba[3];
        }
        if (measure_banding) {
          const auto banding_channels = std::min<uint32_t>(decoded_channel_count, 3u);
          const bool banding_finite = std::isfinite(rgba[0]) && std::isfinite(rgba[1]) && std::isfinite(rgba[2]);
          for (uint32_t channel = 0u; channel < banding_channels && banding_finite; ++channel) {
            const auto stored = (banding_srgb ? LinearToSrgb(rgba[channel]) : rgba[channel]);
            const auto row_index = (static_cast<std::size_t>(x) * 3u) + channel;
            if (banding_left_valid) {
              switch (banding_measure_step(channel, banding_left[channel], stored)) {
                case BandingStep::EQUAL:
                  ++banding_run_length[channel];
                  break;
                case BandingStep::SMOOTH:
                  // A single pixel between two steps is a ramp, not a band.
                  if (banding_run_bounded[channel] && banding_run_length[channel] >= 2u) {
                    ++banding_band_count[channel];
                    banding_band_pixels[channel] += banding_run_length[channel];
                    banding_longest_band[channel] = std::max(banding_longest_band[channel], banding_run_length[channel]);
                    if (banding_run_length[channel] >= BANDING_LONG_RUN) {
                      ++banding_long_bands[channel];
                    }
                  }
                  banding_run_length[channel] = 1u;
                  banding_run_bounded[channel] = true;
                  break;
                case BandingStep::LARGE:
                  banding_run_length[channel] = 1u;
                  banding_run_bounded[channel] = false;
                  break;
              }
            } else {
              banding_run_length[channel] = 1u;
              banding_run_bounded[channel] = false;
            }
            if (y > 0u && banding_previous_row_valid[x] != 0u) {
              (void)banding_measure_step(channel, banding_previous_row[row_index], stored);
            }
            banding_previous_row[row_index] = stored;
            banding_left[channel] = stored;
          }
          banding_previous_row_valid[x] = (banding_finite ? 1u : 0u);
          banding_left_valid = banding_finite;
        }
        ++pixel_count;
        channel_count = decoded_channel_count;

        bool has_nan = false;
        bool has_inf = false;
        for (uint32_t channel = 0; channel < 4u; ++channel) {
          has_nan |= std::isnan(rgba[channel]);
          has_inf |= std::isinf(rgba[channel]);
        }
        if (has_nan) ++nan_pixel_count;
        if (has_inf) ++inf_pixel_count;
        if (has_nan || has_inf) {
          // A non-finite alpha drops the pixel too, as for every statistic here.
          if (pixel_luminance != nullptr) {
            pixel_luminance->push_back(std::numeric_limits<float>::quiet_NaN());
          }
          continue;
        }

        ++finite_pixel_count;

        const auto any_negative = rgba[0] < 0.f || rgba[1] < 0.f || rgba[2] < 0.f;
        const auto any_at_or_above_one = rgba[0] >= 1.f || rgba[1] >= 1.f || rgba[2] >= 1.f;
        const auto any_above_one = rgba[0] > 1.f || rgba[1] > 1.f || rgba[2] > 1.f;
        const auto any_above_two = rgba[0] > 2.f || rgba[1] > 2.f || rgba[2] > 2.f;
        const auto any_above_four = rgba[0] > 4.f || rgba[1] > 4.f || rgba[2] > 4.f;
        const auto all_rgb_zero = rgba[0] == 0.f && rgba[1] == 0.f && rgba[2] == 0.f;

        pixels_any_negative += any_negative ? 1u : 0u;
        pixels_any_at_or_above_one += any_at_or_above_one ? 1u : 0u;
        pixels_any_above_one += any_above_one ? 1u : 0u;
        pixels_any_above_two += any_above_two ? 1u : 0u;
        pixels_any_above_four += any_above_four ? 1u : 0u;
        pixels_any_exactly_one += (rgba[0] == 1.f || rgba[1] == 1.f || rgba[2] == 1.f) ? 1u : 0u;
        pixels_all_rgb_zero += all_rgb_zero ? 1u : 0u;

        for (uint32_t channel = 0; channel < 4u; ++channel) {
          channel_min[channel] = std::min(channel_min[channel], static_cast<double>(rgba[channel]));
          channel_max[channel] = std::max(channel_max[channel], static_cast<double>(rgba[channel]));
          channel_sum[channel] += rgba[channel];
        }

        const auto luminance = (0.2126 * rgba[0]) + (0.7152 * rgba[1]) + (0.0722 * rgba[2]);
        luminance_min = std::min(luminance_min, luminance);
        luminance_max = std::max(luminance_max, luminance);
        luminance_sum += luminance;

        if (signal == Signal::RAW) {
          if (pixel_luminance != nullptr) {
            pixel_luminance->push_back(static_cast<float>(luminance));
          }
          continue;
        }

        const auto display_light = DecodeDisplayLight(signal, rgba);
        const auto& nits = display_light.nits;
        const auto nits_luminance = display_light.luminance;
        if (pixel_luminance != nullptr) {
          pixel_luminance->push_back(static_cast<float>(nits_luminance));
        }
        const auto nits_max_channel = std::max({nits[0], nits[1], nits[2]});
        nits_luminance_values.push_back(static_cast<float>(nits_luminance));
        nits_luminance_min = std::min(nits_luminance_min, nits_luminance);
        nits_luminance_max = std::max(nits_luminance_max, nits_luminance);
        nits_luminance_sum += nits_luminance;
        nits_max_channel_min = std::min(nits_max_channel_min, nits_max_channel);
        nits_max_channel_max = std::max(nits_max_channel_max, nits_max_channel);
        nits_max_channel_sum += nits_max_channel;
        for (size_t threshold_index = 0; threshold_index < NITS_THRESHOLDS.size(); ++threshold_index) {
          nits_pixels_above[threshold_index] += (nits_luminance > NITS_THRESHOLDS[threshold_index] ? 1u : 0u);
        }

        // The colour statistics stay last in the pixel loop: their early continues skip dark and neutral pixels.
        if (nits_luminance < MIN_COLOR_NITS) continue;
        // XYZ (D65) of the display light in the signal's primaries: BT.2020 for PQ, BT.709 for scRGB.
        const auto xyz = (signal == Signal::PQ
                              ? std::array<double, 3>{
                                    (0.6369580 * nits[0]) + (0.1446169 * nits[1]) + (0.1688810 * nits[2]),
                                    (0.2627002 * nits[0]) + (0.6779981 * nits[1]) + (0.0593017 * nits[2]),
                                    (0.0280727 * nits[1]) + (1.0609851 * nits[2]),
                                }
                              : std::array<double, 3>{
                                    (0.4123908 * nits[0]) + (0.3575843 * nits[1]) + (0.1804808 * nits[2]),
                                    (0.2126390 * nits[0]) + (0.7151687 * nits[1]) + (0.0721923 * nits[2]),
                                    (0.0193308 * nits[0]) + (0.1191948 * nits[1]) + (0.9505322 * nits[2]),
                                });
        const auto uv_denominator = xyz[0] + (15.0 * xyz[1]) + (3.0 * xyz[2]);
        if (uv_denominator <= 0.0) continue;
        // D65 white (x 0.3127, y 0.3290) in u'v', consistent with the matrices above.
        const auto u_offset = (4.0 * xyz[0] / uv_denominator) - 0.197830;
        const auto v_offset = (9.0 * xyz[1] / uv_denominator) - 0.468320;
        const auto saturation = 13.0 * std::sqrt((u_offset * u_offset) + (v_offset * v_offset));
        saturation_values.push_back(static_cast<float>(saturation));
        saturation_min = std::min(saturation_min, saturation);
        saturation_max = std::max(saturation_max, saturation);
        saturation_sum += saturation;
        if (saturation < NEUTRAL_SATURATION) continue;
        ++chromatic_pixel_count;
        auto hue_degrees = std::atan2(v_offset, u_offset) * (180.0 / std::numbers::pi);
        if (hue_degrees < 0.0) {
          hue_degrees += 360.0;
        }
        ++hue_bin_pixels[std::min(static_cast<size_t>(hue_degrees / (360.0 / HUE_BIN_COUNT)), HUE_BIN_COUNT - 1u)];
      }
    }

    if (!supported_format || signal_without_rgb) {
      device->unmap_texture_region(intermediate, 0);
      const auto error = (supported_format
                              ? std::format(
                                    "Format {} has fewer than three channels, so luminance-based results (signal nits and colour, the "
                                    "tone curve) do not apply; analyze it with signal 'raw' and read channels.r (and channels.g), or "
                                    "give the tone curve another inputSlot.",
                                    FormatFormat(context, analysis_format))
                              : std::format("Readback is not implemented for format {}.", FormatFormat(context, analysis_format)));
      tool_result = ToolResult{
          .text = error,
          .structured_content = json{{"error", error}},
          .is_error = true,
      };
    } else {
      json output_json = nullptr;
      json hash_dump_json = nullptr;
      std::optional<std::string> output_error = std::nullopt;
      if (output_path.has_value()) {
        std::error_code filesystem_error;
        const auto parent_path = output_path->parent_path();
        if (!parent_path.empty()) {
          std::filesystem::create_directories(parent_path, filesystem_error);
        }
        if (filesystem_error) {
          output_error = std::format(
              "Failed to create output directory '{}': {}",
              output_path->parent_path().string(),
              filesystem_error.message());
        } else {
          switch (output_format) {
            case OutputFormat::PNG:
              if (!WritePngPreview(output_path.value(), width, height, preview_pixels)) {
                output_error = std::format(
                    "Failed to write preview PNG to '{}'.",
                    output_path->string());
              } else {
                output_json = json{
                    {"path", output_path->string()},
                    {"format", "png"},
                    {"mode", preview_mode_name},
                    {"width", width},
                    {"height", height},
                };
              }
              break;
            case OutputFormat::EXR:
              try {
                if (analysis_format == reshade::api::format::r16g16b16a16_float) {
                  if ((mapped_data.row_pitch % sizeof(std::uint16_t)) != 0u) {
                    output_error = std::format(
                        "Mapped RGBA16F readback row pitch ({}) is not aligned to 16-bit values.",
                        mapped_data.row_pitch);
                    break;
                  }

                  const auto row_stride_values = static_cast<size_t>(mapped_data.row_pitch) / sizeof(std::uint16_t);
                  const auto total_values = row_stride_values * static_cast<size_t>(height);
                  const auto* half_pixels = static_cast<const std::uint16_t*>(mapped_data.data);
                  renodx::utils::exr::WriteRgba16f(
                      output_path.value(),
                      width,
                      height,
                      std::span<const std::uint16_t>(half_pixels, total_values),
                      row_stride_values);
                } else {
                  renodx::utils::exr::WriteRgba16f(
                      output_path.value(),
                      width,
                      height,
                      std::span<const float>(exr_pixels.data(), exr_pixels.size()));
                }
                output_json = json{
                    {"path", output_path->string()},
                    {"format", "exr"},
                    {"layout", "scanline"},
                    {"compression", "none"},
                    {"pixelType", "rgba16f"},
                    {"width", width},
                    {"height", height},
                };
              } catch (const std::exception& exception) {
                output_error = std::format(
                    "Failed to write RGBA16F EXR dump to '{}': {}",
                    output_path->string(),
                    exception.what());
              }
              break;
          }
        }
      }

      if (!output_error.has_value() && output_directory.has_value()) {
        std::error_code directory_error = {};
        std::filesystem::create_directories(output_directory.value(), directory_error);
        if (directory_error) {
          output_error = std::format(
              "Failed to create output directory '{}': {}",
              output_directory->string(),
              directory_error.message());
        } else if (!TryBuildRawPngPixels(intermediate_format, mapped_data, width, height, raw_png_pixels)) {
          output_error = std::format(
              "Hash-named dumping supports only RGBA8/BGRA8-style resource formats, the formats texture replacement loads back (8-bit PNG). The current resource format is {}; for an HDR copy use devkit_analyze_resource with an outputPath ending in .exr.",
              resource_format_name);
        } else {
          std::string chosen_prefix = name_prefix.value_or("");
          if (chosen_prefix.empty() && resource_view_summary_value.has_value()) {
            if (resource_view_summary_value->resource_reflection.has_value()
                && !resource_view_summary_value->resource_reflection->empty()) {
              chosen_prefix = resource_view_summary_value->resource_reflection.value();
            } else if (resource_view_summary_value->resource_view_reflection.has_value()
                       && !resource_view_summary_value->resource_view_reflection->empty()) {
              chosen_prefix = resource_view_summary_value->resource_view_reflection.value();
            }
          }
          if (chosen_prefix.empty()) {
            chosen_prefix = "resource";
          }

          const auto file_name = MakeSuggestedFileName(
              chosen_prefix,
              resource_format_name,
              width,
              height,
              preferred_crc32,
              "png");
          const auto hash_dump_path = (output_directory.value() / file_name).lexically_normal();
          if (!WritePngPreview(hash_dump_path, width, height, raw_png_pixels)) {
            output_error = std::format(
                "Failed to write hash-named PNG dump to '{}'.",
                hash_dump_path.string());
          } else {
            hash_dump_json = json{
                {"path", hash_dump_path.string()},
                {"fileName", file_name},
                {"format", "png"},
                {"width", width},
                {"height", height},
                {"crc32", FormatCrc32(preferred_crc32)},
                {"crc32Value", preferred_crc32},
                {"crc32Source", preferred_crc32_source},
                {"liveCrc32", FormatCrc32(live_raw_crc32)},
                {"liveCrc32Value", live_raw_crc32},
            };
          }
        }
      }

      device->unmap_texture_region(intermediate, 0);

      if (output_error.has_value()) {
        tool_result = ToolResult{
            .text = output_error.value(),
            .structured_content = json{{"error", output_error.value()}},
            .is_error = true,
        };
        device->destroy_resource(intermediate);
        return tool_result;
      }

      std::string suggested_prefix = name_prefix.value_or("");
      if (suggested_prefix.empty() && resource_view_summary_value.has_value()) {
        if (resource_view_summary_value->resource_reflection.has_value()
            && !resource_view_summary_value->resource_reflection->empty()) {
          suggested_prefix = resource_view_summary_value->resource_reflection.value();
        } else if (resource_view_summary_value->resource_view_reflection.has_value()
                   && !resource_view_summary_value->resource_view_reflection->empty()) {
          suggested_prefix = resource_view_summary_value->resource_view_reflection.value();
        }
      }
      if (suggested_prefix.empty()) {
        suggested_prefix = "resource";
      }

      auto resource_view_json = resource_view_summary_value.has_value()
                                    ? json(resource_view_summary_value.value())
                                    : json(nullptr);
      json analysis_target_json = {
          {"preferClone", prefer_clone},
          {"usedClone", target.used_clone},
          {"requestedResourceViewHandle", FormatHandle(context, resource_view.handle)},
          {"resolvedResourceViewHandle", FormatHandle(context, target.resolved_view.handle)},
          {"resolvedResourceHandle", FormatHandle(context, target.resolved_resource.handle)},
          {"resolvedViewType", std::format("{}", static_cast<std::uint32_t>(target.resolved_view_desc.type))},
          {"resolvedViewFormat", FormatFormat(context, analysis_format)},
          {"resolvedResourceType", std::format("{}", static_cast<std::uint32_t>(target.resolved_resource_desc.type))},
          {"resolvedResourceFormat", target.resolved_resource_desc.type == reshade::api::resource_type::buffer
                                         ? json(nullptr)
                                         : json(FormatFormat(context, target.resolved_resource_desc.texture.format))},
          {"resolvedResourceUsage", std::format("0x{:X}", static_cast<std::uint32_t>(target.resolved_resource_desc.usage))},
          {"nativeResourceUsage", std::format("0x{:X}", static_cast<std::uint32_t>(device->get_resource_desc(target.resolved_resource).usage))},
          {"depthStencil", IsDepthStencilResource(device, target.resolved_resource, target.resolved_resource_desc)},
      };
      json result = {
          {"deviceIndex", device_index},
          {"resourceViewHandle", FormatHandle(context, resource_view.handle)},
          {"resourceView", resource_view_json},
          {"analysisTarget", analysis_target_json},
          {
              "resourceIdentity",
              {
                  {"liveCrc32", FormatCrc32(live_raw_crc32)},
                  {"liveCrc32Value", live_raw_crc32},
                  {"preferredCrc32", FormatCrc32(preferred_crc32)},
                  {"preferredCrc32Value", preferred_crc32},
                  {"preferredCrc32Source", preferred_crc32_source},
                  {"initialUpload", initial_upload.has_value() ? UploadSignatureToJson(initial_upload.value()) : json(nullptr)},
                  {"latestUpload", latest_upload.has_value() ? UploadSignatureToJson(latest_upload.value()) : json(nullptr)},
                  {"suggestedFileName", MakeSuggestedFileName(suggested_prefix, resource_format_name, width, height, preferred_crc32, "png")},
              },
          },
          {
              "analysis",
              {
                  {"snapshotExact", false},
                  {"note", "This reads the current contents of the live resource view, not a frozen historical copy from the snapshot cache."},
                  {"subresourceIndex", subresource},
                  {"width", width},
                  {"height", height},
                  {"analysisFormat", FormatFormat(context, analysis_format)},
                  {"intermediateFormat", FormatFormat(context, intermediate_format)},
                  {"channelCount", channel_count},
                  {"pixelCount", pixel_count},
                  {"finitePixels", RatioStats(finite_pixel_count, pixel_count)},
                  {"nanPixels", RatioStats(nan_pixel_count, pixel_count)},
                  {"infPixels", RatioStats(inf_pixel_count, pixel_count)},
                  {"pixelsAllRgbZero", RatioStats(pixels_all_rgb_zero, pixel_count)},
                  {"pixelsAnyNegative", RatioStats(pixels_any_negative, pixel_count)},
                  {"pixelsAnyChannelAtOrAbove1", RatioStats(pixels_any_at_or_above_one, pixel_count)},
                  {"pixelsAnyChannelAbove1", RatioStats(pixels_any_above_one, pixel_count)},
                  {"pixelsAnyChannelAbove2", RatioStats(pixels_any_above_two, pixel_count)},
                  {"pixelsAnyChannelAbove4", RatioStats(pixels_any_above_four, pixel_count)},
                  {"pixelsAnyChannelExactly1", RatioStats(pixels_any_exactly_one, pixel_count)},
                  {
                      "channels",
                      {
                          {"r", ScalarStats(channel_min[0], channel_max[0], channel_sum[0], finite_pixel_count)},
                          {"g", ScalarStats(channel_min[1], channel_max[1], channel_sum[1], finite_pixel_count)},
                          {"b", ScalarStats(channel_min[2], channel_max[2], channel_sum[2], finite_pixel_count)},
                          {"a", ScalarStats(channel_min[3], channel_max[3], channel_sum[3], finite_pixel_count)},
                      },
                  },
                  {"luminance", ScalarStats(luminance_min, luminance_max, luminance_sum, finite_pixel_count)},
              },
          },
      };
      if (region.has_value()) {
        result["analysis"]["region"] = {
            {"x", region->left},
            {"y", region->top},
            {"width", region->width()},
            {"height", region->height()},
            {"textureWidth", texture_width},
            {"textureHeight", texture_height},
            {"note", "Statistics, preview, dump and resourceIdentity describe this region only."},
        };
      }
      // Rule-labelled facts about the signal, each with the rule that produced it. They are candidates for the
      // classification the skills require proving (snapshot, shader, watch); no guess of linear against gamma encoding is
      // made, because the values alone cannot tell them apart.
      {
        // At least this share of pixels with a channel exactly at 1.0 counts as a clamp.
        constexpr double CLIP_SHARE = 0.01;
        const auto share_of = [&](uint64_t count) {
          return (pixel_count == 0u ? 0.0 : static_cast<double>(count) / static_cast<double>(pixel_count));
        };
        json signal_hints = json::array();
        // One- and two-channel data is often not colour (motion vectors, normals, masks): the colour rules are skipped.
        const bool colour_data = (channel_count >= 3u);
        if (unorm_max_code > 0.0) {
          signal_hints.push_back(json{
              {"type", "unormStorage"},
              {"rule", "A UNORM format stores values from 0 to 1; whether that clipped light depends on the encoding (PQ in 10 bits covers 0 to 10000 nits)."},
          });
        } else if (colour_data && pixels_any_above_one != 0u) {
          signal_hints.push_back(json{
              {"type", "aboveOne"},
              {"rule", "A float format with values above 1.0: not clamped here (an untonemapped scene, scRGB, or a mod's upgrade)."},
              {"share", share_of(pixels_any_above_one)},
          });
        }
        // In a UNORM format 1.0 is only the top code, which white content reaches too; in a float format it is consistent
        // with a saturate or clamp.
        if (unorm_max_code == 0.0 && colour_data && share_of(pixels_any_exactly_one) >= CLIP_SHARE) {
          signal_hints.push_back(json{
              {"type", "atOne"},
              {"rule", "A float format with at least 1% of pixels having a channel exactly at 1.0: consistent with a saturate or clamp at or before this write (or white content)."},
              {"share", share_of(pixels_any_exactly_one)},
          });
        }
        if (colour_data && pixels_any_negative != 0u) {
          signal_hints.push_back(json{
              {"type", "negative"},
              {"rule", "Negative values: wide gamut encoded in BT.709 primaries (scRGB), or a math error."},
              {"share", share_of(pixels_any_negative)},
          });
        }
        if (nan_pixel_count + inf_pixel_count != 0u) {
          signal_hints.push_back(json{
              {"type", "nonFinite"},
              {"rule", "NaN or infinite values."},
              {"share", share_of(nan_pixel_count + inf_pixel_count)},
          });
        }
        result["analysis"]["signalHints"] = std::move(signal_hints);
      }
      if (measure_banding) {
        // Nearest bin holding the middle step of a histogram; nullopt when it is empty.
        const auto median_bin = []<std::size_t BIN_COUNT>(const std::array<std::uint64_t, BIN_COUNT>& histogram) -> std::optional<std::size_t> {
          std::uint64_t total = 0u;
          for (const auto count : histogram) {
            total += count;
          }
          if (total == 0u) return std::nullopt;
          std::uint64_t seen = 0u;
          for (std::size_t bin = 0u; bin < BIN_COUNT; ++bin) {
            seen += histogram[bin];
            if (seen * 2u >= total) return bin;
          }
          return BIN_COUNT - 1u;
        };
        const auto log_bin_center = [&](std::size_t bin, double log_min, double log_max) {
          return log_min * std::pow(log_max / log_min, (static_cast<double>(bin) + 0.5) / BANDING_LOG_BINS);
        };
        json banding_channels_json = json::array();
        std::uint64_t smooth_steps_total = 0u;
        for (std::size_t channel = 0u; channel < std::min<std::size_t>(channel_count, 3u); ++channel) {
          const auto pairs = static_cast<double>(std::max<std::uint64_t>(banding_pairs[channel], 1u));
          json channel_json = {
              {"channel", std::string(1, "rgb"[channel])},
              {"pairs", banding_pairs[channel]},
              {"smoothShare", static_cast<double>(banding_zero_pairs[channel] + banding_smooth_steps[channel]) / pairs},
              {"equalShare", static_cast<double>(banding_zero_pairs[channel]) / pairs},
              {"smoothSteps", banding_smooth_steps[channel]},
              {"bands", banding_band_count[channel]},
              {"meanBandLength",
               (banding_band_count[channel] == 0u
                    ? 0.0
                    : static_cast<double>(banding_band_pixels[channel]) / static_cast<double>(banding_band_count[channel]))},
              {"longestBand", banding_longest_band[channel]},
              {"bandsAtLeast8px", banding_long_bands[channel]},
          };
          if (unorm_max_code > 0.0) {
            const auto median = median_bin(banding_code_histogram[channel]);
            channel_json["medianStepCodes"] = (median.has_value() ? json(median.value()) : json(nullptr));
          } else {
            const auto median_relative = median_bin(banding_relative_histogram[channel]);
            const auto median_absolute = median_bin(banding_absolute_histogram[channel]);
            channel_json["medianStepRelative"] =
                (median_relative.has_value()
                     ? json(log_bin_center(median_relative.value(), BANDING_RELATIVE_LOG_MIN, BANDING_RELATIVE_LOG_MAX))
                     : json(nullptr));
            if (median_absolute.has_value()) {
              const auto median_step = log_bin_center(median_absolute.value(), BANDING_ABSOLUTE_LOG_MIN, BANDING_ABSOLUTE_LOG_MAX);
              channel_json["medianStepAbsolute"] = median_step;
              // In 8-bit codes of the same values: comparable with medianStepCodes of an RGBA8 read only when both hold
              // the same encoding (a linear float clone of an sRGB-encoded RGBA8 target is not).
              channel_json["medianStepIn8BitCodes"] = median_step * 255.0;
            } else {
              channel_json["medianStepAbsolute"] = nullptr;
              channel_json["medianStepIn8BitCodes"] = nullptr;
            }
          }
          smooth_steps_total += banding_smooth_steps[channel];
          banding_channels_json.push_back(std::move(channel_json));
        }
        json banding_json = {
            {"domain",
             (unorm_max_code > 0.0
                  ? std::format("codes of 1/{}{}", unorm_max_code, (banding_srgb ? ", sRGB-encoded values" : ""))
                  : std::string("float: step relative to the value, and absolute (log-binned)"))},
            {"smoothStepMaxCodes", BANDING_SMOOTH_CODES},
            {"smoothStepMaxRelative", BANDING_SMOOTH_RELATIVE},
            {"smoothStepMaxAbsolute", (unorm_max_code > 0.0 ? json(nullptr) : json(BANDING_SMOOTH_CODES / 255.0))},
            {"channels", std::move(banding_channels_json)},
        };
        // Below this many smooth non-zero steps the region holds too little gradient to judge.
        constexpr std::uint64_t BANDING_MIN_SMOOTH_STEPS = 100u;
        if (smooth_steps_total < BANDING_MIN_SMOOTH_STEPS) {
          banding_json["note"] = "Few smooth non-zero steps: the region has too little gradient to judge banding (pick a smooth gradient such as sky).";
        }
        result["analysis"]["banding"] = std::move(banding_json);
      }
      if (signal != Signal::RAW) {
        // Reorders the values; percentiles take the nearest rank.
        const auto compute_percentiles = [](std::vector<float>* values) {
          constexpr std::array<std::pair<const char*, double>, 4> PERCENTILES = {{
              {"p50", 0.50},
              {"p90", 0.90},
              {"p99", 0.99},
              {"p99_9", 0.999},
          }};
          json percentiles = json::object();
          if (values->empty()) return percentiles;
          for (const auto& [name, fraction] : PERCENTILES) {
            auto nth = values->begin() + static_cast<std::ptrdiff_t>(fraction * static_cast<double>(values->size() - 1u));
            std::nth_element(values->begin(), nth, values->end());
            percentiles[name] = *nth;
          }
          return percentiles;
        };
        const auto nits_count = static_cast<std::uint64_t>(nits_luminance_values.size());
        result["analysis"]["nits"] = {
            {"signal", std::string(GetSignalName(signal))},
            {"primaries", (signal == Signal::PQ ? "bt2020" : "bt709")},
            {"luminance", ScalarStats(nits_luminance_min, nits_luminance_max, nits_luminance_sum, nits_count)},
            {"luminancePercentiles", compute_percentiles(&nits_luminance_values)},
            {"maxChannel", ScalarStats(nits_max_channel_min, nits_max_channel_max, nits_max_channel_sum, nits_count)},
            {"pixelsLuminanceAbove203", RatioStats(nits_pixels_above[0], nits_count)},
            {"pixelsLuminanceAbove400", RatioStats(nits_pixels_above[1], nits_count)},
            {"pixelsLuminanceAbove1000", RatioStats(nits_pixels_above[2], nits_count)},
            {"pixelsLuminanceAbove4000", RatioStats(nits_pixels_above[3], nits_count)},
        };

        const auto color_count = static_cast<std::uint64_t>(saturation_values.size());
        json hue_histogram = json::array();
        for (size_t bin = 0; bin < HUE_BIN_COUNT; ++bin) {
          hue_histogram.push_back({
              {"fromDegrees", bin * (360u / HUE_BIN_COUNT)},
              {"toDegrees", (bin + 1u) * (360u / HUE_BIN_COUNT)},
              {"pixels", RatioStats(hue_bin_pixels[bin], chromatic_pixel_count)},
          });
        }
        result["analysis"]["color"] = {
            {"space", "cie1976_uv"},
            {"minNits", MIN_COLOR_NITS},
            {"pixelsCounted", RatioStats(color_count, nits_count)},
            {"saturation", ScalarStats(saturation_min, saturation_max, saturation_sum, color_count)},
            {"saturationPercentiles", compute_percentiles(&saturation_values)},
            {"neutralSaturationBelow", NEUTRAL_SATURATION},
            {"pixelsChromatic", RatioStats(chromatic_pixel_count, color_count)},
            {"hueHistogram", hue_histogram},
        };
      }
      if (!output_json.is_null()) {
        if (output_format == OutputFormat::PNG) {
          result["preview"] = output_json;
        } else {
          result["dump"] = output_json;
        }
      }
      if (!hash_dump_json.is_null()) {
        result["hashDump"] = hash_dump_json;
      }

      std::string text;
      const auto analyzed_view_handle = FormatHandle(context, target.resolved_view.handle);
      const auto* const analyzed_view_label = target.used_clone ? "clone resource view" : "resource view";
      if (output_path.has_value()) {
        if (output_format == OutputFormat::PNG) {
          text = std::format(
              "Analyzed {}x{} {} {} {} on device #{}. Saved a {} preview to '{}'. {} pixel(s) are above 1.0 in at least one RGB channel.",
              width,
              height,
              FormatFormat(context, analysis_format),
              analyzed_view_label,
              analyzed_view_handle,
              device_index,
              preview_mode_name,
              output_path->string(),
              pixels_any_above_one);
        } else {
          text = std::format(
              "Analyzed {}x{} {} {} {} on device #{}. Saved an RGBA16F EXR dump to '{}'. {} pixel(s) are above 1.0 in at least one RGB channel.",
              width,
              height,
              FormatFormat(context, analysis_format),
              analyzed_view_label,
              analyzed_view_handle,
              device_index,
              output_path->string(),
              pixels_any_above_one);
        }
        if (!hash_dump_json.is_null()) {
          text += std::format(" Saved a hash-named PNG dump to '{}'.", hash_dump_json["path"].get<std::string>());
        }
      } else {
        if (!hash_dump_json.is_null()) {
          text = std::format(
              "Analyzed {}x{} {} {} {} on device #{}. Saved a hash-named PNG dump to '{}'. {} pixel(s) are above 1.0 in at least one RGB channel.",
              width,
              height,
              FormatFormat(context, analysis_format),
              analyzed_view_label,
              analyzed_view_handle,
              device_index,
              hash_dump_json["path"].get<std::string>(),
              pixels_any_above_one);
        } else {
          text = std::format(
              "Analyzed {}x{} {} {} {} on device #{}. {} pixel(s) are above 1.0 in at least one RGB channel.",
              width,
              height,
              FormatFormat(context, analysis_format),
              analyzed_view_label,
              analyzed_view_handle,
              device_index,
              pixels_any_above_one);
        }
      }
      if (signal != Signal::RAW && !nits_luminance_values.empty()) {
        text += std::format(
            " Luminance ({}) max {:.1f} nits, mean {:.2f} nits.",
            GetSignalName(signal),
            result["analysis"]["nits"]["luminance"]["max"].get<double>(),
            result["analysis"]["nits"]["luminance"]["mean"].get<double>());
      }
      if (sample_count != 1u) {
        result["resolvedFromSampleCount"] = sample_count;
      }
      tool_result = ToolResult{
          .text = text,
          .structured_content = result,
      };
    }
  }

  device->destroy_resource(intermediate);
  return tool_result;
}

}  // namespace internal

inline constexpr std::uint32_t MAX_ANALYSIS_FRAMES = 32u;
// 65 analyses of a 4K frame with a signal take about a minute on the game's thread; three times that is room enough.
inline constexpr std::uint32_t MAX_MULTI_FRAME_WAIT_SECONDS = 180u;

// Headline numbers kept per frame of a multi-frame analysis, by JSON pointer into that frame's analysis. Missing or null
// ones are left out (nits and colour need a signal).
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 17> FRAME_METRICS = {{
    {"luminanceMean", "/luminance/mean"},
    {"luminanceMax", "/luminance/max"},
    {"nanRatio", "/nanPixels/ratio"},
    {"negativeRatio", "/pixelsAnyNegative/ratio"},
    // One- and two-channel formats (an exposure value, for example) are read from channels r and g.
    {"rMean", "/channels/r/mean"},
    {"gMean", "/channels/g/mean"},
    {"nitsMean", "/nits/luminance/mean"},
    {"nitsMax", "/nits/luminance/max"},
    {"nitsP50", "/nits/luminancePercentiles/p50"},
    {"nitsP90", "/nits/luminancePercentiles/p90"},
    {"nitsP99", "/nits/luminancePercentiles/p99"},
    {"nitsP99_9", "/nits/luminancePercentiles/p99_9"},
    {"shareAbove203", "/nits/pixelsLuminanceAbove203/ratio"},
    {"shareAbove400", "/nits/pixelsLuminanceAbove400/ratio"},
    {"shareAbove1000", "/nits/pixelsLuminanceAbove1000/ratio"},
    {"saturationMean", "/color/saturation/mean"},
    {"saturationP90", "/color/saturationPercentiles/p90"},
}};

// Per metric of FRAME_METRICS: sample count, mean, population standard deviation, min and max over the given frames.
[[nodiscard]] inline json SummarizeFrameSamples(const std::vector<json>& frame_samples) {
  json summary = json::object();
  for (const auto& [metric_name, metric_path] : FRAME_METRICS) {
    std::vector<double> values;
    for (const auto& frame_sample : frame_samples) {
      if (const auto found = frame_sample.find(metric_name); found != frame_sample.end() && found->is_number()) {
        values.push_back(found->get<double>());
      }
    }
    if (values.empty()) continue;
    double sum = 0.0;
    for (const auto value : values) {
      sum += value;
    }
    const auto mean = sum / static_cast<double>(values.size());
    double squared_deviation_sum = 0.0;
    for (const auto value : values) {
      squared_deviation_sum += (value - mean) * (value - mean);
    }
    summary[metric_name] = {
        {"n", values.size()},
        {"mean", mean},
        {"stddev", std::sqrt(squared_deviation_sum / static_cast<double>(values.size()))},
        {"min", std::ranges::min(values)},
        {"max", std::ranges::max(values)},
    };
  }
  return summary;
}

[[nodiscard]] inline ToolResult HandleAnalyzeResourceTool(const json& arguments, const ToolContext& context) {
  if (!context.resolve_device_index || !context.enqueue_request) {
    throw std::runtime_error("AnalyzeResourceToolContext is not initialized.");
  }

  const auto resource_view_handle = resource_handle::GetRequired(arguments, "resourceViewHandle");
  const auto device_index = context.resolve_device_index(arguments);
  const auto output_path_value = mcp_arguments::GetOptional<std::string>(arguments, "outputPath").value_or("");
  // A .json outputPath writes the finished result (frames and A/B summaries included) instead of an image, so two runs can
  // be compared outside the game; it is the one output allowed with frames and abShaderHash.
  const bool json_output = (std::filesystem::path(output_path_value).extension() == L".json");
  const auto preview_mode_value = mcp_arguments::GetOptional<std::string>(arguments, "previewMode").value_or("clamped");
  const auto preview_mode = internal::ParsePreviewMode(preview_mode_value);
  const auto prefer_clone = mcp_arguments::GetOptional<bool>(arguments, "preferClone").value_or(false);
  const auto signal = internal::ParseSignal(mcp_arguments::GetOptional<std::string>(arguments, "signal").value_or("raw"));
  const auto region = internal::ParseRegion(arguments);
  const auto measure_point = mcp_arguments::GetOptional<std::string>(arguments, "measurePoint").value_or("present");
  if (measure_point != "present" && measure_point != "frame_end") {
    throw std::runtime_error("measurePoint must be 'present' or 'frame_end'.");
  }
  const auto frames = mcp_arguments::ValidateRange<std::uint32_t>(
      "frames",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "frames").value_or(1u),
      1u,
      MAX_ANALYSIS_FRAMES);
  if (frames > 1u && measure_point != "present") {
    throw std::runtime_error("frames above 1 needs measurePoint 'present'.");
  }
  if (frames > 1u && !output_path_value.empty() && !json_output) {
    throw std::runtime_error("frames above 1 can only write a .json outputPath.");
  }
  std::optional<std::uint32_t> ab_shader_hash = std::nullopt;
  if (arguments.contains("abShaderHash") && !arguments["abShaderHash"].is_null()) {
    ab_shader_hash = shader_hash::GetRequired(arguments, "abShaderHash");
  }
  const std::array<std::string, 2> ab_sources = {
      mcp_arguments::GetOptional<std::string>(arguments, "abSourceA").value_or("original"),
      mcp_arguments::GetOptional<std::string>(arguments, "abSourceB").value_or("addon"),
  };
  // The source names themselves are checked when the game's thread switches the first time.
  if (!ab_shader_hash.has_value() && (arguments.contains("abSourceA") || arguments.contains("abSourceB"))) {
    throw std::runtime_error("abSourceA and abSourceB need abShaderHash.");
  }
  if (ab_shader_hash.has_value() && (measure_point != "present" || (!output_path_value.empty() && !json_output))) {
    throw std::runtime_error("abShaderHash needs measurePoint 'present' and no image outputPath (a .json one is allowed).");
  }

  std::optional<std::filesystem::path> json_output_path;
  std::optional<std::filesystem::path> output_path = std::nullopt;
  if (json_output) {
    // The argument is UTF-8; a narrow std::filesystem::path would decode it with the ANSI code page.
    const auto requested_path = std::filesystem::path(std::u8string(output_path_value.begin(), output_path_value.end()));
    if (!requested_path.is_absolute()) {
      throw std::runtime_error("outputPath must be an absolute path.");
    }
    json_output_path = requested_path.lexically_normal();
  } else if (!output_path_value.empty()) {
    auto requested_path = std::filesystem::path(output_path_value);
    if (!requested_path.is_absolute()) {
      throw std::runtime_error("outputPath must be an absolute path.");
    }
    if (!requested_path.has_extension()) {
      requested_path.replace_extension(L".png");
    } else if (requested_path.extension() != L".png" && requested_path.extension() != L".exr") {
      throw std::runtime_error("outputPath must end in '.png' or '.exr'.");
    }
    output_path = requested_path.lexically_normal();
  }

  auto request = std::make_shared<PendingRequest>();
  request->resource_view = reshade::api::resource_view{resource_view_handle};
  request->output_path = std::move(output_path);
  request->output_directory = std::nullopt;
  request->name_prefix = std::nullopt;
  request->preview_mode = preview_mode;
  request->preview_mode_name = std::string(internal::GetPreviewModeName(preview_mode));
  request->prefer_clone = prefer_clone;
  request->region = region;
  request->signal = signal;
  request->at_frame_end = (measure_point == "frame_end");
  // A/B takes frames per side.
  request->frames = (ab_shader_hash.has_value() ? frames * 2u : frames);
  request->ab_shader_hash = ab_shader_hash;
  request->ab_sources = ab_sources;
  request->banding = mcp_arguments::GetOptional<bool>(arguments, "banding").value_or(false);
  if (request->banding && request->frames > 1u) {
    throw std::runtime_error("banding is measured on one frame: leave frames at 1 and abShaderHash out.");
  }

  if (!context.enqueue_request(device_index, request)) {
    const auto error = std::format("Failed to queue the resource analysis request for device #{}.", device_index);
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }

  {
    std::unique_lock request_lock(request->mutex);
    if (!request->cv.wait_for(request_lock, std::chrono::seconds(15), [&request] { return request->completed || request->started; })) {
      request->canceled = true;
      request_lock.unlock();
      if (context.cancel_request) {
        context.cancel_request(device_index, request);
      }
      const auto error = (request->at_frame_end
                              ? std::string("Timed out waiting for the next frame end (reshade_present) to process the resource analysis request: measurePoint 'frame_end' needs a swap chain with a ReShade effect runtime.")
                              : std::string("Timed out waiting for the next present to process the resource analysis request."));
      return ToolResult{
          .text = error,
          .structured_content = json{{"error", error}},
          .is_error = true,
      };
    }

    // A multi-frame request spans many presents, which stop when the game is minimized or loading: give up then, and let
    // the game's thread put an A/B shader back at its next present.
    if (!request->cv.wait_for(request_lock, std::chrono::seconds(MAX_MULTI_FRAME_WAIT_SECONDS), [&request] { return request->completed; })) {
      request->canceled = true;
      const auto error = std::format(
          "The multi-frame analysis did not finish within {} s (the game stopped presenting frames?). It is canceled; an A/B shader gets its previous source back at the next present.",
          MAX_MULTI_FRAME_WAIT_SECONDS);
      return ToolResult{
          .text = error,
          .structured_content = json{{"error", error}},
          .is_error = true,
      };
    }
  }

  if (!request->result.has_value()) {
    return ToolResult{
        .text = "The resource analysis request completed without a result.",
        .structured_content = json{{"error", "The resource analysis request completed without a result."}},
        .is_error = true,
    };
  }

  const auto finish = [&json_output_path, &output_path_value](ToolResult finished) {
    if (!json_output_path.has_value() || finished.is_error) return finished;
    std::ofstream json_file(json_output_path.value(), std::ios::trunc);
    json_file << finished.structured_content.dump(2);
    json_file.close();
    if (json_file) {
      finished.structured_content["jsonPath"] = output_path_value;
      finished.text += std::format(" Wrote the result to '{}'.", output_path_value);
    } else {
      finished.structured_content["jsonError"] = std::format("Failed to write '{}'.", output_path_value);
      finished.text += std::format(" Failed to write '{}'.", output_path_value);
    }
    return finished;
  };

  auto result = std::move(request->result.value());
  if (request->frames == 1u || result.is_error) return finish(std::move(result));

  std::vector<json> frame_samples;
  std::array<std::vector<json>, 2> side_samples;
  frame_samples.reserve(request->samples.size());
  for (const auto& sample : request->samples) {
    json frame_sample = {{"frame", sample["frame"]}};
    if (sample.contains("side")) {
      frame_sample["side"] = sample["side"];
    }
    for (const auto& [metric_name, metric_path] : FRAME_METRICS) {
      const auto pointer = json::json_pointer(std::string(metric_path));
      if (sample["analysis"].contains(pointer) && sample["analysis"][pointer].is_number()) {
        frame_sample[metric_name] = sample["analysis"][pointer];
      }
    }
    if (sample.contains("side")) {
      side_samples[(sample["side"] == "a" ? 0u : 1u)].push_back(frame_sample);
    }
    frame_samples.push_back(std::move(frame_sample));
  }
  result.structured_content["frames"] = {
      {"count", frame_samples.size()},
      {"note", "analysis is the last frame; frame numbers count the presents this request saw (one sample per present)."},
  };
  if (!ab_shader_hash.has_value()) {
    result.structured_content["frames"]["summary"] = SummarizeFrameSamples(frame_samples);
    result.structured_content["frames"]["samples"] = std::move(frame_samples);
    result.text += std::format(" Measured {} frames, one per present; per-frame headline numbers and their summary are in frames.", frames);
    return finish(std::move(result));
  }
  result.structured_content["frames"]["samples"] = std::move(frame_samples);

  const auto summary_a = SummarizeFrameSamples(side_samples[0]);
  const auto summary_b = SummarizeFrameSamples(side_samples[1]);
  json difference = json::object();
  for (const auto& [metric_name, metric_stats_a] : summary_a.items()) {
    if (!summary_b.contains(metric_name)) continue;
    const auto& metric_stats_b = summary_b[metric_name];
    const auto overlap_low = std::max(metric_stats_a["min"].get<double>(), metric_stats_b["min"].get<double>());
    const auto overlap_high = std::min(metric_stats_a["max"].get<double>(), metric_stats_b["max"].get<double>());
    difference[metric_name] = {
        {"meanBMinusA", metric_stats_b["mean"].get<double>() - metric_stats_a["mean"].get<double>()},
        {"rangesOverlap", overlap_low <= overlap_high},
    };
  }
  result.structured_content["ab"] = {
      {"shaderHash", std::format("0x{:08X}", ab_shader_hash.value())},
      {"a", {{"source", ab_sources[0]}, {"summary", summary_a}}},
      {"b", {{"source", ab_sources[1]}, {"summary", summary_b}}},
      {"difference", std::move(difference)},
      {"previousSource", request->ab_restore_source.value_or("")},
      {"restoredSource", request->ab_restored_source},
      {"note", "Frames alternate A, B, B, A, ... from the frame after the first switch; each side has frames samples. A difference whose ranges overlap is within frame-to-frame noise; run A against A (abSourceA = abSourceB) to see the noise floor. Effects that carry earlier frames (TAA history, eye adaptation, bloom feedback) mix both sides into each sample."},
  };
  if (!request->ab_restore_note.empty()) {
    result.structured_content["ab"]["restoreNote"] = request->ab_restore_note;
  }
  result.text += std::format(
      " A/B on shader 0x{:08X}: {} frames of '{}' (A) and of '{}' (B), alternating A, B, B, A; summaries and B minus A in ab. The shader's source is now '{}'.{}",
      ab_shader_hash.value(),
      frames,
      ab_sources[0],
      ab_sources[1],
      request->ab_restored_source,
      (request->ab_restore_note.empty() ? "" : std::format(" {}", request->ab_restore_note)));
  return finish(std::move(result));
}

// The effective transfer of one pass: its input and output read at the same present, paired pixel by pixel, binned by
// input luminance (log-spaced) with the median output per bin.
[[nodiscard]] inline ToolResult HandleMeasureToneCurveTool(const json& arguments, const ToolContext& context) {
  if (!context.resolve_device_index || !context.enqueue_request || !context.resolve_draw_curve_views) {
    throw std::runtime_error("AnalyzeResourceToolContext is not initialized.");
  }
  constexpr std::size_t MIN_BIN_PIXELS = 16u;
  constexpr double MID_GRAY = 0.18;

  const auto device_index = context.resolve_device_index(arguments);
  const auto draw_index = mcp_arguments::GetRequired<std::uint32_t>(arguments, "drawIndex");
  const auto input_slot = mcp_arguments::GetOptional<std::uint32_t>(arguments, "inputSlot");
  const auto input_signal = internal::ParseSignal(mcp_arguments::GetOptional<std::string>(arguments, "inputSignal").value_or("raw"));
  const auto output_signal = internal::ParseSignal(mcp_arguments::GetOptional<std::string>(arguments, "outputSignal").value_or("raw"));
  const auto region = internal::ParseRegion(arguments);
  const auto bin_count = mcp_arguments::ValidateRange<std::uint32_t>(
      "bins",
      mcp_arguments::GetOptional<std::uint32_t>(arguments, "bins").value_or(24u),
      4u,
      128u);
  const auto allow_later_writers = mcp_arguments::GetOptional<bool>(arguments, "allowLaterWriters").value_or(false);

  const auto views = context.resolve_draw_curve_views(device_index, draw_index, input_slot);
  if (!allow_later_writers && (!views.later_input_writers.empty() || !views.later_output_writers.empty())) {
    const auto error = std::format(
        "The snapshot frame writes the pass's input again at draw(s) {} and its output at draw(s) {}: read at the end of the "
        "frame, those textures no longer hold what this pass read or wrote, so the pairs would be wrong. Pick a pass "
        "whose textures stay untouched, or pass allowLaterWriters when the later writes cannot matter (e.g. UI blended "
        "outside the region).",
        json(views.later_input_writers).dump(),
        json(views.later_output_writers).dump());
    return ToolResult{
        .text = error,
        .structured_content = json{
            {"error", error},
            {"laterInputWriters", views.later_input_writers},
            {"laterOutputWriters", views.later_output_writers},
        },
        .is_error = true,
    };
  }

  // Each view resolves to a mod's active clone when it has one (see ProcessPendingRequests).
  auto request = std::make_shared<PendingRequest>();
  request->resource_view = views.output_view;
  request->region = region;
  request->signal = output_signal;
  request->curve_input_view = views.input_view;
  request->curve_input_signal = input_signal;
  if (!context.enqueue_request(device_index, request)) {
    throw std::runtime_error(std::format("Failed to queue the tone curve request for device #{}.", device_index));
  }
  {
    std::unique_lock request_lock(request->mutex);
    if (!request->cv.wait_for(request_lock, std::chrono::seconds(15), [&request] { return request->completed || request->started; })) {
      request->canceled = true;
      request_lock.unlock();
      if (context.cancel_request) {
        context.cancel_request(device_index, request);
      }
      throw std::runtime_error("Timed out waiting for the next present to read the pass's textures.");
    }
    request->cv.wait(request_lock, [&request] { return request->completed; });
  }
  if (!request->result.has_value()) {
    throw std::runtime_error("The tone curve request completed without a result.");
  }
  if (request->result->is_error) return request->result.value();

  const auto& input_luminance = request->curve_input_luminance;
  const auto& output_luminance = request->curve_output_luminance;
  if (input_luminance.empty() || input_luminance.size() != output_luminance.size()) {
    throw std::runtime_error(std::format(
        "The input ({} pixels) and output ({} pixels) do not pair up: the pass must read and write textures of the same size "
        "(use a region inside both), in a format the Devkit decodes.",
        input_luminance.size(),
        output_luminance.size()));
  }

  std::vector<std::pair<float, float>> pairs;
  pairs.reserve(input_luminance.size());
  for (std::size_t index = 0u; index < input_luminance.size(); ++index) {
    if (std::isfinite(input_luminance[index]) && std::isfinite(output_luminance[index]) && input_luminance[index] > 0.f) {
      pairs.emplace_back(input_luminance[index], output_luminance[index]);
    }
  }
  if (pairs.size() < MIN_BIN_PIXELS * 4u) {
    throw std::runtime_error(std::format("Only {} pixels have a finite, positive input: too few for a curve.", pairs.size()));
  }
  // Two 4K frames of floats are 66 MB; free them before the binning copies (32-bit games have little address space).
  std::vector<float>().swap(request->curve_input_luminance);
  std::vector<float>().swap(request->curve_output_luminance);

  // The bins span the input's 0.5th to 99.5th percentile, so a few stray pixels do not stretch them. One scratch buffer
  // serves the input percentiles, then the output peak.
  std::vector<float> scratch;
  scratch.reserve(pairs.size());
  for (const auto& [input_value, output_value] : pairs) {
    scratch.push_back(input_value);
  }
  const auto nth_value = [&scratch](std::size_t index) {
    std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(index), scratch.end());
    return static_cast<double>(scratch[index]);
  };
  const auto input_low = nth_value(scratch.size() / 200u);
  const auto input_high = nth_value(scratch.size() - 1u - (scratch.size() / 200u));
  if (!(input_high > input_low)) {
    throw std::runtime_error("The input luminance has no range in this region (a flat area?).");
  }
  const auto log_low = std::log(input_low);
  const auto log_step = (std::log(input_high) - log_low) / static_cast<double>(bin_count);

  std::vector<std::vector<float>> bin_inputs(bin_count);
  std::vector<std::vector<float>> bin_outputs(bin_count);
  scratch.clear();
  for (const auto& [input_value, output_value] : pairs) {
    scratch.push_back(output_value);
    const auto bin = static_cast<std::int64_t>(std::floor((std::log(static_cast<double>(input_value)) - log_low) / log_step));
    if (bin < 0 || bin >= static_cast<std::int64_t>(bin_count)) continue;
    bin_inputs[static_cast<std::size_t>(bin)].push_back(input_value);
    bin_outputs[static_cast<std::size_t>(bin)].push_back(output_value);
  }
  const auto median = [](std::vector<float>* values) {
    auto middle = values->begin() + static_cast<std::ptrdiff_t>(values->size() / 2u);
    std::nth_element(values->begin(), middle, values->end());
    return static_cast<double>(*middle);
  };

  struct CurvePoint {
    double input = 0.0;
    double output = 0.0;
    std::size_t count = 0u;
  };
  std::vector<CurvePoint> points;
  for (std::uint32_t bin = 0u; bin < bin_count; ++bin) {
    if (bin_outputs[bin].size() < MIN_BIN_PIXELS) continue;
    points.push_back({
        .input = median(&bin_inputs[bin]),
        .output = median(&bin_outputs[bin]),
        .count = bin_outputs[bin].size(),
    });
  }
  if (points.size() < 2u) {
    throw std::runtime_error("Fewer than two bins hold enough pixels for a curve; try a larger region or fewer bins.");
  }

  json curve_points = json::array();
  for (const auto& point : points) {
    curve_points.push_back({{"input", point.input}, {"output", point.output}, {"pixels", point.count}});
  }
  json result_json = {
      {"deviceIndex", device_index},
      {"drawIndex", draw_index},
      {"inputSlot", views.input_slot},
      {"inputSignal", std::string(internal::GetSignalName(input_signal))},
      {"outputSignal", std::string(internal::GetSignalName(output_signal))},
      {"pairedPixels", pairs.size()},
      {"inputRange", {input_low, input_high}},
      {"points", std::move(curve_points)},
      {"input", request->curve_input_result.value("analysisTarget", json(nullptr))},
      {"output", request->result->structured_content.value("analysisTarget", json(nullptr))},
  };
  if (!views.later_input_writers.empty() || !views.later_output_writers.empty()) {
    result_json["laterInputWriters"] = views.later_input_writers;
    result_json["laterOutputWriters"] = views.later_output_writers;
  }

  // Mid grey: the output at a linear 0.18 (14.4 nits as scRGB) and the log-log slope there, from the two points around
  // it. A PQ input is absolute display light, which has no scene mid grey, so none is reported for it.
  std::optional<double> mid_gray_input = std::nullopt;
  if (input_signal == Signal::RAW) {
    mid_gray_input = MID_GRAY;
  } else if (input_signal == Signal::SCRGB) {
    mid_gray_input = MID_GRAY * 80.0;
  }
  const auto above_mid_gray = std::ranges::find_if(points, [&](const CurvePoint& point) {
    return mid_gray_input.has_value() && point.input >= mid_gray_input.value();
  });
  if (mid_gray_input.has_value() && above_mid_gray != points.begin() && above_mid_gray != points.end()) {
    const auto& upper = *above_mid_gray;
    const auto& lower = *(above_mid_gray - 1);
    const auto fraction = (std::log(mid_gray_input.value()) - std::log(lower.input)) / (std::log(upper.input) - std::log(lower.input));
    json mid_gray = {
        {"input", mid_gray_input.value()},
        {"output", lower.output + (fraction * (upper.output - lower.output))},
    };
    if (lower.output > 0.0 && upper.output > 0.0) {
      mid_gray["logLogSlope"] = (std::log(upper.output) - std::log(lower.output)) / (std::log(upper.input) - std::log(lower.input));
    }
    result_json["midGray"] = std::move(mid_gray);
  }
  const auto peak_output = nth_value((scratch.size() - 1u) * 999u / 1000u);
  const auto highest_point = std::ranges::max_element(points, {}, &CurvePoint::output);
  result_json["peakOutputP99_9"] = peak_output;
  result_json["highestMedianOutput"] = {{"input", highest_point->input}, {"output", highest_point->output}};
  // Only a positive curve has a shoulder (a pass picked by mistake can output signed values). When only the top bin
  // reaches 90% of the highest median, the curve rises into the last bin and no shoulder is resolved below it.
  std::optional<double> shoulder_input = std::nullopt;
  bool still_rising_at_top = false;
  if (highest_point->output > 0.0) {
    const auto shoulder_point = std::ranges::find_if(points, [&](const CurvePoint& point) { return point.output >= highest_point->output * 0.9; });
    if (shoulder_point == points.end() - 1) {
      still_rising_at_top = true;
      result_json["shoulderInput"] = nullptr;
    } else {
      shoulder_input = shoulder_point->input;
      result_json["shoulderInput"] = shoulder_input.value();
    }
  }
  result_json["note"] =
      "An effective transfer: median output per log-spaced input bin of the pixels this pass read and wrote in one frame. "
      "Grading, LUTs, local effects (bloom, vignette) and UI also move pixels, so read it as the pass's overall mapping, "
      "not a formula. Output is in the output signal's unit (nits for pq/scrgb, decoded values for raw).";

  auto text = std::format(
      "Tone curve of draw #{} (input slot {}): {} paired pixels, input {:.4g}..{:.4g} in {} bins; peak output (p99.9) {:.4g}, "
      "highest median {:.4g} at input {:.4g}.",
      draw_index,
      views.input_slot,
      pairs.size(),
      input_low,
      input_high,
      points.size(),
      peak_output,
      highest_point->output,
      highest_point->input);
  if (shoulder_input.has_value()) {
    text += std::format(" 90% of that from input {:.4g}.", shoulder_input.value());
  }
  if (still_rising_at_top) {
    text += " Only the top bin reaches 90% of the highest median: no shoulder resolved below it (a clip or knee inside the top bin is not resolved; a smaller region or more bins can).";
  }
  if (result_json.contains("midGray")) {
    text += std::format(
        " At mid grey (input {:.4g}) the output is {:.4g}.",
        result_json["midGray"]["input"].get<double>(),
        result_json["midGray"]["output"].get<double>());
  }
  return ToolResult{
      .text = std::move(text),
      .structured_content = std::move(result_json),
  };
}

[[nodiscard]] inline ToolResult HandleDumpResourceWithHashTool(const json& arguments, const ToolContext& context) {
  if (!context.resolve_device_index || !context.enqueue_request) {
    throw std::runtime_error("AnalyzeResourceToolContext is not initialized.");
  }

  const auto resource_view_handle = resource_handle::GetRequired(arguments, "resourceViewHandle");
  const auto device_index = context.resolve_device_index(arguments);
  const auto output_directory_value = mcp_arguments::GetRequired<std::string>(arguments, "outputDirectory");
  const auto prefer_clone = mcp_arguments::GetOptional<bool>(arguments, "preferClone").value_or(false);
  const auto name_prefix = mcp_arguments::GetOptional<std::string>(arguments, "namePrefix");

  auto output_directory = std::filesystem::path(output_directory_value);
  if (!output_directory.is_absolute()) {
    throw std::runtime_error("outputDirectory must be an absolute path.");
  }
  output_directory = output_directory.lexically_normal();

  auto request = std::make_shared<PendingRequest>();
  request->resource_view = reshade::api::resource_view{resource_view_handle};
  request->output_path = std::nullopt;
  request->output_directory = output_directory;
  request->name_prefix = name_prefix.has_value() && !name_prefix->empty()
                             ? std::optional<std::string>(*name_prefix)
                             : std::nullopt;
  request->preview_mode = PreviewMode::CLAMPED;
  request->preview_mode_name = std::string(internal::GetPreviewModeName(PreviewMode::CLAMPED));
  request->prefer_clone = prefer_clone;

  if (!context.enqueue_request(device_index, request)) {
    const auto error = std::format("Failed to queue the resource dump request for device #{}.", device_index);
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }

  {
    std::unique_lock request_lock(request->mutex);
    if (!request->cv.wait_for(request_lock, std::chrono::seconds(15), [&request] { return request->completed || request->started; })) {
      request->canceled = true;
      request_lock.unlock();
      if (context.cancel_request) {
        context.cancel_request(device_index, request);
      }
      return ToolResult{
          .text = "Timed out waiting for the next present to process the resource dump request.",
          .structured_content = json{{"error", "Timed out waiting for the next present to process the resource dump request."}},
          .is_error = true,
      };
    }

    if (!request->completed) {
      request->cv.wait(request_lock, [&request] { return request->completed; });
    }
  }

  if (!request->result.has_value()) {
    return ToolResult{
        .text = "The resource dump request completed without a result.",
        .structured_content = json{{"error", "The resource dump request completed without a result."}},
        .is_error = true,
    };
  }

  return request->result.value();
}

[[nodiscard]] inline ToolResult HandleAnalyzeLutTool(const json& arguments, const ToolContext& context) {
  if (!context.resolve_device_index || !context.enqueue_request) {
    throw std::runtime_error("AnalyzeResourceToolContext is not initialized.");
  }

  const auto resource_view_handle = resource_handle::GetRequired(arguments, "resourceViewHandle");
  const auto device_index = context.resolve_device_index(arguments);
  auto lut_options = LutOptions{
      .layout = mcp_arguments::GetOptional<std::string>(arguments, "layout").value_or("auto"),
      .flip_y = mcp_arguments::GetOptional<bool>(arguments, "flipY").value_or(false),
  };
  if (lut_options.layout != "auto" && lut_options.layout != "3d" && lut_options.layout != "strip_x" && lut_options.layout != "strip_y") {
    throw std::runtime_error("layout must be 'auto', '3d', 'strip_x' or 'strip_y'.");
  }
  if (const auto output_path = mcp_arguments::GetOptional<std::string>(arguments, "outputPath"); output_path.has_value()) {
    const auto csv_path = std::filesystem::path(output_path.value());
    if (!csv_path.is_absolute() || csv_path.extension() != ".csv") {
      throw std::runtime_error("outputPath must be an absolute path ending in .csv.");
    }
    lut_options.csv_path = csv_path.lexically_normal();
  }

  auto request = std::make_shared<PendingRequest>();
  request->resource_view = reshade::api::resource_view{resource_view_handle};
  request->prefer_clone = mcp_arguments::GetOptional<bool>(arguments, "preferClone").value_or(false);
  request->lut = std::move(lut_options);

  if (!context.enqueue_request(device_index, request)) {
    const auto error = std::format("Failed to queue the LUT analysis request for device #{}.", device_index);
    return ToolResult{
        .text = error,
        .structured_content = json{{"error", error}},
        .is_error = true,
    };
  }

  {
    std::unique_lock request_lock(request->mutex);
    if (!request->cv.wait_for(request_lock, std::chrono::seconds(15), [&request] { return request->completed || request->started; })) {
      request->canceled = true;
      request_lock.unlock();
      if (context.cancel_request) {
        context.cancel_request(device_index, request);
      }
      return ToolResult{
          .text = "Timed out waiting for the next present to process the LUT analysis request.",
          .structured_content = json{{"error", "Timed out waiting for the next present to process the LUT analysis request."}},
          .is_error = true,
      };
    }

    if (!request->completed) {
      request->cv.wait(request_lock, [&request] { return request->completed; });
    }
  }

  if (!request->result.has_value()) {
    return ToolResult{
        .text = "The LUT analysis request completed without a result.",
        .structured_content = json{{"error", "The LUT analysis request completed without a result."}},
        .is_error = true,
    };
  }

  return request->result.value();
}

// Returns the multi-frame requests that still need frames; the caller queues them again for the next present.
[[nodiscard]] inline std::deque<std::shared_ptr<PendingRequest>> ProcessPendingRequests(
    const std::deque<std::shared_ptr<PendingRequest>>& requests,
    std::uint32_t device_index,
    reshade::api::device* device,
    reshade::api::command_queue* queue,
    const ProcessingContext& context) {
  std::deque<std::shared_ptr<PendingRequest>> unfinished;
  for (const auto& request : requests) {
    const auto make_error = [](const std::string& error) {
      return ToolResult{
          .text = error,
          .structured_content = json{{"error", error}},
          .is_error = true,
      };
    };
    const auto complete = [&request](ToolResult result) {
      {
        std::scoped_lock request_lock(request->mutex);
        request->result = std::move(result);
        request->completed = true;
      }
      request->cv.notify_all();
    };
    // An A/B ends with the shader's source as it was, whether it finishes, fails or is canceled. The recorded source can
    // describe nothing active (a live file that failed to compile), so a failed restore falls back to the add-on's
    // shader, then to the original, and says so in ab_restore_note.
    const auto restore_ab_source = [&]() {
      if (!request->ab_restore_source.has_value()) return;
      for (const auto* fallback_source : {request->ab_restore_source->c_str(), "addon", "original"}) {
        try {
          (void)context.set_shader_source(device, request->ab_shader_hash.value(), fallback_source, true);
        } catch (const std::exception& exception) {
          request->ab_restore_note += std::format("Restoring '{}' failed: {} ", fallback_source, exception.what());
          continue;
        }
        request->ab_restored_source = fallback_source;
        return;
      }
    };

    bool canceled = false;
    {
      std::scoped_lock request_lock(request->mutex);
      if (request->completed) {
        request->cv.notify_all();
        continue;
      }
      canceled = request->canceled;
      if (!canceled) {
        request->started = true;
      }
    }
    if (canceled) {
      restore_ab_source();
      complete(make_error("The resource analysis request was canceled."));
      continue;
    }
    request->cv.notify_all();
    ++request->presents_seen;

    // A/B: the first present only switches to side A, since the frame it ends was drawn before the switch.
    if (request->ab_shader_hash.has_value() && !request->ab_restore_source.has_value()) {
      try {
        request->ab_restore_source = context.set_shader_source(device, request->ab_shader_hash.value(), request->ab_sources[0], false);
      } catch (const std::exception& exception) {
        complete(make_error(exception.what()));
        continue;
      }
      request->ab_current_side = 0u;
      unfinished.push_back(request);
      continue;
    }

    // The tone curve reads what the GPU really renders: a mod's clone where one is active, else the resource itself (a
    // game without a mod, or a texture the mod leaves alone). Explicit preferClone elsewhere stays strict.
    const auto has_active_clone = [](reshade::api::resource_view resource_view) {
      const auto view_info = internal::FindTrackedResourceViewInfo(resource_view);
      if (!view_info.has_value() || view_info->clone.handle == 0u) return false;
      bool clone_active = false;
      renodx::utils::resource::GetResourceInfo(view_info->original_resource, [&](const renodx::utils::resource::ResourceInfo& info) {
        clone_active = info.clone_enabled && info.clone.handle != 0u;
      });
      return clone_active;
    };

    // Tone curve: the input first, in the same present as the output below.
    if (request->curve_input_view.has_value()) {
      auto input_result = make_error("Unknown resource analysis failure.");
      try {
        input_result = internal::PerformResourceAnalysis(
            context,
            device_index,
            device,
            queue,
            request->curve_input_view.value(),
            std::nullopt,
            std::nullopt,
            std::nullopt,
            request->preview_mode,
            request->preview_mode_name,
            has_active_clone(request->curve_input_view.value()),
            request->region,
            request->curve_input_signal,
            {0u},
            &request->curve_input_luminance);
      } catch (const std::exception& exception) {
        input_result = make_error(exception.what());
      }
      if (input_result.is_error) {
        complete(make_error(std::format("Reading the pass's input failed: {}", input_result.text)));
        continue;
      }
      request->curve_input_result = std::move(input_result.structured_content);
    }

    ToolResult result = make_error("Unknown resource analysis failure.");
    try {
      result = internal::PerformResourceAnalysis(
          context,
          device_index,
          device,
          queue,
          request->resource_view,
          request->output_path,
          request->output_directory,
          request->name_prefix,
          request->preview_mode,
          request->preview_mode_name,
          (request->curve_input_view.has_value() ? has_active_clone(request->resource_view) : request->prefer_clone),
          request->region,
          request->signal,
          request->frame_end_copy,
          (request->curve_input_view.has_value() ? &request->curve_output_luminance : nullptr),
          (request->lut.has_value() ? &request->lut.value() : nullptr),
          request->banding);
    } catch (const std::exception& exception) {
      result = make_error(exception.what());
    }
    if (!result.is_error && result.structured_content.is_object() && !request->lut.has_value()) {
      result.structured_content["measurePoint"] = (request->at_frame_end ? "frame_end" : "present");
    }

    if (request->frames > 1u && !result.is_error) {
      json sample = {
          {"frame", request->presents_seen},
          {"analysis", result.structured_content["analysis"]},
      };
      if (request->ab_shader_hash.has_value()) {
        sample["side"] = (request->ab_current_side == 0u ? "a" : "b");
      }
      request->samples.push_back(std::move(sample));
      if (request->samples.size() < request->frames) {
        if (request->ab_shader_hash.has_value()) {
          // A, B, B, A, ...: balances a scene that alternates between two states from frame to frame.
          const auto sample_index = request->samples.size() % 4u;
          const std::size_t next_side = ((sample_index == 0u || sample_index == 3u) ? 0u : 1u);
          if (next_side != request->ab_current_side) {
            try {
              (void)context.set_shader_source(device, request->ab_shader_hash.value(), request->ab_sources[next_side], false);
            } catch (const std::exception& exception) {
              restore_ab_source();
              complete(make_error(std::format(
                  "Switching to side {} failed: {} The shader's source is now '{}'. {}",
                  (next_side == 0u ? "A" : "B"),
                  exception.what(),
                  request->ab_restored_source,
                  request->ab_restore_note)));
              continue;
            }
            request->ab_current_side = next_side;
          }
        }
        unfinished.push_back(request);
        continue;
      }
    }

    restore_ab_source();
    complete(std::move(result));
  }
  return unfinished;
}

}  // namespace renodx::addons::devkit::mcp::resource_analysis
