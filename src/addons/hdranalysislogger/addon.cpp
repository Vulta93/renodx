/*
 * Copyright (C) 2026 Vulta93
 * SPDX-License-Identifier: MIT
 */

// Reads the statistics computed by Lilium's HDR/SDR analysis ReShade effect
// (lilium__hdr_and_sdr_analysis.fx, EndlesslyFlowering/ReShade_HDR_shaders)
// back to the CPU and writes them as JSON, so the values shown on its overlay
// can be consumed without screenshots. It also measures a small region of the
// back buffer (screen centre or a picked position) to isolate one light source.

#define ImTextureID ImU64

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>
#include <nlohmann/json.hpp>


namespace {

using json = nlohmann::ordered_json;

constexpr const char* EFFECT_NAME = "lilium__hdr_and_sdr_analysis.fx";
constexpr const char* TECHNIQUE_NAME = "lilium__hdr_and_sdr_analysis";
constexpr const char* CONSOLIDATED_TEXTURE_NAME = "TextureConsolidated";
constexpr const char* CONFIG_SECTION = "HDRAnalysisLogger";

// The effect stores its per-frame statistics in TextureConsolidated (R32F).
// Its width depends on the compile-time colour space and on whether the API
// supports compute shaders, so the width identifies the layout. Offsets follow
// the COORDS_* defines in lilium__include/hdr_and_sdr_analysis/main.fxh:
// index 0 is the overlay update timer, then max/min/avg luminance (and, on the
// compute path, R/G/B and CLL), then the gamut percentages.
struct ConsolidatedLayout {
  uint32_t texture_width = 0;
  const char* colour_space = "";
  const char* units = "";
  bool has_rgb_and_cll = false;
  uint32_t gamut_count = 0;
};

constexpr std::array<ConsolidatedLayout, 6> CONSOLIDATED_LAYOUTS = {{
    {.texture_width = 53, .colour_space = "scrgb", .units = "nits", .has_rgb_and_cll = true, .gamut_count = 5},
    {.texture_width = 49, .colour_space = "hdr10", .units = "nits", .has_rgb_and_cll = true, .gamut_count = 3},
    {.texture_width = 43, .colour_space = "sdr", .units = "percent", .has_rgb_and_cll = true, .gamut_count = 0},
    {.texture_width = 9, .colour_space = "scrgb", .units = "nits", .has_rgb_and_cll = false, .gamut_count = 5},
    {.texture_width = 7, .colour_space = "hdr10", .units = "nits", .has_rgb_and_cll = false, .gamut_count = 3},
    {.texture_width = 4, .colour_space = "sdr", .units = "percent", .has_rgb_and_cll = false, .gamut_count = 0},
}};

constexpr std::array<const char*, 5> GAMUT_KEYS = {
    "gamut_bt709_percent",
    "gamut_dci_p3_percent",
    "gamut_bt2020_percent",
    "gamut_ap0_percent",
    "gamut_invalid_percent",
};

struct ActiveAreaCrop {
  const char* uniform_name = "";
  const char* key = "";
};

constexpr std::array<ActiveAreaCrop, 4> ACTIVE_AREA_CROPS = {{
    {.uniform_name = "ACTIVE_AREA_CROP_LEFT", .key = "crop_left_percent"},
    {.uniform_name = "ACTIVE_AREA_CROP_TOP", .key = "crop_top_percent"},
    {.uniform_name = "ACTIVE_AREA_CROP_RIGHT", .key = "crop_right_percent"},
    {.uniform_name = "ACTIVE_AREA_CROP_BOTTOM", .key = "crop_bottom_percent"},
}};

// Luminance weights and the BT.709 to BT.2020 matrix, matching how the effect
// reports luminance and per-channel values (BT.2020 "optical" values in HDR,
// BT.709 relative values in SDR).
constexpr std::array<float, 3> BT709_LUMINANCE = {0.2126390f, 0.7151687f, 0.0721923f};
constexpr std::array<float, 3> BT2020_LUMINANCE = {0.2627002f, 0.6779981f, 0.0593017f};
constexpr std::array<std::array<float, 3>, 3> BT709_TO_BT2020 = {{
    {0.6274040f, 0.3292820f, 0.0433136f},
    {0.0690970f, 0.9195400f, 0.0113612f},
    {0.0163916f, 0.0880132f, 0.8955950f},
}};

constexpr auto SERIES_SAMPLE_INTERVAL = std::chrono::milliseconds(100);

constexpr int REGION_ANCHOR_SCREEN_CENTRE = 0;
constexpr int REGION_ANCHOR_DRAWN = 1;

struct Series {
  std::string label;
  std::chrono::steady_clock::time_point end_time;
  json samples = json::array();
};

struct Hotkey {
  uint32_t key = VK_F9;
  bool ctrl = false;
  bool shift = true;
  bool alt = false;
};

struct Readback {
  reshade::api::device* device = nullptr;
  reshade::api::resource staging = {0u};
  reshade::api::resource_desc desc = {};
};

struct RegionBox {
  uint32_t left = 0;
  uint32_t top = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

int sample_interval_ms = 500;
bool write_continuous_log = true;
int series_duration_seconds = 5;
std::array<char, 64> series_label = {"scene"};
Hotkey series_hotkey;
int region_anchor = REGION_ANCHOR_SCREEN_CENTRE;
// Drawn rectangle as fractions of the screen (left, top, right, bottom), so it
// survives resolution changes.
std::array<float, 4> region_rect = {0.45f, 0.45f, 0.55f, 0.55f};
int region_size = 16;
bool show_region_outline = true;

bool capturing_hotkey = false;
bool drawing_region = false;
// While drawing: the first corner of a new rectangle, or (when moving) the
// grab offset from the existing box's top-left corner to the mouse.
std::optional<ImVec2> drag_start;
std::optional<ImVec2> move_grab_offset;
Readback analysis_readback;
Readback region_readback;
std::optional<Series> series;
std::ofstream session_log;
std::chrono::steady_clock::time_point last_sample_time;
uint64_t frame_index = 0;
uint32_t back_buffer_width = 0;
uint32_t back_buffer_height = 0;
bool sample_pending = false;
std::optional<RegionBox> copied_region;
std::string status = "Waiting for the first sample.";
std::string completion_message;
std::chrono::steady_clock::time_point completion_message_end_time;
json latest_sample;

// <ReShade base folder>/hdr_analysis, normally next to ReShade.ini.
std::filesystem::path GetOutputFolder() {
  static const std::filesystem::path OUTPUT_FOLDER = [] {
    size_t path_size = 0;
    reshade::get_reshade_base_path(nullptr, &path_size);
    std::string base_path(path_size, '\0');
    reshade::get_reshade_base_path(base_path.data(), &path_size);
    base_path.resize(std::strlen(base_path.c_str()));
    return std::filesystem::path(std::u8string(base_path.begin(), base_path.end())) / "hdr_analysis";
  }();
  return OUTPUT_FOLDER;
}

const char* FormatName(reshade::api::format format) {
  switch (format) {
    case reshade::api::format::r8g8b8a8_unorm:      return "r8g8b8a8_unorm";
    case reshade::api::format::r8g8b8a8_unorm_srgb: return "r8g8b8a8_unorm_srgb";
    case reshade::api::format::b8g8r8a8_unorm:      return "b8g8r8a8_unorm";
    case reshade::api::format::b8g8r8a8_unorm_srgb: return "b8g8r8a8_unorm_srgb";
    case reshade::api::format::r8g8b8x8_unorm:      return "r8g8b8x8_unorm";
    case reshade::api::format::b8g8r8x8_unorm:      return "b8g8r8x8_unorm";
    case reshade::api::format::r10g10b10a2_unorm:   return "r10g10b10a2_unorm";
    case reshade::api::format::b10g10r10a2_unorm:   return "b10g10r10a2_unorm";
    case reshade::api::format::r11g11b10_float:     return "r11g11b10_float";
    case reshade::api::format::r16g16b16a16_float:  return "r16g16b16a16_float";
    default:                                        return "other";
  }
}

std::string GetFileTimestamp() {
  const auto now = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
  return std::format("{:%Y%m%d-%H%M%S}", now);
}

void WriteJsonFile(const std::filesystem::path& path, const json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::trunc);
  file << value.dump(2) << '\n';
}

std::string FormatHotkey(const Hotkey& hotkey) {
  std::string text;
  if (hotkey.ctrl) text += "Ctrl+";
  if (hotkey.shift) text += "Shift+";
  if (hotkey.alt) text += "Alt+";

  // Extended keys (arrows, Insert, Home, ...) need bit 24 set or
  // GetKeyNameText returns the numpad name.
  const auto scan_code = MapVirtualKeyA(hotkey.key, MAPVK_VK_TO_VSC_EX);
  const auto extended_flag = ((scan_code & 0xFF00u) != 0u ? (1u << 24) : 0u);
  std::array<char, 64> key_name = {};
  if (GetKeyNameTextA(static_cast<LONG>(((scan_code & 0xFFu) << 16) | extended_flag), key_name.data(), static_cast<int>(key_name.size())) > 0) {
    text += key_name.data();
  } else {
    text += std::format("VK 0x{:02X}", hotkey.key);
  }
  return text;
}

RegionBox ComputeRegionBox(uint32_t width, uint32_t height) {
  if (region_anchor == REGION_ANCHOR_DRAWN) {
    const auto left = std::min(static_cast<uint32_t>(region_rect[0] * width), width - 1);
    const auto top = std::min(static_cast<uint32_t>(region_rect[1] * height), height - 1);
    const auto right = std::clamp(static_cast<uint32_t>(std::ceil(region_rect[2] * width)), left + 1, width);
    const auto bottom = std::clamp(static_cast<uint32_t>(std::ceil(region_rect[3] * height)), top + 1, height);
    return {.left = left, .top = top, .width = right - left, .height = bottom - top};
  }
  const auto size = std::min({static_cast<uint32_t>(std::max(region_size, 1)), width, height});
  return {.left = (width - size) / 2, .top = (height - size) / 2, .width = size, .height = size};
}

void DestroyReadback(Readback* readback) {
  if (readback->staging.handle != 0u) {
    readback->device->destroy_resource(readback->staging);
  }
  *readback = {};
}

bool PrepareReadback(reshade::api::device* device, const reshade::api::resource_desc& desc, Readback* readback) {
  if (readback->staging.handle != 0u
      && readback->device == device
      && readback->desc.type == desc.type
      && readback->desc.texture.width == desc.texture.width
      && readback->desc.texture.height == desc.texture.height
      && readback->desc.texture.format == desc.texture.format) {
    return true;
  }
  DestroyReadback(readback);
  reshade::api::resource staging = {0u};
  if (!device->create_resource(desc, nullptr, reshade::api::resource_usage::copy_dest, &staging)) return false;
  *readback = {.device = device, .staging = staging, .desc = desc};
  return true;
}

void SaveSettings(reshade::api::effect_runtime* runtime) {
  reshade::set_config_value(runtime, CONFIG_SECTION, "SampleIntervalMs", sample_interval_ms);
  reshade::set_config_value(runtime, CONFIG_SECTION, "WriteContinuousLog", write_continuous_log);
  reshade::set_config_value(runtime, CONFIG_SECTION, "SeriesDurationSeconds", series_duration_seconds);
  reshade::set_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyKey", series_hotkey.key);
  reshade::set_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyCtrl", series_hotkey.ctrl);
  reshade::set_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyShift", series_hotkey.shift);
  reshade::set_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyAlt", series_hotkey.alt);
  reshade::set_config_value(runtime, CONFIG_SECTION, "RegionAnchor", region_anchor);
  reshade::set_config_value(runtime, CONFIG_SECTION, "RegionLeft", region_rect[0]);
  reshade::set_config_value(runtime, CONFIG_SECTION, "RegionTop", region_rect[1]);
  reshade::set_config_value(runtime, CONFIG_SECTION, "RegionRight", region_rect[2]);
  reshade::set_config_value(runtime, CONFIG_SECTION, "RegionBottom", region_rect[3]);
  reshade::set_config_value(runtime, CONFIG_SECTION, "RegionSize", region_size);
  reshade::set_config_value(runtime, CONFIG_SECTION, "ShowRegionOutline", show_region_outline);
}

void StartSeries() {
  series = Series{
      .label = series_label.data(),
      .end_time = std::chrono::steady_clock::now() + std::chrono::seconds(series_duration_seconds),
  };
  status = std::format("Recording series \"{}\" for {} s.", series->label, series_duration_seconds);
  reshade::log::message(reshade::log::level::info, std::format("HDRAnalysisLogger: {}", status).c_str());
}

void OnInitEffectRuntime(reshade::api::effect_runtime* runtime) {
  reshade::get_config_value(runtime, CONFIG_SECTION, "SampleIntervalMs", sample_interval_ms);
  reshade::get_config_value(runtime, CONFIG_SECTION, "WriteContinuousLog", write_continuous_log);
  reshade::get_config_value(runtime, CONFIG_SECTION, "SeriesDurationSeconds", series_duration_seconds);
  reshade::get_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyKey", series_hotkey.key);
  reshade::get_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyCtrl", series_hotkey.ctrl);
  reshade::get_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyShift", series_hotkey.shift);
  reshade::get_config_value(runtime, CONFIG_SECTION, "SeriesHotkeyAlt", series_hotkey.alt);
  reshade::get_config_value(runtime, CONFIG_SECTION, "RegionAnchor", region_anchor);
  reshade::get_config_value(runtime, CONFIG_SECTION, "RegionLeft", region_rect[0]);
  reshade::get_config_value(runtime, CONFIG_SECTION, "RegionTop", region_rect[1]);
  reshade::get_config_value(runtime, CONFIG_SECTION, "RegionRight", region_rect[2]);
  reshade::get_config_value(runtime, CONFIG_SECTION, "RegionBottom", region_rect[3]);
  reshade::get_config_value(runtime, CONFIG_SECTION, "RegionSize", region_size);
  reshade::get_config_value(runtime, CONFIG_SECTION, "ShowRegionOutline", show_region_outline);
}

void OnDestroyEffectRuntime(reshade::api::effect_runtime* runtime) {
  auto* device = runtime->get_device();
  if (analysis_readback.device == device) {
    DestroyReadback(&analysis_readback);
  }
  if (region_readback.device == device) {
    DestroyReadback(&region_readback);
  }
}

// The region is copied before any effect runs, so it measures the game image
// without Lilium's overlay, heatmap or active-area blackout drawn into it.
void OnReShadeBeginEffects(
    reshade::api::effect_runtime* runtime,
    reshade::api::command_list* /*cmd_list*/,
    reshade::api::resource_view rtv,
    reshade::api::resource_view /*rtv_srgb*/) {
  ++frame_index;

  if (!series.has_value()
      && !capturing_hotkey
      && runtime->is_key_pressed(series_hotkey.key)
      && runtime->is_key_down(VK_CONTROL) == series_hotkey.ctrl
      && runtime->is_key_down(VK_SHIFT) == series_hotkey.shift
      && runtime->is_key_down(VK_MENU) == series_hotkey.alt) {
    StartSeries();
  }

  auto* device = runtime->get_device();
  const auto back_buffer = device->get_resource_from_view(rtv);
  const auto back_buffer_desc = device->get_resource_desc(back_buffer);
  back_buffer_width = back_buffer_desc.texture.width;
  back_buffer_height = back_buffer_desc.texture.height;

  const auto now = std::chrono::steady_clock::now();
  const auto interval = (series.has_value()
                             ? std::min<std::chrono::milliseconds>(SERIES_SAMPLE_INTERVAL, std::chrono::milliseconds(sample_interval_ms))
                             : std::chrono::milliseconds(sample_interval_ms));
  if (now - last_sample_time < interval) return;
  last_sample_time = now;
  sample_pending = true;
  copied_region.reset();

  if (back_buffer_desc.texture.samples != 1 || back_buffer_width == 0 || back_buffer_height == 0) return;

  const auto region_box = ComputeRegionBox(back_buffer_width, back_buffer_height);
  const auto staging_desc = reshade::api::resource_desc(
      reshade::api::resource_type::texture_2d,
      region_box.width,
      region_box.height,
      1,
      1,
      reshade::api::format_to_default_typed(back_buffer_desc.texture.format, 0),
      1,
      reshade::api::memory_heap::gpu_to_cpu,
      reshade::api::resource_usage::copy_dest);
  if (!PrepareReadback(device, staging_desc, &region_readback)) return;

  // ReShade transitions the back buffer to render_target before rendering
  // effects. Recording on the immediate command list keeps the copy ahead of
  // the effect passes; it is flushed and waited on in OnReShadeFinishEffects.
  const reshade::api::subresource_box source_box = {
      .left = region_box.left,
      .top = region_box.top,
      .front = 0,
      .right = region_box.left + region_box.width,
      .bottom = region_box.top + region_box.height,
      .back = 1,
  };
  auto* copy_cmd_list = runtime->get_command_queue()->get_immediate_command_list();
  copy_cmd_list->barrier(back_buffer, reshade::api::resource_usage::render_target, reshade::api::resource_usage::copy_source);
  copy_cmd_list->copy_texture_region(back_buffer, 0, &source_box, region_readback.staging, 0, nullptr);
  copy_cmd_list->barrier(back_buffer, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::render_target);
  copied_region = region_box;
}

void OnReShadeFinishEffects(
    reshade::api::effect_runtime* runtime,
    reshade::api::command_list* /*cmd_list*/,
    reshade::api::resource_view /*rtv*/,
    reshade::api::resource_view /*rtv_srgb*/) {
  if (!sample_pending) return;
  sample_pending = false;

  auto* device = runtime->get_device();
  auto* queue = runtime->get_command_queue();

  // Locate Lilium's statistics texture. The region is still logged when the
  // effect is missing or disabled.
  std::string analysis_status;
  const ConsolidatedLayout* layout = nullptr;
  const auto technique = runtime->find_technique(EFFECT_NAME, TECHNIQUE_NAME);
  const auto texture_variable = runtime->find_texture_variable(EFFECT_NAME, CONSOLIDATED_TEXTURE_NAME);
  reshade::api::resource_view texture_srv = {0u};
  if (texture_variable.handle != 0u) {
    runtime->get_texture_binding(texture_variable, &texture_srv, nullptr);
  }
  if (technique.handle == 0u || !runtime->get_technique_state(technique)) {
    analysis_status = "Lilium's HDR/SDR analysis technique is not loaded or not enabled.";
  } else if (texture_srv.handle == 0u) {
    analysis_status = "TextureConsolidated was not found in the analysis effect.";
  } else {
    const auto texture = device->get_resource_from_view(texture_srv);
    const auto texture_desc = device->get_resource_desc(texture);
    const auto texture_width = texture_desc.texture.width;
    const auto layout_it = std::ranges::find(CONSOLIDATED_LAYOUTS, texture_width, &ConsolidatedLayout::texture_width);
    const auto staging_desc = reshade::api::resource_desc(
        texture_desc.type,
        texture_width,
        1,
        1,
        1,
        texture_desc.texture.format,
        1,
        reshade::api::memory_heap::gpu_to_cpu,
        reshade::api::resource_usage::copy_dest);
    if (layout_it == CONSOLIDATED_LAYOUTS.end() || texture_desc.texture.format != reshade::api::format::r32_float) {
      analysis_status = std::format("Unknown TextureConsolidated layout (width {}). The effect version may have changed.", texture_width);
    } else if (!PrepareReadback(device, staging_desc, &analysis_readback)) {
      analysis_status = "Failed to create the analysis readback resource.";
    } else {
      // ReShade leaves effect textures in the shader resource state after the
      // technique runs.
      auto* copy_cmd_list = queue->get_immediate_command_list();
      copy_cmd_list->barrier(texture, reshade::api::resource_usage::shader_resource, reshade::api::resource_usage::copy_source);
      copy_cmd_list->copy_texture_region(texture, 0, nullptr, analysis_readback.staging, 0, nullptr);
      copy_cmd_list->barrier(texture, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::shader_resource);
      layout = &*layout_it;
    }
  }

  if (layout == nullptr && !copied_region.has_value()) {
    status = analysis_status;
    return;
  }

  // Waiting here makes the mapped data belong to this frame. Sampling is
  // rate-limited so the stall stays cheap.
  queue->flush_immediate_command_list();
  queue->wait_idle();

  std::wstring module_path(MAX_PATH, L'\0');
  module_path.resize(GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size())));
  const std::filesystem::path game_folder = module_path;
  const auto* device_api = [&]() {
    switch (device->get_api()) {
      case reshade::api::device_api::d3d9:   return "d3d9";
      case reshade::api::device_api::d3d10:  return "d3d10";
      case reshade::api::device_api::d3d11:  return "d3d11";
      case reshade::api::device_api::d3d12:  return "d3d12";
      case reshade::api::device_api::opengl: return "opengl";
      case reshade::api::device_api::vulkan: return "vulkan";
      default:                               return "unknown";
    }
  }();
  const auto local_now = std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
  latest_sample = json{
      {"time", std::format("{:%FT%T}", local_now)},
      {"frame", frame_index},
      {"game_folder", game_folder.parent_path().filename().string()},
      {"api", device_api},
      {"back_buffer_format", FormatName(device->get_resource_desc(runtime->get_current_back_buffer()).texture.format)},
  };

  reshade::api::subresource_data mapped = {};
  if (layout != nullptr && device->map_texture_region(analysis_readback.staging, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
    std::vector<float> v(layout->texture_width);
    std::memcpy(v.data(), mapped.data, v.size() * sizeof(float));
    device->unmap_texture_region(analysis_readback.staging, 0);

    // Averages are clamped to [min, max] the same way the effect does before
    // displaying them (CopyShowValues in main.fxh).
    const auto clamp_average = [](float average, float minimum, float maximum) {
      return std::min(std::max(average, minimum), maximum);
    };
    json values = json::object();
    values["max_luminance"] = v[1];
    values["avg_luminance"] = clamp_average(v[3], v[2], v[1]);
    values["min_luminance"] = v[2];
    if (layout->has_rgb_and_cll) {
      values["max_r"] = v[4];
      values["avg_r"] = clamp_average(v[7], v[10], v[4]);
      values["min_r"] = v[10];
      values["max_g"] = v[5];
      values["avg_g"] = clamp_average(v[8], v[11], v[5]);
      values["min_g"] = v[11];
      values["max_b"] = v[6];
      values["avg_b"] = clamp_average(v[9], v[12], v[6]);
      values["min_b"] = v[12];
      values["max_cll"] = v[13];
      values["avg_cll"] = clamp_average(v[14], v[15], v[13]);
      values["min_cll"] = v[15];
    }
    const uint32_t gamut_offset = (layout->has_rgb_and_cll ? 16u : 4u);
    for (uint32_t gamut_index = 0; gamut_index < layout->gamut_count; ++gamut_index) {
      values[GAMUT_KEYS[gamut_index]] = v[gamut_offset + gamut_index];
    }

    // Uniform names carry an SDR_ prefix when the effect is compiled for SDR.
    const auto* uniform_prefix = (std::string_view(layout->colour_space) == "sdr" ? "SDR_" : "");
    json active_area = json::object();
    if (const auto uniform = runtime->find_uniform_variable(EFFECT_NAME, std::format("{}ACTIVE_AREA_ENABLE", uniform_prefix).c_str());
        uniform.handle != 0u) {
      bool enabled = false;
      runtime->get_uniform_value_bool(uniform, &enabled, 1);
      active_area["enabled"] = enabled;
    }
    for (const auto& crop : ACTIVE_AREA_CROPS) {
      const auto uniform = runtime->find_uniform_variable(EFFECT_NAME, std::format("{}{}", uniform_prefix, crop.uniform_name).c_str());
      if (uniform.handle == 0u) continue;
      float crop_percent = 0.f;
      runtime->get_uniform_value_float(uniform, &crop_percent, 1);
      active_area[crop.key] = crop_percent;
    }

    latest_sample["colour_space"] = layout->colour_space;
    latest_sample["units"] = layout->units;
    latest_sample["active_area"] = active_area;
    latest_sample["values"] = values;
  } else {
    latest_sample["analysis_status"] = (layout == nullptr ? analysis_status : "Failed to map the analysis readback resource.");
  }

  // The region uses the same colour space interpretation as the effect's
  // defaults: 16-bit float is scRGB, 10-bit is HDR10 PQ, 8-bit is gamma 2.2.
  const auto region_format = region_readback.desc.texture.format;
  const char* region_colour_space = nullptr;
  switch (region_format) {
    case reshade::api::format::r16g16b16a16_float: region_colour_space = "scrgb"; break;
    case reshade::api::format::r10g10b10a2_unorm:
    case reshade::api::format::b10g10r10a2_unorm:  region_colour_space = "hdr10"; break;
    case reshade::api::format::r8g8b8a8_unorm:
    case reshade::api::format::b8g8r8a8_unorm:
    case reshade::api::format::r8g8b8x8_unorm:
    case reshade::api::format::b8g8r8x8_unorm:     region_colour_space = "sdr"; break;
    default:                                       break;
  }
  if (copied_region.has_value() && region_colour_space == nullptr) {
    latest_sample["region"] = {{"status", std::format("Unsupported back buffer format {}.", FormatName(region_format))}};
  } else if (copied_region.has_value() && device->map_texture_region(region_readback.staging, 0, nullptr, reshade::api::map_access::read_only, &mapped)) {
    const auto box = copied_region.value();
    const bool is_sdr = (std::string_view(region_colour_space) == "sdr");
    std::array<double, 4> channel_max;
    std::array<double, 4> channel_min;
    std::array<double, 4> channel_sum = {};
    channel_max.fill(-std::numeric_limits<double>::infinity());
    channel_min.fill(std::numeric_limits<double>::infinity());
    for (uint32_t y = 0; y < box.height; ++y) {
      const auto* row = static_cast<const uint8_t*>(mapped.data) + (static_cast<size_t>(mapped.row_pitch) * y);
      for (uint32_t x = 0; x < box.width; ++x) {
        std::array<float, 3> rgb = {};
        switch (region_format) {
          case reshade::api::format::r16g16b16a16_float: {
            const auto decode_half = [](uint16_t half) {
              const auto exponent = static_cast<int>((half >> 10) & 0x1Fu);
              const auto mantissa = static_cast<float>(half & 0x3FFu);
              float magnitude = 0.f;
              if (exponent == 0) {
                magnitude = std::ldexp(mantissa, -24);
              } else if (exponent == 31) {
                magnitude = (mantissa == 0.f ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN());
              } else {
                magnitude = std::ldexp(mantissa + 1024.f, exponent - 25);
              }
              return ((half & 0x8000u) != 0u ? -magnitude : magnitude);
            };
            std::array<uint16_t, 3> half = {};
            std::memcpy(half.data(), row + (x * 8u), sizeof(half));
            const std::array<float, 3> bt709 = {decode_half(half[0]), decode_half(half[1]), decode_half(half[2])};
            for (int channel = 0; channel < 3; ++channel) {
              const auto& row_weights = BT709_TO_BT2020[channel];
              rgb[channel] = 80.f * ((row_weights[0] * bt709[0]) + (row_weights[1] * bt709[1]) + (row_weights[2] * bt709[2]));
            }
            break;
          }
          case reshade::api::format::r10g10b10a2_unorm:
          case reshade::api::format::b10g10r10a2_unorm: {
            uint32_t packed = 0;
            std::memcpy(&packed, row + (x * 4u), sizeof(packed));
            const bool is_bgr = (region_format == reshade::api::format::b10g10r10a2_unorm);
            for (int channel = 0; channel < 3; ++channel) {
              const auto shift = 10 * (is_bgr ? (2 - channel) : channel);
              const auto encoded = static_cast<float>((packed >> shift) & 0x3FFu) / 1023.f;
              // SMPTE ST 2084 (PQ) EOTF.
              const auto p = std::pow(encoded, 1.f / 78.84375f);
              rgb[channel] = 10000.f * std::pow(std::max(p - 0.8359375f, 0.f) / (18.8515625f - (18.6875f * p)), 1.f / 0.1593017578125f);
            }
            break;
          }
          default: {
            const auto* bytes = row + (x * 4u);
            const bool is_bgr = (region_format == reshade::api::format::b8g8r8a8_unorm || region_format == reshade::api::format::b8g8r8x8_unorm);
            for (int channel = 0; channel < 3; ++channel) {
              const auto encoded = bytes[is_bgr ? (2 - channel) : channel] / 255.f;
              rgb[channel] = 100.f * std::pow(encoded, 2.2f);
            }
            break;
          }
        }
        const auto& weights = (is_sdr ? BT709_LUMINANCE : BT2020_LUMINANCE);
        const std::array<double, 4> pixel = {
            rgb[0],
            rgb[1],
            rgb[2],
            (weights[0] * rgb[0]) + (weights[1] * rgb[1]) + (weights[2] * rgb[2]),
        };
        for (int channel = 0; channel < 4; ++channel) {
          channel_max[channel] = std::max(channel_max[channel], pixel[channel]);
          channel_min[channel] = std::min(channel_min[channel], pixel[channel]);
          channel_sum[channel] += pixel[channel];
        }
      }
    }
    device->unmap_texture_region(region_readback.staging, 0);

    const auto pixel_count = static_cast<double>(box.width) * box.height;
    json values = json::object();
    values["max_luminance"] = channel_max[3];
    values["avg_luminance"] = channel_sum[3] / pixel_count;
    values["min_luminance"] = channel_min[3];
    constexpr std::array<const char*, 3> CHANNEL_NAMES = {"r", "g", "b"};
    for (int channel = 0; channel < 3; ++channel) {
      values[std::format("max_{}", CHANNEL_NAMES[channel])] = channel_max[channel];
      values[std::format("avg_{}", CHANNEL_NAMES[channel])] = channel_sum[channel] / pixel_count;
      values[std::format("min_{}", CHANNEL_NAMES[channel])] = channel_min[channel];
    }
    values["max_cll"] = std::max({channel_max[0], channel_max[1], channel_max[2]});
    values["avg_cll"] = (channel_sum[0] + channel_sum[1] + channel_sum[2]) / (3.0 * pixel_count);
    values["min_cll"] = std::min({channel_min[0], channel_min[1], channel_min[2]});

    latest_sample["region"] = {
        {"anchor", (region_anchor == REGION_ANCHOR_DRAWN ? "drawn" : "screen_centre")},
        {"left", box.left},
        {"top", box.top},
        {"width", box.width},
        {"height", box.height},
        {"colour_space", region_colour_space},
        {"units", (is_sdr ? "percent" : "nits")},
        {"values", values},
    };
  }

  if (series.has_value()) {
    latest_sample["series_label"] = series->label;
    series->samples.push_back(latest_sample);
  }

  WriteJsonFile(GetOutputFolder() / "latest.json", latest_sample);

  if (write_continuous_log) {
    if (!session_log.is_open()) {
      std::filesystem::create_directories(GetOutputFolder());
      session_log.open(GetOutputFolder() / std::format("session-{}.jsonl", GetFileTimestamp()), std::ios::app);
    }
    session_log << latest_sample.dump() << '\n';
    session_log.flush();
  }

  if (!series.has_value()) {
    status = (layout != nullptr ? std::format("Sampling ({}, {}).", layout->colour_space, layout->units)
                                : std::format("Sampling region only. {}", analysis_status));
    return;
  }
  if (std::chrono::steady_clock::now() < series->end_time) return;

  json summary = json::object();
  const auto add_to_summary = [&summary](const json& values, std::string_view prefix) {
    for (const auto& item : values.items()) {
      if (!item.value().is_number()) continue;
      const auto number = item.value().get<double>();
      auto& field = summary[std::format("{}{}", prefix, item.key())];
      if (field.is_null()) {
        field = {{"min", number}, {"mean", 0.0}, {"max", number}, {"count", 0}};
      }
      field["min"] = std::min(field["min"].get<double>(), number);
      field["max"] = std::max(field["max"].get<double>(), number);
      field["mean"] = field["mean"].get<double>() + number;
      field["count"] = field["count"].get<int>() + 1;
    }
  };
  for (const auto& sample : series->samples) {
    if (sample.contains("values")) {
      add_to_summary(sample["values"], "");
    }
    if (sample.contains("region") && sample["region"].contains("values")) {
      add_to_summary(sample["region"]["values"], "region_");
    }
  }
  for (auto& field : summary) {
    field["mean"] = field["mean"].get<double>() / field["count"].get<double>();
  }

  std::string file_label = series->label;
  std::ranges::replace_if(
      file_label,
      [](char c) { return std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '-' && c != '_'; },
      '_');
  const auto file_name = std::format("series-{}-{}.json", GetFileTimestamp(), file_label);
  WriteJsonFile(
      GetOutputFolder() / file_name,
      json{
          {"label", series->label},
          {"duration_seconds", series_duration_seconds},
          {"sample_count", series->samples.size()},
          {"colour_space", latest_sample.value("colour_space", "")},
          {"units", latest_sample.value("units", "")},
          {"region_units", (latest_sample.contains("region") ? latest_sample["region"].value("units", "") : "")},
          {"summary", summary},
          {"samples", series->samples},
      });
  status = std::format("Wrote {} ({} samples).", file_name, series->samples.size());
  completion_message = std::format("Logging complete: {} ({} samples)", file_name, series->samples.size());
  completion_message_end_time = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  reshade::log::message(reshade::log::level::info, std::format("HDRAnalysisLogger: {}", status).c_str());
  series.reset();
}

// Runs every frame (not only while the ReShade overlay is open), so the region
// outline stays visible during play. ReShade draws it after effects, so it is
// never part of a measurement.
void OnReShadeOverlay(reshade::api::effect_runtime* runtime) {
  if (back_buffer_width == 0 || back_buffer_height == 0) return;
  auto& io = ImGui::GetIO();
  auto* draw_list = ImGui::GetForegroundDrawList(nullptr);
  const auto draw_outline = [draw_list](ImVec2 top_left, ImVec2 bottom_right, ImU32 colour) {
    draw_list->AddRect({top_left.x - 1.f, top_left.y - 1.f}, {bottom_right.x + 1.f, bottom_right.y + 1.f}, IM_COL32(0, 0, 0, 255));
    draw_list->AddRect(top_left, bottom_right, colour);
  };

  // On-screen banner, so recording progress and completion are visible with
  // the ReShade overlay closed. Drawn after effects, so it is never measured.
  std::string banner;
  if (drawing_region && move_grab_offset.has_value()) {
    banner = "Draw region: release to place the box (Esc cancels)";
  } else if (drawing_region && drag_start.has_value()) {
    banner = "Draw region: release, or click the opposite corner (Esc cancels)";
  } else if (drawing_region) {
    banner = "Draw region: drag the cyan box to move it, or click one corner of a new box (Esc cancels)";
  } else if (series.has_value()) {
    const auto remaining = std::chrono::duration<float>(series->end_time - std::chrono::steady_clock::now()).count();
    banner = std::format("Recording \"{}\"... {:.1f} s", series->label, std::max(remaining, 0.f));
  } else if (std::chrono::steady_clock::now() < completion_message_end_time) {
    banner = completion_message;
  }
  if (!banner.empty()) {
    const auto text_size = ImGui::CalcTextSize(banner.c_str());
    const ImVec2 text_position = {(io.DisplaySize.x - text_size.x) * 0.5f, io.DisplaySize.y * 0.05f};
    draw_list->AddRectFilled(
        {text_position.x - 8.f, text_position.y - 6.f},
        {text_position.x + text_size.x + 8.f, text_position.y + text_size.y + 6.f},
        IM_COL32(0, 0, 0, 200),
        4.f);
    draw_list->AddText(text_position, IM_COL32(255, 255, 255, 255), banner.c_str());
  }

  const auto box = ComputeRegionBox(back_buffer_width, back_buffer_height);
  const auto scale_x = io.DisplaySize.x / back_buffer_width;
  const auto scale_y = io.DisplaySize.y / back_buffer_height;
  const ImVec2 box_top_left = {box.left * scale_x, box.top * scale_y};
  const ImVec2 box_size = {box.width * scale_x, box.height * scale_y};

  if (drawing_region) {
    // Lets the region be drawn with the ReShade overlay closed: the game stops
    // receiving mouse and keyboard input (no camera movement, Esc doesn't reach
    // it), can't re-centre the cursor, and ImGui draws a cursor.
    runtime->block_input_next_frame();
    ImGui::SetNextFrameWantCaptureMouse(true);
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    io.MouseDrawCursor = true;

    const auto commit_rect = [&](ImVec2 top_left, ImVec2 bottom_right) {
      region_rect = {
          std::clamp(top_left.x / io.DisplaySize.x, 0.f, 1.f),
          std::clamp(top_left.y / io.DisplaySize.y, 0.f, 1.f),
          std::clamp(bottom_right.x / io.DisplaySize.x, 0.f, 1.f),
          std::clamp(bottom_right.y / io.DisplaySize.y, 0.f, 1.f),
      };
      region_anchor = REGION_ANCHOR_DRAWN;
      drawing_region = false;
      drag_start.reset();
      move_grab_offset.reset();
      SaveSettings(runtime);
    };

    const auto mouse = ImGui::GetMousePos();
    if (runtime->is_key_pressed(VK_ESCAPE)) {
      drawing_region = false;
      drag_start.reset();
      move_grab_offset.reset();
    } else if (move_grab_offset.has_value()) {
      // Moving keeps the box size and keeps it on screen.
      const ImVec2 moved_top_left = {
          std::clamp(mouse.x - move_grab_offset->x, 0.f, std::max(io.DisplaySize.x - box_size.x, 0.f)),
          std::clamp(mouse.y - move_grab_offset->y, 0.f, std::max(io.DisplaySize.y - box_size.y, 0.f)),
      };
      const ImVec2 moved_bottom_right = {moved_top_left.x + box_size.x, moved_top_left.y + box_size.y};
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        draw_outline(moved_top_left, moved_bottom_right, IM_COL32(255, 255, 0, 255));
        return;
      }
      commit_rect(moved_top_left, moved_bottom_right);
    } else if (!drag_start.has_value()) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        const bool inside_box = mouse.x >= box_top_left.x && mouse.x <= box_top_left.x + box_size.x
                                && mouse.y >= box_top_left.y && mouse.y <= box_top_left.y + box_size.y;
        if (inside_box) {
          move_grab_offset = ImVec2(mouse.x - box_top_left.x, mouse.y - box_top_left.y);
        } else {
          drag_start = mouse;
        }
      }
    } else {
      // Two ways to finish a new box: release after dragging (press, drag,
      // release), or a second click (click one corner, move, click the
      // opposite corner).
      const bool dragged = std::abs(mouse.x - drag_start->x) >= 3.f && std::abs(mouse.y - drag_start->y) >= 3.f;
      const bool finished = (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && dragged)
                            || ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
      const ImVec2 top_left = {std::min(drag_start->x, mouse.x), std::min(drag_start->y, mouse.y)};
      const ImVec2 bottom_right = {std::max(drag_start->x, mouse.x), std::max(drag_start->y, mouse.y)};
      if (!finished) {
        draw_outline(top_left, bottom_right, IM_COL32(255, 255, 0, 255));
        return;
      }
      commit_rect(top_left, bottom_right);
    }
  }

  if (!show_region_outline && !drawing_region) return;
  // Recomputed because a move or a new box may have been committed above.
  const auto current_box = ComputeRegionBox(back_buffer_width, back_buffer_height);
  draw_outline(
      {current_box.left * scale_x, current_box.top * scale_y},
      {(current_box.left + current_box.width) * scale_x, (current_box.top + current_box.height) * scale_y},
      IM_COL32(0, 255, 255, 255));
}

void OnRegisterOverlay(reshade::api::effect_runtime* runtime) {
  ImGui::TextWrapped("%s", status.c_str());
  if (latest_sample.contains("values")) {
    const auto& values = latest_sample["values"];
    ImGui::Text(
        "Frame:  max %.3f  avg %.3f  min %.6f %s",
        values["max_luminance"].get<double>(),
        values["avg_luminance"].get<double>(),
        values["min_luminance"].get<double>(),
        latest_sample["units"].get<std::string>().c_str());
  }
  if (latest_sample.contains("region") && latest_sample["region"].contains("values")) {
    const auto& region = latest_sample["region"];
    const auto& values = region["values"];
    ImGui::Text(
        "Region: max %.3f  avg %.3f  min %.6f %s",
        values["max_luminance"].get<double>(),
        values["avg_luminance"].get<double>(),
        values["min_luminance"].get<double>(),
        region["units"].get<std::string>().c_str());
  }
  ImGui::Separator();

  bool settings_changed = false;
  settings_changed |= ImGui::SliderInt("Sample interval (ms)", &sample_interval_ms, 100, 5000);
  settings_changed |= ImGui::Checkbox("Write continuous session log", &write_continuous_log);
  settings_changed |= ImGui::SliderInt("Series duration (s)", &series_duration_seconds, 1, 60);
  ImGui::InputText("Series label", series_label.data(), series_label.size());

  ImGui::BeginDisabled(series.has_value());
  if (ImGui::Button(std::format("Record series ({})", FormatHotkey(series_hotkey)).c_str())) {
    StartSeries();
  }
  ImGui::EndDisabled();

  if (capturing_hotkey) {
    ImGui::Text("Press the new key combination (Esc cancels)...");
    for (uint32_t key = 0x08; key <= 0xFE; ++key) {
      const bool is_modifier = (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || (key >= VK_LSHIFT && key <= VK_RMENU));
      if (is_modifier || !runtime->is_key_pressed(key)) continue;
      if (key != VK_ESCAPE) {
        series_hotkey = {
            .key = key,
            .ctrl = runtime->is_key_down(VK_CONTROL),
            .shift = runtime->is_key_down(VK_SHIFT),
            .alt = runtime->is_key_down(VK_MENU),
        };
        settings_changed = true;
      }
      capturing_hotkey = false;
      break;
    }
  } else if (ImGui::Button("Change hotkey")) {
    capturing_hotkey = true;
  }

  ImGui::Separator();
  ImGui::Text("Measurement region");
  settings_changed |= ImGui::Combo("Position", &region_anchor, "Screen centre\0Drawn rectangle\0", -1);
  settings_changed |= ImGui::SliderInt("Centre square size (px)", &region_size, 1, 256);
  settings_changed |= ImGui::Checkbox("Show outline", &show_region_outline);
  if (drawing_region) {
    ImGui::TextWrapped("Drag the cyan box to move it, or click one corner of the light source and then the "
                       "opposite corner to draw a new one (press, drag and release also works). Esc cancels. "
                       "You can close the ReShade overlay first; game input stays blocked until you finish.");
  } else if (ImGui::Button("Draw region")) {
    drawing_region = true;
  }

  if (settings_changed) {
    SaveSettings(runtime);
  }

  ImGui::Separator();
  ImGui::TextWrapped("Output: %s", GetOutputFolder().string().c_str());
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX HDR Analysis Logger";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION =
    "Logs the statistics of Lilium's HDR/SDR analysis effect and a measurement region to JSON files.";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
      reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
      reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnReShadeBeginEffects);
      reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnReShadeFinishEffects);
      reshade::register_event<reshade::addon_event::reshade_overlay>(OnReShadeOverlay);
      reshade::register_overlay("HDR Analysis Logger", OnRegisterOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay("HDR Analysis Logger", OnRegisterOverlay);
      reshade::unregister_event<reshade::addon_event::reshade_overlay>(OnReShadeOverlay);
      reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(OnReShadeFinishEffects);
      reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(OnReShadeBeginEffects);
      reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
      reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
      reshade::unregister_addon(h_module);
      break;
  }

  return TRUE;
}
