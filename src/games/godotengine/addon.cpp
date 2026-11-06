/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <windows.h>

#include <atomic>
#include <initializer_list>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/data.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/swapchain.hpp"
#include "./shared.h"

namespace {

// Set once the addon leaves the game to Godot's own HDR swap chain: the swap chain then stays as Godot made it, so the
// shader replacements must not write HDR either. Read from draw-time callbacks.
std::atomic_bool standing_down = false;
// Set once D3D12 devices are left alone because Godot renders with Vulkan (OnInitDevice); Vulkan stays converted.
std::atomic_bool d3d12_left_alone = false;

bool IsActive(reshade::api::command_list* cmd_list) {
  if (standing_down) return false;
  return !d3d12_left_alone || cmd_list->get_device()->get_api() != reshade::api::device_api::d3d12;
}

// Godot also tone maps SubViewports, whose narrower 8-bit targets are not cloned: an HDR result there would be
// clipped per channel by the UNORM write, so those draws keep the original shader. Float targets are the viewport
// clone, Godot's own RGBA16F tonemap destination (before FSR/SMAA) and HDR 2D viewports.
bool IsFloatRenderTarget(reshade::api::command_list* cmd_list) {
  if (!IsActive(cmd_list)) return false;
  auto* cmd_list_data = renodx::utils::data::Get<renodx::utils::swapchain::CommandListData>(cmd_list);
  if (cmd_list_data == nullptr || cmd_list_data->current_render_targets.empty()) return false;
  if (cmd_list_data->current_render_targets[0].handle == 0u) return false;
  bool is_float = false;
  renodx::utils::resource::GetResourceViewInfo(
      cmd_list_data->current_render_targets[0],
      [&is_float](const renodx::utils::resource::ResourceViewInfo& view_info) {
        is_float = view_info.clone.handle != 0u
                   || view_info.desc.format == reshade::api::format::r16g16b16a16_float;
      });
  return is_float;
}

renodx::mods::shader::CustomShaders custom_shaders = {
    // Final viewport blit (blit.glsl), D3D12: 4.3, 4.4, 4.5, 4.6, 4.7.
    CustomShaderEntryCallback(0x22687DAC, &IsActive),  // 4.3
    CustomShaderEntryCallback(0xA252A18D, &IsActive),  // 4.4
    CustomShaderEntryCallback(0x2845C867, &IsActive),  // 4.5
    CustomShaderEntryCallback(0x83ADBE18, &IsActive),  // 4.6
    CustomShaderEntryCallback(0x13955D03, &IsActive),  // 4.7
    // Final viewport blit, Vulkan: 4.3, 4.4, 4.5 and 4.6, 4.7.
    CustomShaderEntryCallback(0x7109F4EE, &IsActive),
    CustomShaderEntryCallback(0xE52452DA, &IsActive),
    CustomShaderEntryCallback(0x416C3F79, &IsActive),
    CustomShaderEntryCallback(0x15F02F27, &IsActive),
    // Forward+ tonemap (tonemap.glsl), D3D12: 3D/1D colour-correction LUT x bilinear/bicubic glow.
    // 4.3
    CustomShaderEntryCallback(0x914EEDB6, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x51BDE35D, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xC200205E, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xA71CD15D, &IsFloatRenderTarget),
    // 4.4
    CustomShaderEntryCallback(0xA0DD4706, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x4A3080B2, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x9E1957A5, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xFDC2A39E, &IsFloatRenderTarget),
    // 4.5
    CustomShaderEntryCallback(0x2D4AED1E, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xD44A3465, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x19657F6D, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xF01F42A0, &IsFloatRenderTarget),
    // 4.6
    CustomShaderEntryCallback(0x0656E0CE, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xBF1B5017, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x667C84AD, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x6291BC2F, &IsFloatRenderTarget),
    // 4.7
    CustomShaderEntryCallback(0xB21EACEC, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x3F415A05, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x32F7250B, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xEF76A934, &IsFloatRenderTarget),
    // Forward+ tonemap, Vulkan, same variant order.
    // 4.3
    CustomShaderEntryCallback(0x0941A535, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x4E7421AB, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xE5B09824, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xCCD1E425, &IsFloatRenderTarget),
    // 4.4
    CustomShaderEntryCallback(0xFA110435, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x679069A8, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x219BD7CC, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x12C854BA, &IsFloatRenderTarget),
    // 4.5
    CustomShaderEntryCallback(0xE1AC35ED, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x277EEA0D, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x8EA1E429, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x5B6C8FD0, &IsFloatRenderTarget),
    // 4.6
    CustomShaderEntryCallback(0x8D6FF34D, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x80315116, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xB4D324B8, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xCC2CAB8B, &IsFloatRenderTarget),
    // 4.7
    CustomShaderEntryCallback(0x548A4E8B, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x88473F26, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0x67FE91D5, &IsFloatRenderTarget),
    CustomShaderEntryCallback(0xE6ABA95D, &IsFloatRenderTarget),
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
        .default_value = 3.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Sets the tone mapper type"
                   "\nGodot HDR: Godot 4.7's own HDR curve for AgX and Reinhard (white above 1), RenoDRT otherwise."
                   " Not neutral: Reinhard lifts midtones. The grading sliders apply to RenoDRT only.",
        .labels = {"Vanilla", "None", "ACES", "RenoDRT", "Godot HDR"},
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
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
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
        .is_enabled = []() { return shader_injection.tone_map_type == 3; },
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

void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"ToneMapType", 0.f},
      {"ToneMapPeakNits", 203.f},
      {"ToneMapGameNits", 203.f},
      {"ToneMapUINits", 203.f},
      {"GammaCorrection", 0.f},
      {"ColorGradeExposure", 1.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"ColorGradeHighlightSaturation", 50.f},
      {"ColorGradeBlowout", 0.f},
      {"ColorGradeFlare", 0.f},
      {"ColorGradeScene", 100.f},
  });
}

// NVIDIA can present a Vulkan game through a DXGI swap chain. ReShade then sees only the driver's D3D12 device, which
// copies Godot's finished RGBA8 image into the swap chain: modifying that swap chain gives a black screen, and Godot's
// own shaders are not visible there. Godot loads vulkan-1.dll when it renders with (or tried) Vulkan, so leave every
// D3D12 device alone once it is loaded, swap chain and shader replacements (the Vulkan device itself, seen through ReShade's Vulkan
// layer, is converted); a D3D12 fallback after a failed Vulkan start is skipped too, which is safe.
// Registered before mods::swapchain::Use so it runs before its OnInitDevice.
void OnInitDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d12) return;
  if (GetModuleHandleW(L"vulkan-1.dll") == nullptr) return;
  d3d12_left_alone = true;
  renodx::mods::swapchain::ignored_device_apis.insert(device->get_api());
  reshade::log::message(reshade::log::level::info, "Godot renders with Vulkan (vulkan-1.dll is loaded): device left untouched.");
}

// Godot 4.7+ can output HDR itself (Window.hdr_output_requested): it then recreates its swap chain as RGBA16F scRGB.
// Leave the game to Godot from then on (swap chain and shaders). One-way: mods::swapchain drops its device data when
// it aborts a swap chain, so a later 8-bit swap chain could not be converted correctly; a restart is needed after
// turning Godot's HDR off. Registered before mods::swapchain::Use so it runs before its handler.
bool OnCreateSwapchain(reshade::api::device_api device_api, reshade::api::swapchain_desc& desc, void* hwnd) {
  // Also skips the APIs the addon does not convert (d3d9/10/11, OpenGL) and D3D12 left alone for Vulkan.
  if (renodx::mods::swapchain::ignored_device_apis.contains(device_api)) return false;
  // Godot asks for RGBA16F only when its own HDR output is on, on D3D12 and Vulkan alike (SDR is 8-bit).
  if (desc.back_buffer.texture.format != reshade::api::format::r16g16b16a16_float) return false;
  standing_down = true;
  renodx::mods::swapchain::ignored_device_apis.insert(device_api);
  reshade::log::message(reshade::log::level::info, "Godot native HDR swap chain: left to Godot.");
  return false;
}

bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Godot Engine";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::create_swapchain>(OnCreateSwapchain);

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;
        // Godot's tonemap root signature already holds 28 root constants and 8 descriptor tables (36 of 64 DWORDs),
        // too few for the injected constants; a root CBV costs 2.
        renodx::mods::shader::use_root_signature_cbv = true;
        // Vulkan: Godot's push-constant ranges are fragment-only (tonemap) or vertex + fragment (blit). Extend those
        // ranges for the injected data; the default (pixel + compute) would add a separate range the fragment shader
        // cannot see.
        renodx::mods::shader::minimum_constant_buffer_stages = reshade::api::shader_stage::pixel;

        renodx::mods::swapchain::expected_constant_buffer_index = 13;
        renodx::mods::swapchain::expected_constant_buffer_space = 50;
        renodx::mods::swapchain::use_resource_cloning = true;

        // Only Godot 4 Forward+ on D3D12 and Vulkan is proven so far. Without a proxy shader for the other APIs the
        // swap chain would still switch to HDR10 and show SDR output as PQ, so leave those games untouched (no
        // upgrades either: mods::swapchain stops at OnInitDevice for an ignored API).
        renodx::mods::swapchain::ignored_device_apis = {
            reshade::api::device_api::d3d9,
            reshade::api::device_api::d3d10,
            reshade::api::device_api::d3d11,
            reshade::api::device_api::opengl,
        };

        // Godot 4 creates the viewport target and the normal/roughness buffer with identical descs (RGBA8 typeless,
        // render target + UAV), so these rules clone both. A viewport kept at its aspect ratio fills the window in one
        // dimension only: letterboxed it is as wide as the swap chain (1920x1080 in 1920x1200), pillarboxed as tall
        // (3779x2126 in 3840x2126), so either dimension matches; not the exact size, since windows can also be a few
        // pixels larger (3840x2160 in 3840x2162). SubViewport targets must stay RGBA8 (their views force alpha to 1 or
        // decode sRGB, which a D3D12 clone view loses); one as wide or as tall as the window is cloned anyway.
        for (const auto& dimensions : {
                 renodx::utils::resource::ResourceUpgradeInfo::Dimensions{
                     .width = renodx::utils::resource::ResourceUpgradeInfo::BACK_BUFFER,
                 },
                 renodx::utils::resource::ResourceUpgradeInfo::Dimensions{
                     .height = renodx::utils::resource::ResourceUpgradeInfo::BACK_BUFFER,
                 },
             }) {
          renodx::mods::swapchain::resource_upgrade_infos.push_back({
              .old_format = reshade::api::format::r8g8b8a8_typeless,
              .new_format = reshade::api::format::r16g16b16a16_float,
              .use_resource_view_cloning = true,
              .dimensions = dimensions,
              .usage_include = reshade::api::resource_usage::render_target | reshade::api::resource_usage::unordered_access,
          });
        }
        renodx::mods::swapchain::swap_chain_proxy_shaders = {
            {
                reshade::api::device_api::d3d12,
                {
                    .vertex_shader = __swap_chain_proxy_vertex_shader_dx12,
                    .pixel_shader = __swap_chain_proxy_pixel_shader_dx12,
                },
            },
            {
                reshade::api::device_api::vulkan,
                {
                    .vertex_shader = __swap_chain_proxy_vertex_shader,
                    .pixel_shader = __swap_chain_proxy_pixel_shader,
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
          setting->ReadFromConfig(renodx::utils::settings::global_name);
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
          setting->ReadFromConfig(renodx::utils::settings::global_name);
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
          setting->ReadFromConfig(renodx::utils::settings::global_name);
          bool is_hdr10 = setting->GetValue() == 4;
          renodx::mods::swapchain::SetUseHDR10(is_hdr10);
          renodx::mods::swapchain::use_resize_buffer = setting->GetValue() < 4;
          shader_injection.swap_chain_encoding_color_space = is_hdr10 ? 1.f : 0.f;
          settings.push_back(setting);
        }

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::create_swapchain>(OnCreateSwapchain);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
