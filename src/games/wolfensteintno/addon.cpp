/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <cstddef>
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
#include "../../utils/resource_upgrade.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

// The GLSL composite 0xA85A9FE0 reads these ShaderInjectData fields by explicit byte offset (layout(offset = N) in its
// RenoDXShaderInjection block). Update both sides together.
static_assert(sizeof(ShaderInjectData) % 16 == 0, "std140 uniform block size must be a multiple of 16 bytes");
static_assert(offsetof(ShaderInjectData, peak_white_nits) == 0);
static_assert(offsetof(ShaderInjectData, diffuse_white_nits) == 4);
static_assert(offsetof(ShaderInjectData, graphics_white_nits) == 8);
static_assert(offsetof(ShaderInjectData, tone_map_type) == 16);
static_assert(offsetof(ShaderInjectData, tone_map_exposure) == 20);
static_assert(offsetof(ShaderInjectData, tone_map_highlights) == 24);
static_assert(offsetof(ShaderInjectData, tone_map_shadows) == 28);
static_assert(offsetof(ShaderInjectData, tone_map_contrast) == 32);
static_assert(offsetof(ShaderInjectData, tone_map_saturation) == 36);
static_assert(offsetof(ShaderInjectData, tone_map_blowout) == 44);
static_assert(offsetof(ShaderInjectData, tone_map_hue_correction) == 52);
static_assert(offsetof(ShaderInjectData, intermediate_encoding) == 88);

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
        .key = "ToneMapHueCorrection",
        .binding = &shader_injection.tone_map_hue_correction,
        .default_value = 100.f,
        .label = "Hue Correction",
        .section = "Tone Mapping",
        .tooltip = "Hue retention strength.",
        .min = 0.f,
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type == 2.f; },  // PsychoV only (0xA85A9FE0)
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .is_enabled = []() { return shader_injection.tone_map_type == 2.f; },  // PsychoV only (0xA85A9FE0)
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
        .is_enabled = []() { return shader_injection.tone_map_type == 2.f; },  // PsychoV only (0xA85A9FE0)
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
        .is_enabled = []() { return shader_injection.tone_map_type == 2.f; },  // PsychoV only (0xA85A9FE0)
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
        .is_enabled = []() { return shader_injection.tone_map_type == 2.f; },  // PsychoV only (0xA85A9FE0)
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .is_enabled = []() { return shader_injection.tone_map_type == 2.f; },  // PsychoV only (0xA85A9FE0)
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
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

// With the Display Proxy there are two ReShade runtimes: the OpenGL one (ReShade.ini), whose overlay is drawn into the
// GL back buffer that is no longer presented, and the D3D11 proxy one (ReShade2.ini) that the player sees. Both open on
// the overlay key and take the same mouse input, so a click on the visible overlay also hit a different control in the
// invisible one (other settings changing, effects switching on and being saved to the shared preset). Keep the
// invisible overlay closed.
// ReShade lets only the first runtime registered for a window block the game's input (runtime.cpp
// _primary_input_handler), which is the OpenGL one. So while the visible proxy overlay is open, the OpenGL runtime is
// asked to block input every frame: block_input_next_frame() from the present event, which ReShade fires right before
// that runtime's GUI pass applies the flag (opengl_hooks_wgl.cpp).
reshade::api::effect_runtime* opengl_runtime = nullptr;
bool proxy_overlay_open = false;

bool OnReShadeOpenOverlay(reshade::api::effect_runtime* runtime, bool open, reshade::api::input_source source) {
  if (runtime->get_device()->get_api() != reshade::api::device_api::opengl) {
    proxy_overlay_open = open;
    return false;
  }
  if (!open) return false;
  return renodx::mods::swapchain::use_device_proxy && !renodx::utils::device_proxy::device_proxy_creation_failed;
}

void OnInitEffectRuntime(reshade::api::effect_runtime* runtime) {
  if (runtime->get_device()->get_api() == reshade::api::device_api::opengl) {
    opengl_runtime = runtime;
  }
}

void OnDestroyEffectRuntime(reshade::api::effect_runtime* runtime) {
  if (runtime == opengl_runtime) {
    opengl_runtime = nullptr;
  }
}

void OnPresentBlockGameInput(reshade::api::command_queue* queue, reshade::api::swapchain*, const reshade::api::rect*,
                             const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  if (!proxy_overlay_open || opengl_runtime == nullptr) return;
  if (queue->get_device()->get_api() != reshade::api::device_api::opengl) return;
  opengl_runtime->block_input_next_frame();
}

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
      reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnReShadeOpenOverlay);
      reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
      reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
      reshade::register_event<reshade::addon_event::present>(OnPresentBlockGameInput);

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
      reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(OnReShadeOpenOverlay);
      reshade::unregister_event<reshade::addon_event::init_effect_runtime>(OnInitEffectRuntime);
      reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
      reshade::unregister_event<reshade::addon_event::present>(OnPresentBlockGameInput);
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

  return TRUE;
}
