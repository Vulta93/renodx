/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <d3d9.h>

#include <cfloat>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection;

// With the Display Proxy the game renders into a float16 clone of the backbuffer, but RenoDX's copy_texture_region
// redirect only handles texture_2d/texture_3d resources, and the D3D9 swapchain backbuffer is a `surface`.
// Warrior Within renders the frame into a 3840x2160 render-target texture and moves it with StretchRect in both
// directions (Devkit snapshot): draw 603 blits texture -> backbuffer, 604 copies backbuffer -> texture (glow source),
// the glow and the HUD are drawn into the texture, and 621 copies texture -> backbuffer at the end of the frame.
// Unredirected, 604 read the original backbuffer and 621 wrote it (the proxy never sees it: no HUD, stale data).
// Redirect any copy that has the backbuffer on either side to the clones with a native StretchRect, as
// utils::device_proxy does for its DX9 handoff.
IDirect3DSurface9* GetD3D9Surface(reshade::api::resource resource) {
  auto* native = reinterpret_cast<IDirect3DResource9*>(resource.handle);
  switch (native->GetType()) {
    case D3DRTYPE_SURFACE:
      native->AddRef();
      return static_cast<IDirect3DSurface9*>(native);
    case D3DRTYPE_TEXTURE: {
      IDirect3DSurface9* surface = nullptr;
      if (FAILED(static_cast<IDirect3DTexture9*>(native)->GetSurfaceLevel(0, &surface))) return nullptr;
      return surface;
    }
    default:
      return nullptr;
  }
}

bool OnCopyBackBufferSurface(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    uint32_t source_subresource,
    const reshade::api::subresource_box* source_box,
    reshade::api::resource dest,
    uint32_t dest_subresource,
    const reshade::api::subresource_box* dest_box,
    reshade::api::filter_mode filter) {
  auto* device = cmd_list->get_device();
  if (device->get_api() != reshade::api::device_api::d3d9) return false;
  if (source_subresource != 0 || dest_subresource != 0) return false;

  // Only copies with the (cloned) backbuffer surface on at least one side; texture <-> texture copies stay with RenoDX.
  bool has_cloned_surface = false;
  auto resolve = [&](reshade::api::resource resource) {
    reshade::api::resource target = resource;
    renodx::utils::resource::GetResourceInfo(resource, [&](const renodx::utils::resource::ResourceInfo& info) {
      if (!info.clone_enabled || info.clone.handle == 0u) return;
      target = info.clone;
      if (info.desc.type == reshade::api::resource_type::surface) has_cloned_surface = true;
    });
    return target;
  };
  const reshade::api::resource source_target = resolve(source);
  const reshade::api::resource dest_target = resolve(dest);
  if (!has_cloned_surface) return false;

  IDirect3DSurface9* src_surface = GetD3D9Surface(source_target);
  IDirect3DSurface9* dst_surface = GetD3D9Surface(dest_target);
  bool handled = false;
  if (src_surface != nullptr && dst_surface != nullptr) {
    RECT src_rect = {};
    RECT dst_rect = {};
    if (source_box != nullptr) {
      src_rect = {static_cast<LONG>(source_box->left), static_cast<LONG>(source_box->top),
                  static_cast<LONG>(source_box->right), static_cast<LONG>(source_box->bottom)};
    }
    if (dest_box != nullptr) {
      dst_rect = {static_cast<LONG>(dest_box->left), static_cast<LONG>(dest_box->top),
                  static_cast<LONG>(dest_box->right), static_cast<LONG>(dest_box->bottom)};
    }
    auto* native_device = reinterpret_cast<IDirect3DDevice9*>(device->get_native());
    const HRESULT hr = native_device->StretchRect(
        src_surface, source_box != nullptr ? &src_rect : nullptr,
        dst_surface, dest_box != nullptr ? &dst_rect : nullptr,
        filter == reshade::api::filter_mode::min_mag_mip_point ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    handled = SUCCEEDED(hr);
  }
  if (src_surface != nullptr) src_surface->Release();
  if (dst_surface != nullptr) dst_surface->Release();
  return handled;
}

// ---- Scene finish pass (game-local; there is no scene shader to replace) ----
// Frame order (Devkit snapshot, gameplay): 3D scene, post effects and glow source -> render-target texture T;
// draw 603 = scene blit T -> backbuffer (vs 0x344B89F7, ps_1_1 0x2059E26C: r0 = t0 * c0 + (t0 - c0)); copy
// backbuffer -> T; glow blur + add into T; HUD draws into T; copy T -> backbuffer.
// The blit is the last draw that sees only the 3D scene, so the addon draws it itself (same math, c0 read from the
// device) through scene_finish_ps (tone mapping, user grading, RenderIntermediatePass for Game vs UI Brightness)
// and skips the original. Native D3D9 calls, state saved/restored with a state block.
constexpr uint32_t kSceneBlitVertexShader = 0x344B89F7;
constexpr uint32_t kSceneBlitPixelShader = 0x2059E26C;

struct SceneFinishPass {
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexShader9* vertex_shader = nullptr;
  IDirect3DPixelShader9* pixel_shader = nullptr;
  IDirect3DVertexDeclaration9* vertex_declaration = nullptr;
  IDirect3DStateBlock9* state_block = nullptr;
  UINT width = 0;
  UINT height = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  uint64_t back_buffer = 0;
  bool done_this_frame = false;

  template <typename T>
  static void SafeRelease(T*& object) {
    if (object != nullptr) {
      object->Release();
      object = nullptr;
    }
  }

  void Release() {
    SafeRelease(state_block);
    SafeRelease(vertex_declaration);
    SafeRelease(pixel_shader);
    SafeRelease(vertex_shader);
    device = nullptr;
    width = 0;
    height = 0;
    format = D3DFMT_UNKNOWN;
  }

  bool EnsureResources(IDirect3DDevice9* native_device, const D3DSURFACE_DESC& desc) {
    if (device != native_device) Release();
    device = native_device;
    if (vertex_shader == nullptr
        && FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(__scene_finish_vs.data()), &vertex_shader))) {
      return false;
    }
    if (pixel_shader == nullptr
        && FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(__scene_finish_ps.data()), &pixel_shader))) {
      return false;
    }
    if (vertex_declaration == nullptr) {
      const D3DVERTEXELEMENT9 elements[] = {
          {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
          D3DDECL_END(),
      };
      if (FAILED(device->CreateVertexDeclaration(elements, &vertex_declaration))) return false;
    }
    if (state_block == nullptr && FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_block))) return false;
    width = desc.Width;
    height = desc.Height;
    format = desc.Format;
    return true;
  }

  // Draws the scene blit in place of the original draw. Returns true when the original draw must be skipped.
  bool Run(reshade::api::device* reshade_device) {
    reshade::api::resource clone = {0u};
    renodx::utils::resource::GetResourceInfo({back_buffer}, [&](const renodx::utils::resource::ResourceInfo& info) {
      if (info.clone_enabled) clone = info.clone;
    });
    if (clone.handle == 0u) return false;

    auto* native_device = reinterpret_cast<IDirect3DDevice9*>(reshade_device->get_native());
    IDirect3DSurface9* clone_surface = GetD3D9Surface(clone);
    if (clone_surface == nullptr) return false;

    // Only when the blit targets the backbuffer (clone).
    IDirect3DSurface9* current_target = nullptr;
    native_device->GetRenderTarget(0, &current_target);
    const bool is_back_buffer = (current_target == clone_surface);
    SafeRelease(current_target);

    // Scene texture T: whatever the game bound at stage 0, or its float16 clone.
    IDirect3DBaseTexture9* bound_texture = nullptr;
    native_device->GetTexture(0, &bound_texture);
    IDirect3DBaseTexture9* scene_texture = bound_texture;
    if (bound_texture != nullptr) {
      renodx::utils::resource::GetResourceInfo(
          {reinterpret_cast<uint64_t>(bound_texture)}, [&](const renodx::utils::resource::ResourceInfo& info) {
            if (info.clone_enabled && info.clone.handle != 0u) {
              scene_texture = reinterpret_cast<IDirect3DBaseTexture9*>(info.clone.handle);
            }
          });
    }

    // Game constant c0 of the blit (brightness/contrast of the original ps_1_1).
    float game_c0[4] = {0.f, 0.f, 0.f, 0.f};
    native_device->GetPixelShaderConstantF(0, game_c0, 1);

    D3DSURFACE_DESC desc = {};
    bool drawn = false;
    if (is_back_buffer
        && scene_texture != nullptr
        && scene_texture->GetType() == D3DRTYPE_TEXTURE
        && SUCCEEDED(clone_surface->GetDesc(&desc))
        && EnsureResources(native_device, desc)
        && SUCCEEDED(state_block->Capture())) {
      const D3DVIEWPORT9 viewport = {0, 0, width, height, 0.f, 1.f};
      native_device->SetViewport(&viewport);
      native_device->SetVertexDeclaration(vertex_declaration);
      native_device->SetVertexShader(vertex_shader);
      native_device->SetPixelShader(pixel_shader);
      native_device->SetTexture(0, scene_texture);
      native_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      native_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      native_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      native_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      native_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      native_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
      native_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
      native_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      native_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
      native_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      native_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
      native_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
      native_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
      native_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
      native_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
      native_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
      native_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
      native_device->SetPixelShaderConstantF(50, reinterpret_cast<const float*>(&shader_injection), sizeof(shader_injection) / 16);
      const float params[4] = {1.f / static_cast<float>(width), 1.f / static_cast<float>(height), 0.f, 0.f};
      native_device->SetPixelShaderConstantF(49, params, 1);
      native_device->SetPixelShaderConstantF(48, game_c0, 1);
      const float vertices[4][4] = {
          {-1.f, 1.f, 0.f, 1.f},
          {1.f, 1.f, 0.f, 1.f},
          {-1.f, -1.f, 0.f, 1.f},
          {1.f, -1.f, 0.f, 1.f},
      };
      drawn = SUCCEEDED(native_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0])));
      state_block->Apply();
    }
    SafeRelease(bound_texture);
    SafeRelease(clone_surface);
    return drawn;
  }
};

SceneFinishPass scene_finish;

bool IsSceneBlit(reshade::api::command_list* cmd_list) {
  if (scene_finish.done_this_frame) return false;
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d9) return false;
  auto* state = renodx::utils::shader::GetCurrentState(cmd_list);
  if (state == nullptr) return false;
  if (renodx::utils::shader::GetCurrentPixelShaderHash(renodx::utils::shader::GetCurrentPixelState(state)) != kSceneBlitPixelShader) {
    return false;
  }
  return renodx::utils::shader::GetCurrentVertexShaderHash(state) == kSceneBlitVertexShader;
}

bool OnSceneFinishCheck(reshade::api::command_list* cmd_list) {
  if (!IsSceneBlit(cmd_list)) return false;
  scene_finish.done_this_frame = true;
  return scene_finish.Run(cmd_list->get_device());
}

bool OnSceneFinishDraw(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, uint32_t) {
  return OnSceneFinishCheck(cmd_list);
}

bool OnSceneFinishDrawIndexed(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  return OnSceneFinishCheck(cmd_list);
}

void OnSceneFinishPresent(reshade::api::command_queue*, reshade::api::swapchain* swapchain, const reshade::api::rect*,
                          const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  if (swapchain->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  scene_finish.back_buffer = swapchain->get_current_back_buffer().handle;
  scene_finish.done_this_frame = false;
}

// D3DPOOL_DEFAULT textures and state blocks must be released before IDirect3DDevice9::Reset.
void OnSceneFinishDestroySwapchain(reshade::api::swapchain* swapchain, bool) {
  if (swapchain->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  scene_finish.Release();
}

void OnSceneFinishDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  scene_finish.Release();
}

// No game shader is replaced yet: the whole frame is fixed-function plus ps_1_1 helpers.
renodx::mods::shader::CustomShaders custom_shaders = {};

float current_settings_mode = 0;

renodx::utils::settings::Settings settings = {
    // With the Display Proxy two ImGui contexts draw this menu (the real window and a narrow proxy one) and both read the
    // same mouse. This hidden, sticky entry makes the narrow copy ignore the mouse so a click only reaches one layout.
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::CUSTOM,
        .label = "mouse guard",
        .on_draw = []() {
          if (ImGui::GetWindowSize().x < 600.f) {
            ImGuiIO& io = ImGui::GetIO();
            io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
            io.MouseClicked[0] = false;
            io.MouseDoubleClicked[0] = false;
          }
          return false;
        },
        .is_sticky = true,
    },
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
        .tooltip = "Sets the tone mapper type (3D scene only). Vanilla clips like the original 8-bit image.",
        .labels = {"Vanilla", "None", "RenoDRT", "Neutwo"},
        // ACES is left out: it renders white on ps_3_0 (see clivebarkersjericho). Neutwo = 5 exists only in
        // scene_finish_ps. PsychoV was evaluated and dropped: it whitens and flattens this game's coloured highlights.
        .parse = [](float value) { return value == 2.f ? 3.f : (value == 3.f ? 5.f : value); },
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
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
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
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeBlowout",
        .binding = &shader_injection.tone_map_blowout,
        .default_value = 0.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
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
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "IntermediateDecoding",
        .binding = &shader_injection.intermediate_encoding,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Intermediate Encoding",
        .section = "Display Output",
        .labels = {"Auto", "None", "SRGB", "2.2", "2.4"},
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
}

bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX Prince of Persia: Warrior Within";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::register_event<reshade::addon_event::copy_texture_region>(OnCopyBackBufferSurface);
      reshade::register_event<reshade::addon_event::draw>(OnSceneFinishDraw);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnSceneFinishDrawIndexed);
      reshade::register_event<reshade::addon_event::present>(OnSceneFinishPresent);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnSceneFinishDestroySwapchain);
      reshade::register_event<reshade::addon_event::destroy_device>(OnSceneFinishDestroyDevice);

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;

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
          renodx::mods::swapchain::use_resize_buffer_on_demand = renodx::mods::swapchain::use_resize_buffer;
          renodx::mods::swapchain::set_color_space = !renodx::mods::swapchain::use_resize_buffer;
          shader_injection.swap_chain_encoding_color_space = is_hdr10 ? 1.f : 0.f;
          settings.push_back(setting);
        }

        // The game draws the whole scene, the HUD and the additive glow straight into the D3DFMT_A8R8G8B8 back buffer
        // (fixed-function pipeline, no intermediate scene target). Float16 targets let the glow and any overbright
        // blending accumulate above 1.0.
        // ignore_size: the end-of-frame glow chain (512 px down to 8 px) also uses b8g8r8a8 render targets; keeping
        // them float16 keeps the glow above 1.0 and keeps copies from the float16 backbuffer clone format-compatible.
        renodx::mods::swapchain::resource_upgrade_infos.push_back({
            .old_format = reshade::api::format::b8g8r8a8_unorm,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .ignore_size = true,
            .usage_include = reshade::api::resource_usage::render_target,
        });

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnCopyBackBufferSurface);
      reshade::unregister_event<reshade::addon_event::draw>(OnSceneFinishDraw);
      reshade::unregister_event<reshade::addon_event::draw_indexed>(OnSceneFinishDrawIndexed);
      reshade::unregister_event<reshade::addon_event::present>(OnSceneFinishPresent);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnSceneFinishDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnSceneFinishDestroyDevice);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
