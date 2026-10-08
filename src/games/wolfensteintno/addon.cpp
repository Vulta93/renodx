/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/bitwise.hpp"
#include "../../utils/data.hpp"
#include "../../utils/detour.hpp"
#include "../../utils/device_proxy.hpp"
#include "../../utils/hash.hpp"
#include "../../utils/resource_upgrade.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

renodx::mods::shader::CustomShaders custom_shaders = {
    CustomShaderEntry(0xA85A9FE0),  // main post-process + colour LUT (GLSL)
    CustomShaderEntry(0x97DA5C21),  // box-projected damage decal: UNORM-equivalent clamp (negative edge fade)
    CustomShaderEntry(0x0686B625),  // its vertex shader, only to pin the attribute locations in the replacement program
    // CustomShaderEntry(0x00000000),
    // CustomSwapchainShader(0x00000000),
    // BypassShaderEntry(0x00000000),
    // __ALL_CUSTOM_SHADERS
};

ShaderInjectData shader_injection;

float current_settings_mode = 0;

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "SettingsMode",
        .binding = &current_settings_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Settings Mode",
        .labels = {"Simple", "Intermediate", "Advanced"},
        .is_global = true,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Sets the tone mapper type",
        // 0 Vanilla (clipped like the game's 8-bit buffers), 1 None (untonemapped, clamped at Peak),
        // 2 PsychoV-17 (0xA85A9FE0.frag.glsl).
        .labels = {"Vanilla", "None", "PsychoV"},
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &shader_injection.peak_white_nits,
        .default_value = 1000.f,
        .can_reset = false,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits",
        .min = 48.f,
        .max = 4000.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapUINits",
        .binding = &shader_injection.graphics_white_nits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "GammaCorrection",
        .binding = &shader_injection.gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Gamma Correction",
        .section = "Tone Mapping",
        .tooltip = "Emulates a display EOTF.",
        .labels = {"Off", "2.2", "BT.1886"},
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapScaling",
        .binding = &shader_injection.tone_map_per_channel,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Scaling",
        .section = "Tone Mapping",
        .tooltip = "Luminance scales colors consistently while per-channel saturates and blows out sooner",
        .labels = {"Luminance", "Per Channel"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapWorkingColorSpace",
        .binding = &shader_injection.tone_map_working_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Working Color Space",
        .section = "Tone Mapping",
        .labels = {"BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueProcessor",
        .binding = &shader_injection.tone_map_hue_processor,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Hue Processor",
        .section = "Tone Mapping",
        .tooltip = "Selects hue processor",
        .labels = {"OKLab", "ICtCp", "darkTable UCS"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueCorrection",
        .binding = &shader_injection.tone_map_hue_correction,
        .default_value = 100.f,
        .label = "Hue Correction",
        .section = "Tone Mapping",
        .tooltip = "Hue retention strength.",
        .min = 0.f,
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueShift",
        .binding = &shader_injection.tone_map_hue_shift,
        .default_value = 50.f,
        .label = "Hue Shift",
        .section = "Tone Mapping",
        .tooltip = "Hue-shift emulation strength.",
        .min = 0.f,
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapClampColorSpace",
        .binding = &shader_injection.tone_map_clamp_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Clamp Color Space",
        .section = "Tone Mapping",
        .tooltip = "Hue-shift emulation strength.",
        .labels = {"None", "BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapClampPeak",
        .binding = &shader_injection.tone_map_clamp_peak,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Clamp Peak",
        .section = "Tone Mapping",
        .tooltip = "Hue-shift emulation strength.",
        .labels = {"None", "BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlightSaturation",
        .binding = &shader_injection.tone_map_highlight_saturation,
        .default_value = 50.f,
        .label = "Highlight Saturation",
        .section = "Color Grading",
        .tooltip = "Adds or removes highlight color.",
        .max = 100.f,
        // Not wired: PsychoV-17 has no highlight-saturation input (Blowout drives its bleaching).
        .is_enabled = []() { return false; },
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeBlowout",
        .binding = &shader_injection.tone_map_blowout,
        .default_value = 0.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeFlare",
        .binding = &shader_injection.tone_map_flare,
        .default_value = 0.f,
        .label = "Flare",
        .section = "Color Grading",
        .tooltip = "Flare/Glare Compensation",
        .max = 100.f,
        // Not wired: PsychoV-17 has no flare input.
        .is_enabled = []() { return false; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeScene",
        .binding = &shader_injection.color_grade_strength,
        .default_value = 100.f,
        .label = "Scene Grading",
        .section = "Color Grading",
        .tooltip = "Scene grading as applied by the game",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainCustomColorSpace",
        .binding = &shader_injection.swap_chain_custom_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Custom Color Space",
        .section = "Display Output",
        .tooltip = "Selects output color space"
                   "\nUS Modern for BT.709 D65."
                   "\nJPN Modern for BT.709 D93."
                   "\nUS CRT for BT.601 (NTSC-U)."
                   "\nJPN CRT for BT.601 ARIB-TR-B9 D93 (NTSC-J)."
                   "\nDefault: US CRT",
        .labels = {
            "US Modern",
            "JPN Modern",
            "US CRT",
            "JPN CRT",
        },
        .is_visible = []() { return settings[0]->GetValue() >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "IntermediateDecoding",
        .binding = &shader_injection.intermediate_encoding,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Intermediate Encoding",
        .section = "Display Output",
        .labels = {"Auto", "None", "SRGB", "2.2", "2.4"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) {
            if (value == 0) return shader_injection.gamma_correction + 1.f;
            return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainDecoding",
        .binding = &shader_injection.swap_chain_decoding,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Swapchain Decoding",
        .section = "Display Output",
        .labels = {"Auto", "None", "SRGB", "2.2", "2.4"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) {
            if (value == 0) return shader_injection.intermediate_encoding;
            return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainGammaCorrection",
        .binding = &shader_injection.swap_chain_gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Gamma Correction",
        .section = "Display Output",
        .labels = {"None", "2.2", "2.4"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainClampColorSpace",
        .binding = &shader_injection.swap_chain_clamp_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .label = "Clamp Color Space",
        .section = "Display Output",
        .labels = {"None", "BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
};

const std::unordered_map<std::string, reshade::api::format> UPGRADE_TARGETS = {
    {"R8G8B8A8_TYPELESS", reshade::api::format::r8g8b8a8_typeless},
    {"B8G8R8A8_TYPELESS", reshade::api::format::b8g8r8a8_typeless},
    {"R8G8B8A8_UNORM", reshade::api::format::r8g8b8a8_unorm},
    {"B8G8R8A8_UNORM", reshade::api::format::b8g8r8a8_unorm},
    {"R8G8B8A8_SNORM", reshade::api::format::r8g8b8a8_snorm},
    {"R8G8B8A8_UNORM_SRGB", reshade::api::format::r8g8b8a8_unorm_srgb},
    {"B8G8R8A8_UNORM_SRGB", reshade::api::format::b8g8r8a8_unorm_srgb},
    {"R10G10B10A2_TYPELESS", reshade::api::format::r10g10b10a2_typeless},
    {"R10G10B10A2_UNORM", reshade::api::format::r10g10b10a2_unorm},
    {"B10G10R10A2_UNORM", reshade::api::format::b10g10r10a2_unorm},
    {"R11G11B10_FLOAT", reshade::api::format::r11g11b10_float},
    {"R16G16B16A16_TYPELESS", reshade::api::format::r16g16b16a16_typeless},
};

void OnPresetOff() {
  //   renodx::utils::settings::UpdateSetting("toneMapType", 0.f);
  //   renodx::utils::settings::UpdateSetting("toneMapPeakNits", 203.f);
  //   renodx::utils::settings::UpdateSetting("toneMapGameNits", 203.f);
  //   renodx::utils::settings::UpdateSetting("toneMapUINits", 203.f);
  //   renodx::utils::settings::UpdateSetting("toneMapGammaCorrection", 0);
  //   renodx::utils::settings::UpdateSetting("colorGradeExposure", 1.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeHighlights", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeShadows", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeContrast", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeSaturation", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeLUTStrength", 100.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeLUTScaling", 0.f);
}

const auto UPGRADE_TYPE_NONE = 0.f;
const auto UPGRADE_TYPE_OUTPUT_SIZE = 1.f;
const auto UPGRADE_TYPE_OUTPUT_RATIO = 2.f;
const auto UPGRADE_TYPE_ANY = 3.f;

void OnPresent(reshade::api::command_queue* queue,
               reshade::api::swapchain* swapchain,
               const reshade::api::rect* source_rect,
               const reshade::api::rect* dest_rect,
               uint32_t dirty_rect_count,
               const reshade::api::rect* dirty_rects) {
  auto* device = queue->get_device();
  if (device->get_api() == reshade::api::device_api::opengl) {
    shader_injection.custom_flip_uv_y = 1.f;
  }
}

// RENODX DEBUG (temporary): one-frame trace of render-target binds, copies and resolves. Hold F9 in game.
namespace trace {
bool active = false;
int draws = 0;
int cooldown = 0;
uint64_t back_buffer_handle = 0;
// Draw runs are grouped by pixel shader (set by gl_probe::OnBindPipeline) and list the screen-sized textures they read.
uint32_t current_pixel_shader = 0;
uint32_t run_pixel_shader = 0;
std::string run_inputs;
reshade::api::resource_view bound_textures[16] = {};

void Log(const std::string& message) {
  reshade::log::message(reshade::log::level::info, ("WOLFTRACE " + message).c_str());
}

std::string Describe(reshade::api::device* device, reshade::api::resource resource) {
  std::stringstream s;
  s << std::hex << resource.handle << std::dec;
  if (resource.handle == back_buffer_handle) s << "(BACKBUFFER)";
  const auto desc = device->get_resource_desc(resource);
  s << "[" << desc.texture.width << "x" << desc.texture.height << " " << desc.texture.format
    << " samples=" << desc.texture.samples << "]";
  return s.str();
}

void FlushDraws() {
  if (draws > 0) {
    std::stringstream s;
    s << "  ... " << draws << " draws ps 0x" << std::hex << std::uppercase << run_pixel_shader << std::dec << run_inputs;
    Log(s.str());
  }
  draws = 0;
}

bool IsActive(reshade::api::command_list* cmd_list) {
  return active && cmd_list->get_device()->get_api() == reshade::api::device_api::opengl;
}

void OnPresent(reshade::api::command_queue*, reshade::api::swapchain* swapchain, const reshade::api::rect*,
               const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  auto* device = swapchain->get_device();
  if (device->get_api() != reshade::api::device_api::opengl) return;
  if (active) {
    FlushDraws();
    Log("END FRAME");
    active = false;
    cooldown = 120;
    return;
  }
  if (cooldown > 0) {
    --cooldown;
    return;
  }
  if ((GetAsyncKeyState(VK_F9) & 0x8000) != 0) {
    active = true;
    draws = 0;
    back_buffer_handle = swapchain->get_current_back_buffer().handle;
    Log("BEGIN FRAME back buffer=" + Describe(device, swapchain->get_current_back_buffer()));
  }
}

void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count, const reshade::api::resource_view* rtvs,
                         reshade::api::resource_view) {
  if (!IsActive(cmd_list)) return;
  FlushDraws();
  auto* device = cmd_list->get_device();
  std::string message = "BIND RT x" + std::to_string(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (rtvs[i].handle == 0u) continue;
    message += " rt" + std::to_string(i) + "=" + Describe(device, device->get_resource_from_view(rtvs[i]));
  }
  Log(message);
}

void OnPushDescriptors(reshade::api::command_list* cmd_list, reshade::api::shader_stage, reshade::api::pipeline_layout,
                       uint32_t layout_param, const reshade::api::descriptor_table_update& update) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::opengl) return;
  if (layout_param != 0 || update.type != reshade::api::descriptor_type::sampler_with_resource_view) return;
  const auto* descriptors = static_cast<const reshade::api::sampler_with_resource_view*>(update.descriptors);
  for (uint32_t i = 0; i < update.count; ++i) {
    const uint32_t slot = update.binding + i;
    if (slot < 16) bound_textures[slot] = descriptors[i].view;
  }
}

void CountDraw(reshade::api::command_list* cmd_list) {
  if (!IsActive(cmd_list)) return;
  if (draws > 0 && current_pixel_shader != run_pixel_shader) FlushDraws();
  if (draws == 0) {
    run_pixel_shader = current_pixel_shader;
    run_inputs.clear();
    auto* device = cmd_list->get_device();
    for (uint32_t slot = 0; slot < 16; ++slot) {
      if (bound_textures[slot].handle == 0u) continue;
      const auto resource = device->get_resource_from_view(bound_textures[slot]);
      if (resource.handle == 0u) continue;
      if (device->get_resource_desc(resource).texture.width < 400) continue;
      run_inputs += " t" + std::to_string(slot) + "=" + Describe(device, resource);
    }
  }
  ++draws;
}

bool OnDraw(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, uint32_t) {
  CountDraw(cmd_list);
  return false;
}

bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  CountDraw(cmd_list);
  return false;
}

bool OnCopyResource(reshade::api::command_list* cmd_list, reshade::api::resource source, reshade::api::resource dest) {
  if (!IsActive(cmd_list)) return false;
  FlushDraws();
  auto* device = cmd_list->get_device();
  Log("COPY_RESOURCE " + Describe(device, source) + " -> " + Describe(device, dest));
  return false;
}

bool OnCopyTextureRegion(reshade::api::command_list* cmd_list, reshade::api::resource source, uint32_t,
                         const reshade::api::subresource_box*, reshade::api::resource dest, uint32_t,
                         const reshade::api::subresource_box*, reshade::api::filter_mode) {
  if (!IsActive(cmd_list)) return false;
  FlushDraws();
  auto* device = cmd_list->get_device();
  Log("COPY_TEXTURE_REGION " + Describe(device, source) + " -> " + Describe(device, dest));
  return false;
}

bool OnResolveTextureRegion(reshade::api::command_list* cmd_list, reshade::api::resource source, uint32_t,
                            const reshade::api::subresource_box*, reshade::api::resource dest, uint32_t, uint32_t,
                            uint32_t, uint32_t, reshade::api::format) {
  if (!IsActive(cmd_list)) return false;
  FlushDraws();
  auto* device = cmd_list->get_device();
  Log("RESOLVE " + Describe(device, source) + " -> " + Describe(device, dest));
  return false;
}

bool OnClearRenderTargetView(reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, const float*,
                             uint32_t, const reshade::api::rect*) {
  if (!IsActive(cmd_list)) return false;
  FlushDraws();
  auto* device = cmd_list->get_device();
  Log("CLEAR RTV " + Describe(device, device->get_resource_from_view(rtv)));
  return false;
}

void Register(bool attach) {
  if (attach) {
    reshade::register_event<reshade::addon_event::present>(OnPresent);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
    reshade::register_event<reshade::addon_event::draw>(OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::register_event<reshade::addon_event::copy_resource>(OnCopyResource);
    reshade::register_event<reshade::addon_event::copy_texture_region>(OnCopyTextureRegion);
    reshade::register_event<reshade::addon_event::resolve_texture_region>(OnResolveTextureRegion);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(OnClearRenderTargetView);
    reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
  } else {
    reshade::unregister_event<reshade::addon_event::present>(OnPresent);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
    reshade::unregister_event<reshade::addon_event::draw>(OnDraw);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::unregister_event<reshade::addon_event::copy_resource>(OnCopyResource);
    reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnCopyTextureRegion);
    reshade::unregister_event<reshade::addon_event::resolve_texture_region>(OnResolveTextureRegion);
    reshade::unregister_event<reshade::addon_event::clear_render_target_view>(OnClearRenderTargetView);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
  }
}
}  // namespace trace

// RENODX DEBUG (temporary): per-draw pixel probe. While the F9 trace frame is active, before every draw read back the
// centre pixel of the bound framebuffer (colour attachment 0) as float and log it whenever it changes, with the
// application program and pixel-shader hash of the draw that changed it. Finds which draw writes NaN / Inf / huge values.
namespace gl_probe {
using GLuint = unsigned int;
using GLint = int;
using GLenum = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using PFN_GetIntegerv = void(__stdcall*)(GLenum, GLint*);
using PFN_IsEnabled = GLboolean(__stdcall*)(GLenum);
using PFN_Enable = void(__stdcall*)(GLenum);
using PFN_GenFramebuffers = void(__stdcall*)(GLsizei, GLuint*);
using PFN_GenTextures = void(__stdcall*)(GLsizei, GLuint*);
using PFN_BindTexture = void(__stdcall*)(GLenum, GLuint);
using PFN_TexStorage2D = void(__stdcall*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using PFN_BindFramebuffer = void(__stdcall*)(GLenum, GLuint);
using PFN_FramebufferTexture2D = void(__stdcall*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_ReadBuffer = void(__stdcall*)(GLenum);
using PFN_BlitFramebuffer = void(__stdcall*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using PFN_ReadPixels = void(__stdcall*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using PFN_BindBuffer = void(__stdcall*)(GLenum, GLuint);
using PFN_GetProgramiv = void(__stdcall*)(GLuint, GLenum, GLint*);
using PFN_GetActiveAttrib = void(__stdcall*)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*);
using PFN_GetAttribLocation = GLint(__stdcall*)(GLuint, const char*);
using PFN_GetFragDataLocation = GLint(__stdcall*)(GLuint, const char*);

struct Functions {
  bool loaded = false;
  bool ok = false;
  PFN_GetIntegerv get_integerv;
  PFN_IsEnabled is_enabled;
  PFN_Enable enable;
  PFN_Enable disable;
  PFN_GenFramebuffers gen_framebuffers;
  PFN_GenTextures gen_textures;
  PFN_BindTexture bind_texture;
  PFN_TexStorage2D tex_storage_2d;
  PFN_BindFramebuffer bind_framebuffer;
  PFN_FramebufferTexture2D framebuffer_texture_2d;
  PFN_ReadBuffer read_buffer;
  PFN_BlitFramebuffer blit_framebuffer;
  PFN_ReadPixels read_pixels;
  PFN_BindBuffer bind_buffer;
  PFN_GetProgramiv get_programiv;
  PFN_GetActiveAttrib get_active_attrib;
  PFN_GetAttribLocation get_attrib_location;
  PFN_GetFragDataLocation get_frag_data_location;
} gl = {};

template <typename T>
T Load(const char* name) {
  HMODULE module = GetModuleHandleW(L"opengl32.dll");
  if (module == nullptr) return nullptr;
  using PFN_wglGetProcAddress = PROC(__stdcall*)(LPCSTR);
  auto* wgl_get_proc_address = reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(module, "wglGetProcAddress"));
  PROC proc = wgl_get_proc_address != nullptr ? wgl_get_proc_address(name) : nullptr;
  if (proc == nullptr) proc = GetProcAddress(module, name);
  return reinterpret_cast<T>(proc);
}

bool LoadFunctions() {
  if (gl.loaded) return gl.ok;
  gl.loaded = true;
  gl.get_integerv = Load<PFN_GetIntegerv>("glGetIntegerv");
  gl.is_enabled = Load<PFN_IsEnabled>("glIsEnabled");
  gl.enable = Load<PFN_Enable>("glEnable");
  gl.disable = Load<PFN_Enable>("glDisable");
  gl.gen_framebuffers = Load<PFN_GenFramebuffers>("glGenFramebuffers");
  gl.gen_textures = Load<PFN_GenTextures>("glGenTextures");
  gl.bind_texture = Load<PFN_BindTexture>("glBindTexture");
  gl.tex_storage_2d = Load<PFN_TexStorage2D>("glTexStorage2D");
  gl.bind_framebuffer = Load<PFN_BindFramebuffer>("glBindFramebuffer");
  gl.framebuffer_texture_2d = Load<PFN_FramebufferTexture2D>("glFramebufferTexture2D");
  gl.read_buffer = Load<PFN_ReadBuffer>("glReadBuffer");
  gl.blit_framebuffer = Load<PFN_BlitFramebuffer>("glBlitFramebuffer");
  gl.read_pixels = Load<PFN_ReadPixels>("glReadPixels");
  gl.bind_buffer = Load<PFN_BindBuffer>("glBindBuffer");
  gl.get_programiv = Load<PFN_GetProgramiv>("glGetProgramiv");
  gl.get_active_attrib = Load<PFN_GetActiveAttrib>("glGetActiveAttrib");
  gl.get_attrib_location = Load<PFN_GetAttribLocation>("glGetAttribLocation");
  gl.get_frag_data_location = Load<PFN_GetFragDataLocation>("glGetFragDataLocation");
  gl.ok = gl.get_integerv && gl.is_enabled && gl.enable && gl.disable && gl.gen_framebuffers && gl.gen_textures
          && gl.bind_texture && gl.tex_storage_2d && gl.bind_framebuffer && gl.framebuffer_texture_2d
          && gl.read_buffer && gl.blit_framebuffer && gl.read_pixels && gl.bind_buffer && gl.get_programiv
          && gl.get_active_attrib && gl.get_attrib_location && gl.get_frag_data_location;
  reshade::log::message(gl.ok ? reshade::log::level::info : reshade::log::level::error,
                        gl.ok ? "WOLFPX functions loaded" : "WOLFPX failed to load GL functions");
  return gl.ok;
}

constexpr uint64_t kGlProgramHandleType = 0x82E2;
std::unordered_map<GLuint, uint32_t> pixel_shader_hash_by_program;
GLuint current_program = 0;
GLuint probe_framebuffer = 0;
GLuint probe_texture = 0;

int draw_index = 0;
GLuint previous_program = 0;
GLuint previous_framebuffer = 0;
float previous_value[4] = {-12345.f, 0.f, 0.f, 0.f};

void OnInitPipeline(reshade::api::device* device, reshade::api::pipeline_layout, uint32_t subobject_count,
                    const reshade::api::pipeline_subobject* subobjects, reshade::api::pipeline pipeline) {
  if (device->get_api() != reshade::api::device_api::opengl) return;
  if ((pipeline.handle >> 40) != kGlProgramHandleType) return;
  const auto program = static_cast<GLuint>(pipeline.handle & 0xFFFFFFFFu);
  uint32_t vertex_hash = 0;
  uint32_t pixel_hash = 0;
  for (uint32_t i = 0; i < subobject_count; ++i) {
    const auto* desc = static_cast<const reshade::api::shader_desc*>(subobjects[i].data);
    if (desc == nullptr || desc->code == nullptr || desc->code_size == 0) continue;
    const uint32_t hash = renodx::utils::hash::ComputeCRC32(static_cast<const uint8_t*>(desc->code), desc->code_size);
    if (subobjects[i].type == reshade::api::pipeline_subobject_type::pixel_shader) pixel_hash = hash;
    if (subobjects[i].type == reshade::api::pipeline_subobject_type::vertex_shader) vertex_hash = hash;
  }
  if (pixel_hash != 0) pixel_shader_hash_by_program[program] = pixel_hash;

  // Log the vertex-attribute and fragment-output locations the game gave this program (bound before link). A
  // replacement program linked by ReShade does not get them, so replaced GLSL must pin them with layout(location).
  if (!LoadFunctions()) return;
  std::stringstream s;
  s << "WOLFLOC program " << program << " vs 0x" << std::hex << std::uppercase << vertex_hash << " ps 0x" << pixel_hash
    << std::dec << " attribs:";
  GLint count = 0;
  gl.get_programiv(program, 0x8B89 /* GL_ACTIVE_ATTRIBUTES */, &count);
  for (GLint i = 0; i < count; ++i) {
    char name[128] = {};
    GLsizei length = 0;
    GLint size = 0;
    GLenum type = 0;
    gl.get_active_attrib(program, static_cast<GLuint>(i), sizeof(name), &length, &size, &type, name);
    s << " " << name << "=" << gl.get_attrib_location(program, name);
  }
  s << " outputs:";
  for (int i = 0; i < 4; ++i) {
    const std::string name = "out_FragColor" + std::to_string(i);
    const GLint location = gl.get_frag_data_location(program, name.c_str());
    if (location >= 0) s << " " << name << "=" << location;
  }
  reshade::log::message(reshade::log::level::info, s.str().c_str());
}

void OnBindPipeline(reshade::api::command_list* cmd_list, reshade::api::pipeline_stage,
                    reshade::api::pipeline pipeline) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::opengl) return;
  if ((pipeline.handle >> 40) != kGlProgramHandleType) return;
  current_program = static_cast<GLuint>(pipeline.handle & 0xFFFFFFFFu);
  const auto hash_it = pixel_shader_hash_by_program.find(current_program);
  trace::current_pixel_shader = (hash_it != pixel_shader_hash_by_program.end()) ? hash_it->second : 0u;
}

bool ReadCentre(GLuint framebuffer, float out[4]) {
  if (probe_framebuffer == 0) {
    GLint previous_texture = 0;
    gl.get_integerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &previous_texture);
    gl.gen_textures(1, &probe_texture);
    gl.bind_texture(0x0DE1, probe_texture);
    // Must match the upgraded scene target (RGBA16F): a resolve blit from a multisampled source requires the same format.
    gl.tex_storage_2d(0x0DE1, 1, 0x881A /* GL_RGBA16F */, 1, 1);
    gl.bind_texture(0x0DE1, static_cast<GLuint>(previous_texture));
    gl.gen_framebuffers(1, &probe_framebuffer);
  }
  GLint viewport[4] = {};
  gl.get_integerv(0x0BA2 /* GL_VIEWPORT */, viewport);
  const GLint x = viewport[0] + (viewport[2] / 2);
  const GLint y = viewport[1] + (viewport[3] / 2);

  GLint read_framebuffer = 0;
  GLint read_buffer = 0;
  GLint pack_buffer = 0;
  gl.get_integerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read_framebuffer);
  gl.get_integerv(0x0C02 /* GL_READ_BUFFER */, &read_buffer);
  gl.get_integerv(0x88ED /* GL_PIXEL_PACK_BUFFER_BINDING */, &pack_buffer);
  const bool scissor = gl.is_enabled(0x0C11 /* GL_SCISSOR_TEST */) != 0;

  if (scissor) gl.disable(0x0C11);
  gl.bind_framebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, framebuffer);
  gl.read_buffer(0x8CE0 /* GL_COLOR_ATTACHMENT0 */);
  gl.bind_framebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, probe_framebuffer);
  gl.framebuffer_texture_2d(0x8CA9, 0x8CE0, 0x0DE1, probe_texture, 0);
  gl.blit_framebuffer(x, y, x + 1, y + 1, 0, 0, 1, 1, 0x4000 /* GL_COLOR_BUFFER_BIT */, 0x2600 /* GL_NEAREST */);
  gl.bind_framebuffer(0x8CA8, probe_framebuffer);
  gl.read_buffer(0x8CE0);
  gl.bind_buffer(0x88EB /* GL_PIXEL_PACK_BUFFER */, 0);
  gl.read_pixels(0, 0, 1, 1, 0x1908 /* GL_RGBA */, 0x1406 /* GL_FLOAT */, out);

  gl.bind_buffer(0x88EB, static_cast<GLuint>(pack_buffer));
  gl.bind_framebuffer(0x8CA8, static_cast<GLuint>(read_framebuffer));
  gl.read_buffer(static_cast<GLenum>(read_buffer));
  gl.bind_framebuffer(0x8CA9, framebuffer);
  if (scissor) gl.enable(0x0C11);
  return true;
}

void BeforeDraw(reshade::api::command_list* cmd_list) {
  if (!trace::active || cmd_list->get_device()->get_api() != reshade::api::device_api::opengl) return;
  if (!LoadFunctions()) return;
  GLint framebuffer = 0;
  gl.get_integerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &framebuffer);
  ++draw_index;
  if (framebuffer != 0 && static_cast<GLuint>(framebuffer) == previous_framebuffer) {
    float value[4] = {};
    ReadCentre(static_cast<GLuint>(framebuffer), value);
    if (std::memcmp(value, previous_value, sizeof(value)) != 0) {
      std::stringstream s;
      s << "WOLFPX after draw " << (draw_index - 1) << " program " << previous_program << " ps 0x" << std::hex
        << std::uppercase << pixel_shader_hash_by_program[previous_program] << std::dec << " fb " << framebuffer
        << ": " << value[0] << " " << value[1] << " " << value[2] << " " << value[3];
      reshade::log::message(reshade::log::level::info, s.str().c_str());
      std::memcpy(previous_value, value, sizeof(value));
    }
  } else {
    previous_value[0] = -12345.f;
  }
  previous_framebuffer = static_cast<GLuint>(framebuffer);
  previous_program = current_program;
}

bool OnDraw(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, uint32_t) {
  BeforeDraw(cmd_list);
  return false;
}

bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  BeforeDraw(cmd_list);
  return false;
}

void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*,
               const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  draw_index = 0;
  previous_framebuffer = 0;
  previous_program = 0;
  previous_value[0] = -12345.f;
}

void Register(bool enable) {
  if (enable) {
    reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
    reshade::register_event<reshade::addon_event::draw>(OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::register_event<reshade::addon_event::present>(OnPresent);
  } else {
    reshade::unregister_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::draw>(OnDraw);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
    reshade::unregister_event<reshade::addon_event::present>(OnPresent);
  }
}
}  // namespace gl_probe

// With the Display Proxy the frame reaches the screen through the D3D11 HDR swap chain, presented from ReShade's
// present event. The game renders into the proxy clone, so its own GL back buffer only holds stale ReShade overlay
// pixels, yet the real SwapBuffers still presents that buffer to the same window. In the game's topmost "fullscreen
// windowed" mode the driver shows it instead of the proxy output (black screen with the ReShade overlay burned in).
// ReShade calls the system opengl32 wglSwapBuffers directly, after its present event (opengl_hooks_wgl.cpp), so
// skipping that call drops only the game's GL present. If the proxy could not be created, the game presents as usual.
using WglSwapBuffersFunction = BOOL(WINAPI*)(HDC);
WglSwapBuffersFunction original_wgl_swap_buffers = nullptr;

BOOL WINAPI OnWglSwapBuffers(HDC hdc) {
  if (renodx::mods::swapchain::use_device_proxy && !renodx::utils::device_proxy::device_proxy_creation_failed) return TRUE;
  return original_wgl_swap_buffers(hdc);
}

std::vector<renodx::utils::detour::Export> wgl_swap_buffers_detours = {
    {"wglSwapBuffers", &original_wgl_swap_buffers, &OnWglSwapBuffers},
};

// Scene upgrades by exact size, as fractions of the window's client area (the game sizes its render targets from it, and
// a window can have any shape, e.g. 3818x2104 after switching to windowed in game). Traced 2026-10-08: full = scene
// colour and its MSAA attachments, reflections, resolve target; eighth = luminance / bright-pass / glare / haze chain;
// half is unused in gameplay but kept. Quarter is left 8-bit on purpose: it holds the screen-distortion offsets
// (0xBE1EA7F8, range -0.5..1.5) and vanilla relies on the 8-bit target clamping them.
struct SceneUpgradeScale {
  const char* name;
  int16_t divisor;
};
constexpr SceneUpgradeScale SCENE_UPGRADE_SCALES[] = {
    {.name = "wolfensteintno full", .divisor = 1},
    {.name = "wolfensteintno half", .divisor = 2},
    {.name = "wolfensteintno eighth", .divisor = 8},
};

// Runs before RenoDX's create_resource handler (registered first), so the sizes already apply to the target being created.
bool OnCreateResourceFollowWindowSize(
    reshade::api::device* device,
    reshade::api::resource_desc& desc,
    reshade::api::subresource_data* initial_data,
    reshade::api::resource_usage initial_state) {
  if (desc.type != reshade::api::resource_type::texture_2d) return false;
  if (desc.texture.format != reshade::api::format::r8g8b8a8_unorm) return false;
  if (!renodx::utils::bitwise::HasAnyFlag(desc.usage, reshade::api::resource_usage::render_target)) return false;

  HWND game_window = FindWindowW(L"Wolfenstein The New Order", nullptr);
  if (game_window == nullptr) return false;
  DWORD window_process_id = 0;
  GetWindowThreadProcessId(game_window, &window_process_id);
  if (window_process_id != GetCurrentProcessId()) return false;
  RECT client_rect = {};
  if (GetClientRect(game_window, &client_rect) == FALSE) return false;
  const LONG client_width = client_rect.right - client_rect.left;
  const LONG client_height = client_rect.bottom - client_rect.top;
  if (client_width <= 0 || client_height <= 0) return false;

  auto* upgrade_data = renodx::utils::data::Get<renodx::utils::resource::upgrade::DeviceData>(device);
  if (upgrade_data == nullptr) return false;
  const std::unique_lock lock(upgrade_data->mutex);
  for (auto& upgrade_info : upgrade_data->upgrade_infos) {
    for (const auto& scale : SCENE_UPGRADE_SCALES) {
      if (upgrade_info.name != scale.name) continue;
      const auto width = static_cast<int16_t>(client_width / scale.divisor);
      const auto height = static_cast<int16_t>(client_height / scale.divisor);
      if (upgrade_info.dimensions.width == width && upgrade_info.dimensions.height == height) continue;
      upgrade_info.dimensions.width = width;
      upgrade_info.dimensions.height = height;
      std::stringstream s;
      s << "wolfensteintno: " << scale.name << " upgrade follows window: " << width << "x" << height;
      reshade::log::message(reshade::log::level::info, s.str().c_str());
    }
  }
  return false;
}

bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Wolfenstein: The New Order";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::register_event<reshade::addon_event::create_resource>(OnCreateResourceFollowWindowSize);
      trace::Register(true);

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;

        renodx::mods::swapchain::expected_constant_buffer_index = 13;
        renodx::mods::swapchain::expected_constant_buffer_space = 50;
        renodx::mods::swapchain::use_resource_cloning = true;
        renodx::mods::swapchain::swap_chain_proxy_shaders = {
            {
                reshade::api::device_api::d3d11,
                {
                    .vertex_shader = __swap_chain_proxy_vertex_shader_dx11,
                    .pixel_shader = __swap_chain_proxy_pixel_shader_dx11,
                },
            },
            {
                reshade::api::device_api::d3d12,
                {
                    .vertex_shader = __swap_chain_proxy_vertex_shader_dx12,
                    .pixel_shader = __swap_chain_proxy_pixel_shader_dx12,
                },
            },
        };

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainForceBorderless",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Force Borderless",
              .section = "Display Output",
              .tooltip = "Forces fullscreen to be borderless for proper HDR",
              .labels = {
                  "Disabled",
                  "Enabled",
              },
              .on_change_value = [](float previous, float current) { renodx::mods::swapchain::force_borderless = (current == 1.f); },
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          renodx::mods::swapchain::force_borderless = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainPreventFullscreen",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Prevent Fullscreen",
              .section = "Display Output",
              .tooltip = "Prevent exclusive fullscreen for proper HDR",
              .labels = {
                  "Disabled",
                  "Enabled",
              },
              .on_change_value = [](float previous, float current) { renodx::mods::swapchain::prevent_full_screen = (current == 1.f); },
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          renodx::mods::swapchain::prevent_full_screen = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainEncoding",
              .binding = &shader_injection.swap_chain_encoding,
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 4.f,
              .label = "Encoding",
              .section = "Display Output",
              .labels = {"None", "SRGB", "2.2", "2.4", "HDR10", "scRGB"},
              .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
              .on_change_value = [](float previous, float current) {
                bool is_hdr10 = current == 4;
                shader_injection.swap_chain_encoding_color_space = (is_hdr10 ? 1.f : 0.f);
                // return void
              },
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          bool is_hdr10 = setting->GetValue() == 4;
          renodx::mods::swapchain::SetUseHDR10(is_hdr10);
          renodx::mods::swapchain::use_resize_buffer = setting->GetValue() < 4;
          shader_injection.swap_chain_encoding_color_space = is_hdr10 ? 1.f : 0.f;
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainDeviceProxy",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              // OpenGL has no HDR swap chain of its own; HDR output only exists through the D3D11 Display Proxy.
              .default_value = 1.f,
              .label = "Use Display Proxy",
              .section = "Display Proxy",
              .labels = {"Off", "On"},
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          bool use_device_proxy = setting->GetValue() == 1.f;
          renodx::mods::swapchain::use_device_proxy = use_device_proxy;
          renodx::mods::swapchain::set_color_space = !use_device_proxy;
          if (use_device_proxy) {
            reshade::register_event<reshade::addon_event::present>(OnPresent);

            wchar_t system_directory[MAX_PATH] = {};
            GetSystemDirectoryW(system_directory, MAX_PATH);
            const std::wstring system_opengl_path = std::wstring(system_directory) + L"\\opengl32.dll";
            HMODULE system_opengl = GetModuleHandleW(system_opengl_path.c_str());
            if (system_opengl == nullptr) {
              reshade::log::message(reshade::log::level::warning, "wolfensteintno: system opengl32.dll not loaded, GL present kept");
            } else if (renodx::utils::detour::Install(system_opengl, wgl_swap_buffers_detours).Complete()) {
              reshade::log::message(reshade::log::level::info, "wolfensteintno: GL present skipped while the proxy presents");
            } else {
              reshade::log::message(reshade::log::level::warning, "wolfensteintno: wglSwapBuffers detour failed, GL present kept");
            }
          } else {
            shader_injection.custom_flip_uv_y = 0.f;
          }
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainDeviceProxyBaseWaitIdle",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Base Wait Idle",
              .section = "Display Proxy",
              .labels = {"Off", "On"},
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          bool use_device_proxy =
              renodx::mods::swapchain::device_proxy_wait_idle_source = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainDeviceProxyProxyWaitIdle",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Proxy Wait Idle",
              .section = "Display Proxy",
              .labels = {"Off", "On"},
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          bool use_device_proxy =
              renodx::mods::swapchain::device_proxy_wait_idle_destination = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        for (const auto& [key, format] : UPGRADE_TARGETS) {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "Upgrade_" + key,
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = key,
              .section = "Resource Upgrades",
              .labels = {
                  "Off",
                  "Output size",
                  "Output ratio",
                  "Any size",
              },
              .is_global = true,
              .is_visible = []() { return settings[0]->GetValue() >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          settings.push_back(setting);

          auto value = setting->GetValue();
          if (value > 0) {
            renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
                .old_format = format,
                .new_format = reshade::api::format::r16g16b16a16_float,
                .ignore_size = (value == UPGRADE_TYPE_ANY),
                // OpenGL: clone + redirect leaves the 3D scene black; upgrade the texture itself at creation instead.
                .use_resource_view_cloning = false,
                .aspect_ratio = static_cast<float>((value == UPGRADE_TYPE_OUTPUT_RATIO)
                                                       ? renodx::mods::swapchain::SwapChainUpgradeTarget::BACK_BUFFER
                                                       : renodx::mods::swapchain::SwapChainUpgradeTarget::ANY),
                .usage_include = reshade::api::resource_usage::render_target,
            });
            std::stringstream s;
            s << "Applying user resource upgrade for ";
            s << format << ": " << value;
            reshade::log::message(reshade::log::level::info, s.str().c_str());
          }
        }

        {
          // Wolfenstein creates its full-screen render targets (scene colour 0xFBF, half/quarter res) before the swapchain
          // exists, so a back-buffer size match ("Output size") misses them and the scene stays 8-bit. Match explicit
          // fractions of the window instead (SCENE_UPGRADE_SCALES; start from the screen size,
          // OnCreateResourceFollowWindowSize keeps them on the window).
          const auto screen_width = static_cast<int16_t>(GetSystemMetrics(SM_CXSCREEN));
          const auto screen_height = static_cast<int16_t>(GetSystemMetrics(SM_CYSCREEN));
          for (const auto& scale : SCENE_UPGRADE_SCALES) {
            renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
                .old_format = reshade::api::format::r8g8b8a8_unorm,
                .new_format = reshade::api::format::r16g16b16a16_float,
                // OpenGL: clone + redirect leaves the 3D scene black; upgrade the texture itself at creation instead.
                .use_resource_view_cloning = false,
                .dimensions = {
                    .width = static_cast<int16_t>(screen_width / scale.divisor),
                    .height = static_cast<int16_t>(screen_height / scale.divisor),
                },
                .usage_include = reshade::api::resource_usage::render_target,
                .name = scale.name,
            });
          }
        }

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::create_resource>(OnCreateResourceFollowWindowSize);
      trace::Register(false);
      if (original_wgl_swap_buffers != nullptr) {
        // Never let an exception leave DllMain.
        try {
          renodx::utils::detour::Uninstall(wgl_swap_buffers_detours);
        } catch (const std::exception&) {
          reshade::log::message(reshade::log::level::warning, "wolfensteintno: wglSwapBuffers detour removal failed");
        }
      }
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);
  if (fdw_reason == DLL_PROCESS_ATTACH) gl_probe::Register(true);
  if (fdw_reason == DLL_PROCESS_DETACH) gl_probe::Register(false);

  return TRUE;
}
