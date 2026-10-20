/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <d3d9.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/swapchain.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection;

// The scene as it was just before the HUD is drawn, kept for the final pass (0x785AEA45.ps_3_0.hlsl), which caps what the
// HUD's additive blends can add (the original 8-bit HUD buffer capped the sum at white). Native D3D9: the texture is
// device-owned and has to be released before IDirect3DDevice9::Reset and when the device is destroyed (not at detach: the
// D3D9 runtime may already be unloading then). The state below is written from draw and present callbacks without a lock:
// the game renders and presents from one thread. D3D9 ps_3_0 sampler 14 is not used by any game shader. on_drawn is not
// used on purpose: it cancels the draw and replays it through the ReShade command list, which draws nothing for this
// game's full-screen quads (black screen).
constexpr uint32_t SCENE_BEFORE_HUD_SAMPLER = 14;
IDirect3DTexture9* scene_before_hud = nullptr;
uint64_t scene_finish_target = 0;                // resource the scene finish copy drew into this frame (the HUD buffer)
bool scene_before_hud_save_pending = false;      // the scene finish copy ran and its result is not saved yet
bool scene_before_hud_valid_this_frame = false;  // saved during the current frame

void ReleaseSceneBeforeHud() {
  if (scene_before_hud != nullptr) {
    IDirect3DDevice9* native_device = nullptr;
    if (SUCCEEDED(scene_before_hud->GetDevice(&native_device))) {
      native_device->SetTexture(SCENE_BEFORE_HUD_SAMPLER, nullptr);
      native_device->Release();
    }
    scene_before_hud->Release();
    scene_before_hud = nullptr;
  }
  scene_before_hud_valid_this_frame = false;
  scene_before_hud_save_pending = false;
  scene_finish_target = 0;
}

// Runs before the first 0x707C2DA8 HUD sprite is drawn, i.e. after the scene finish copy has been executed: keeps a copy
// of the HUD buffer (the finished scene). Draws into any other target are not the HUD buffer and are ignored.
bool SaveSceneBeforeHud(reshade::api::command_list* cmd_list) {
  if (!scene_before_hud_save_pending) return true;

  auto* device = cmd_list->get_device();
  const auto& render_targets = renodx::utils::swapchain::GetRenderTargets(cmd_list);
  if (render_targets.empty()) return true;
  const auto target_resource = device->get_resource_from_view(render_targets[0]);
  if (target_resource.handle != scene_finish_target) return true;
  scene_before_hud_save_pending = false;

  auto* native_device = reinterpret_cast<IDirect3DDevice9*>(device->get_native());
  auto* native_target = reinterpret_cast<IDirect3DResource9*>(target_resource.handle);
  IDirect3DSurface9* source_surface = nullptr;
  switch (native_target->GetType()) {
    case D3DRTYPE_TEXTURE:
      if (FAILED(static_cast<IDirect3DTexture9*>(native_target)->GetSurfaceLevel(0, &source_surface))) return true;
      break;
    case D3DRTYPE_SURFACE:
      source_surface = static_cast<IDirect3DSurface9*>(native_target);
      source_surface->AddRef();
      break;
    default:
      return true;
  }

  D3DSURFACE_DESC source_desc = {};
  source_surface->GetDesc(&source_desc);
  // The cap only makes sense against the float16 scene: an 8-bit copy would cap every HDR pixel at white.
  if (source_desc.Format != D3DFMT_A16B16G16R16F) {
    source_surface->Release();
    return true;
  }

  if (scene_before_hud != nullptr) {
    D3DSURFACE_DESC current_desc = {};
    scene_before_hud->GetLevelDesc(0, &current_desc);
    if (current_desc.Width != source_desc.Width || current_desc.Height != source_desc.Height) {
      ReleaseSceneBeforeHud();
    }
  }
  if (scene_before_hud == nullptr) {
    native_device->CreateTexture(
        source_desc.Width, source_desc.Height, 1, D3DUSAGE_RENDERTARGET, source_desc.Format, D3DPOOL_DEFAULT,
        &scene_before_hud, nullptr);
  }

  IDirect3DSurface9* destination_surface = nullptr;
  if (scene_before_hud != nullptr && SUCCEEDED(scene_before_hud->GetSurfaceLevel(0, &destination_surface))) {
    scene_before_hud_valid_this_frame =
        SUCCEEDED(native_device->StretchRect(source_surface, nullptr, destination_surface, nullptr, D3DTEXF_POINT));
    destination_surface->Release();
  }
  source_surface->Release();
  return true;
}

renodx::mods::shader::CustomShaders custom_shaders = {
    CustomShaderEntry(0xD46EFAB6),
    CustomShaderEntry(0x36401A42),
    {
        0x707C2DA8,
        {
            .crc32 = 0x707C2DA8,
            .code = __0x707C2DA8,
            .on_draw = &SaveSceneBeforeHud,
        },
    },
    {
        0x19BC57FB,
        {
            .crc32 = 0x19BC57FB,
            .code = __0x19BC57FB,
            // 0x19BC57FB is a plain texture copy that the luminance downsample chain uses as well. The scene finish
            // replacement only applies to the copy into a back-buffer sized target (the finished scene handed to the
            // HUD buffer).
            .on_replace = [](reshade::api::command_list* cmd_list) {
              auto* device = cmd_list->get_device();
              const auto& render_targets = renodx::utils::swapchain::GetRenderTargets(cmd_list);
              if (render_targets.empty()) return false;
              const auto target_resource = device->get_resource_from_view(render_targets[0]);
              const auto target_desc = device->get_resource_desc(target_resource);
              const auto back_buffer_desc = renodx::utils::swapchain::GetBackBufferDesc(cmd_list);
              if (target_desc.texture.width != back_buffer_desc.texture.width
                  || target_desc.texture.height != back_buffer_desc.texture.height
                  || target_desc.texture.format != reshade::api::format::r16g16b16a16_float) {
                return false;
              }
              scene_finish_target = target_resource.handle;
              scene_before_hud_save_pending = true;
              return true;
            },
        },
    },
    {
        0x785AEA45,
        {
            .crc32 = 0x785AEA45,
            .code = __0x785AEA45,
            .on_draw = [](reshade::api::command_list* cmd_list) {
              // Runs before RenoDX pushes the constants of this draw, so hud_clamp reaches the shader.
              shader_injection.hud_clamp = (scene_before_hud_valid_this_frame ? 1.f : 0.f);
              if (!scene_before_hud_valid_this_frame) return true;
              auto* native_device = reinterpret_cast<IDirect3DDevice9*>(cmd_list->get_device()->get_native());
              native_device->SetTexture(SCENE_BEFORE_HUD_SAMPLER, scene_before_hud);
              native_device->SetSamplerState(SCENE_BEFORE_HUD_SAMPLER, D3DSAMP_MINFILTER, D3DTEXF_POINT);
              native_device->SetSamplerState(SCENE_BEFORE_HUD_SAMPLER, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
              native_device->SetSamplerState(SCENE_BEFORE_HUD_SAMPLER, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
              native_device->SetSamplerState(SCENE_BEFORE_HUD_SAMPLER, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
              native_device->SetSamplerState(SCENE_BEFORE_HUD_SAMPLER, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
              native_device->SetSamplerState(SCENE_BEFORE_HUD_SAMPLER, D3DSAMP_SRGBTEXTURE, FALSE);
              return true;
            },
        },
    },
};

// The frame is over: unbind the saved scene from its sampler and forget it, so a stale copy is never used. The D3D11
// proxy device presents as well; only the game's D3D9 device owns the texture.
void OnPresent(
    reshade::api::command_queue*,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  auto* device = swapchain->get_device();
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  reinterpret_cast<IDirect3DDevice9*>(device->get_native())->SetTexture(SCENE_BEFORE_HUD_SAMPLER, nullptr);
  scene_before_hud_save_pending = false;
  scene_before_hud_valid_this_frame = false;
}

void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool) {
  if (swapchain->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  ReleaseSceneBeforeHud();
}

void OnDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  ReleaseSceneBeforeHud();
}

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
        .tooltip = "Vanilla keeps the original look, clipped at white. None extends the original curve past white, hard-limited at Peak Brightness. "
                   "Roll-off extends it and compresses the brightest highlights smoothly to Peak Brightness. "
                   "PsychoV maps the scene with the PsychoV observer model anchored to the original curve; the brightest highlights are then compressed smoothly to Peak Brightness, like Roll-off.",
        .labels = {"Vanilla", "None", "Roll-off", "PsychoV"},
        .parse = [](float value) {
          // shader values: 4 = Roll-off, 5 = PsychoV
          if (value == 2.f) return 4.f;
          if (value == 3.f) return 5.f;
          return value;
        },
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
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
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
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
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
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
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
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
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
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ShadowFilterWidth",
        .binding = &shader_injection.shadow_filter_width,
        .default_value = 3.f,
        .label = "Shadow Filter Width",
        .section = "Shadows",
        .tooltip = "Width of the soft shadow filter. 1 is the original game (blocky shadow edges), higher values hide the blocks.",
        .min = 1.f,
        .max = 4.f,
        .format = "%.1f",
    },
    new renodx::utils::settings::Setting{
        .key = "IntermediateEncoding",
        .binding = &shader_injection.intermediate_encoding,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .label = "Intermediate Encoding",
        .section = "Display Output",
        .tooltip = "The curve the finished image is stored and shown with. 2.2 matches how the original game is displayed.",
        .labels = {"None", "SRGB", "2.2", "2.4"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
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
  renodx::utils::settings::UpdateSetting("ToneMapType", 0.f);
  renodx::utils::settings::UpdateSetting("ToneMapPeakNits", 203.f);
  renodx::utils::settings::UpdateSetting("ToneMapGameNits", 203.f);
  renodx::utils::settings::UpdateSetting("ToneMapUINits", 203.f);
  renodx::utils::settings::UpdateSetting("ColorGradeExposure", 1.f);
  renodx::utils::settings::UpdateSetting("ColorGradeHighlights", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeShadows", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeContrast", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeSaturation", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeHighlightSaturation", 50.f);
  renodx::utils::settings::UpdateSetting("ColorGradeBlowout", 0.f);
  renodx::utils::settings::UpdateSetting("ColorGradeFlare", 0.f);
  renodx::utils::settings::UpdateSetting("ShadowFilterWidth", 1.f);
}

bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX Binary Domain";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;
        renodx::mods::shader::constant_buffer_offset = 150 * 4;

        renodx::mods::swapchain::expected_constant_buffer_index = 13;
        renodx::mods::swapchain::expected_constant_buffer_space = 50;
        renodx::mods::swapchain::prevent_full_screen = false;
        renodx::mods::swapchain::force_screen_tearing = false;
        renodx::mods::swapchain::use_resource_cloning = true;
        renodx::mods::swapchain::set_color_space = false;
        renodx::mods::swapchain::use_device_proxy = true;
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
              .default_value = 1.f,
              .label = "Force Borderless",
              .section = "Display Output",
              .tooltip = "Forces fullscreen to be borderless for proper HDR",
              .labels = {
                  "Disabled",
                  "Enabled",
              },
              .on_change_value = [](float previous, float current) {
                renodx::mods::swapchain::force_borderless = (current == 1.f);
              },
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
              .default_value = 1.f,
              .label = "Prevent Fullscreen",
              .section = "Display Output",
              .tooltip = "Prevent exclusive fullscreen for proper HDR",
              .labels = {
                  "Disabled",
                  "Enabled",
              },
              .on_change_value = [](float previous, float current) {
                renodx::mods::swapchain::prevent_full_screen = (current == 1.f);
              },
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
                shader_injection.swap_chain_encoding_color_space = (current == 4 ? 1.f : 0.f);
              },
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          bool is_hdr10 = setting->GetValue() == 4;
          renodx::mods::swapchain::SetUseHDR10(is_hdr10);
          renodx::mods::swapchain::use_resize_buffer = setting->GetValue() < 4;
          renodx::mods::swapchain::use_resize_buffer_on_demand = renodx::mods::swapchain::use_resize_buffer;
          renodx::mods::swapchain::set_color_space = !renodx::mods::swapchain::use_resize_buffer;
          shader_injection.swap_chain_encoding_color_space = (is_hdr10 ? 1.f : 0.f);
          settings.push_back(setting);
        }

        // Every back-buffer sized 8-bit render target: the scene composite (0xD46EFAB6) writes one, the copies, the AA
        // pass, the haze sprites and the HUD then keep its range (checked with a Devkit resource clone).
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::b8g8r8a8_unorm,
            .new_format = reshade::api::format::r16g16b16a16_float,
        });

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
