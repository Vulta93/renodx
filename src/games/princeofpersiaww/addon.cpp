/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <d3d9.h>

#include <algorithm>
#include <cfloat>
#include <iterator>

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


// ---- Scene passes (graded SDR bridge around the game's own post effects) ----
// Frame order (Devkit snapshots, gameplay): 3D scene, post effects and glow A -> render-target texture T; scene blit
// T -> backbuffer (vs 0x344B89F7, ps_1_1 0x2059E26C: r0 = t0 * c0 + (t0 - c0)); copy backbuffer -> T; glow B / blur
// (ps_1_1 0x5EA85978, 0xCC6D19AE, composite 0x3D277874) into T; HUD into T; copy T -> backbuffer.
// Glow B / blur needs values <= 1.0 (its ps_1_1 shaders clamp and its blending relies on it: unclamped, the image
// blurred and blew out). So:
//   pass 1, in place of the blit: save T (HDR scene) aside, write the SDR scene (scene_sdr_ps) to the backbuffer;
//   pass 2, at the first draw after glow B (or at the final copy): rebuild HDR on top of the game's SDR result and
//           tone map it (scene_finish_ps), writing back into T. The HUD drawn afterwards stays at UI Brightness.
// Native D3D9 calls; the game's state is saved and restored with a state block, render target 0 separately.
constexpr uint32_t SCENE_BLIT_VERTEX_SHADER = 0x344B89F7;
constexpr uint32_t SCENE_BLIT_PIXEL_SHADER = 0x2059E26C;
constexpr uint32_t GLOW_B_PIXEL_SHADERS[] = {0x5EA85978, 0xCC6D19AE, 0x3D277874};

template <typename T>
void SafeRelease(T** object) {
  if (*object != nullptr) {
    (*object)->Release();
    *object = nullptr;
  }
}

struct ScenePassState {
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexShader9* vertex_shader = nullptr;
  IDirect3DPixelShader9* sdr_pixel_shader = nullptr;
  IDirect3DPixelShader9* finish_pixel_shader = nullptr;
  IDirect3DVertexDeclaration9* vertex_declaration = nullptr;
  IDirect3DStateBlock9* state_block = nullptr;
  IDirect3DTexture9* hdr_scene = nullptr;
  IDirect3DTexture9* graded_copy = nullptr;
  uint64_t back_buffer = 0;
  float game_c0[4] = {0.f, 0.f, 0.f, 0.f};
  bool done_this_frame = false;
  bool finish_pending = false;

  // Device-owned objects must be released before IDirect3DDevice9::Reset and on device destruction.
  void Release() {
    SafeRelease(&graded_copy);
    SafeRelease(&hdr_scene);
    SafeRelease(&state_block);
    SafeRelease(&vertex_declaration);
    SafeRelease(&finish_pixel_shader);
    SafeRelease(&sdr_pixel_shader);
    SafeRelease(&vertex_shader);
    device = nullptr;
    finish_pending = false;
  }
};

ScenePassState scene_pass;


// Float16 render-target texture matching `desc` (recreated when the size changes).
bool EnsureSceneTexture(IDirect3DDevice9* native_device, const D3DSURFACE_DESC& desc, IDirect3DTexture9** texture) {
  if (*texture != nullptr) {
    D3DSURFACE_DESC current_desc = {};
    (*texture)->GetLevelDesc(0, &current_desc);
    if (current_desc.Width == desc.Width && current_desc.Height == desc.Height) return true;
    SafeRelease(texture);
  }
  return SUCCEEDED(native_device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F,
                                                D3DPOOL_DEFAULT, texture, nullptr));
}

bool CopySurface(IDirect3DDevice9* native_device, IDirect3DSurface9* source, IDirect3DTexture9* dest_texture) {
  IDirect3DSurface9* dest_surface = nullptr;
  if (FAILED(dest_texture->GetSurfaceLevel(0, &dest_surface))) return false;
  const bool copied = SUCCEEDED(native_device->StretchRect(source, nullptr, dest_surface, nullptr, D3DTEXF_POINT));
  SafeRelease(&dest_surface);
  return copied;
}

// Full-screen draw of `pixel_shader` into `target` (s0, s1 = point-sampled inputs), restoring the game's state.
bool DrawScenePass(IDirect3DDevice9* native_device, IDirect3DSurface9* target, IDirect3DPixelShader9* pixel_shader,
                   IDirect3DBaseTexture9* input0, IDirect3DBaseTexture9* input1) {
  D3DSURFACE_DESC target_desc = {};
  if (FAILED(target->GetDesc(&target_desc))) return false;
  if (FAILED(scene_pass.state_block->Capture())) return false;
  IDirect3DSurface9* previous_target = nullptr;
  native_device->GetRenderTarget(0, &previous_target);
  native_device->SetRenderTarget(0, target);

  const D3DVIEWPORT9 viewport = {0, 0, target_desc.Width, target_desc.Height, 0.f, 1.f};
  native_device->SetViewport(&viewport);
  native_device->SetVertexDeclaration(scene_pass.vertex_declaration);
  native_device->SetVertexShader(scene_pass.vertex_shader);
  native_device->SetPixelShader(pixel_shader);
  IDirect3DBaseTexture9* const inputs[2] = {input0, input1};
  for (DWORD stage = 0; stage < 2; ++stage) {
    native_device->SetTexture(stage, inputs[stage]);
    native_device->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    native_device->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    native_device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    native_device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    native_device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    native_device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
  }
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
  const float inverse_size[4] = {1.f / static_cast<float>(target_desc.Width), 1.f / static_cast<float>(target_desc.Height), 0.f, 0.f};
  native_device->SetPixelShaderConstantF(49, inverse_size, 1);
  native_device->SetPixelShaderConstantF(48, scene_pass.game_c0, 1);
  const float vertices[4][4] = {
      {-1.f, 1.f, 0.f, 1.f},
      {1.f, 1.f, 0.f, 1.f},
      {-1.f, -1.f, 0.f, 1.f},
      {1.f, -1.f, 0.f, 1.f},
  };
  const bool drawn = SUCCEEDED(native_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0])));

  native_device->SetRenderTarget(0, previous_target);
  SafeRelease(&previous_target);
  scene_pass.state_block->Apply();
  return drawn;
}

// Pass 2 into `scene_target` (T's float16 clone surface): HDR rebuilt on top of the game's SDR result.
// Only runs when `scene_target` is a float16 surface of the saved scene's size; otherwise it stays pending.
void RunSceneFinish(IDirect3DDevice9* native_device, IDirect3DSurface9* scene_target) {
  if (scene_pass.hdr_scene == nullptr) return;
  D3DSURFACE_DESC desc = {};
  D3DSURFACE_DESC hdr_desc = {};
  if (FAILED(scene_target->GetDesc(&desc)) || FAILED(scene_pass.hdr_scene->GetLevelDesc(0, &hdr_desc))) return;
  if (desc.Width != hdr_desc.Width || desc.Height != hdr_desc.Height || desc.Format != D3DFMT_A16B16G16R16F) return;
  scene_pass.finish_pending = false;
  if (!EnsureSceneTexture(native_device, desc, &scene_pass.graded_copy)) return;
  if (!CopySurface(native_device, scene_target, scene_pass.graded_copy)) return;
  DrawScenePass(native_device, scene_target, scene_pass.finish_pixel_shader, scene_pass.graded_copy, scene_pass.hdr_scene);
}

// Returns true when the addon drew the scene blit itself and the original draw must be skipped.
bool OnScenePassCheck(reshade::api::command_list* cmd_list) {
  auto* reshade_device = cmd_list->get_device();
  if (reshade_device->get_api() != reshade::api::device_api::d3d9) return false;
  auto* shader_state = renodx::utils::shader::GetCurrentState(cmd_list);
  if (shader_state == nullptr) return false;
  const uint32_t pixel_shader_hash =
      renodx::utils::shader::GetCurrentPixelShaderHash(renodx::utils::shader::GetCurrentPixelState(shader_state));
  auto* native_device = reinterpret_cast<IDirect3DDevice9*>(reshade_device->get_native());

  if (scene_pass.finish_pending) {
    if (std::ranges::find(GLOW_B_PIXEL_SHADERS, pixel_shader_hash) != std::end(GLOW_B_PIXEL_SHADERS)) return false;
    // First draw after glow B (the HUD, into T): finish the scene in the current render target.
    IDirect3DSurface9* current_target = nullptr;
    native_device->GetRenderTarget(0, &current_target);
    if (current_target != nullptr) {
      RunSceneFinish(native_device, current_target);
    }
    SafeRelease(&current_target);
    return false;
  }

  if (scene_pass.done_this_frame) return false;
  if (pixel_shader_hash != SCENE_BLIT_PIXEL_SHADER) return false;
  if (renodx::utils::shader::GetCurrentVertexShaderHash(shader_state) != SCENE_BLIT_VERTEX_SHADER) return false;
  scene_pass.done_this_frame = true;

  reshade::api::resource back_buffer_clone = {0u};
  renodx::utils::resource::GetResourceInfo({scene_pass.back_buffer}, [&](const renodx::utils::resource::ResourceInfo& info) {
    if (info.clone_enabled) back_buffer_clone = info.clone;
  });
  if (back_buffer_clone.handle == 0u) return false;
  IDirect3DSurface9* clone_surface = GetD3D9Surface(back_buffer_clone);
  if (clone_surface == nullptr) return false;

  // Only when the blit targets the backbuffer (clone).
  IDirect3DSurface9* current_target = nullptr;
  native_device->GetRenderTarget(0, &current_target);
  const bool targets_back_buffer = (current_target == clone_surface);
  SafeRelease(&current_target);

  // Scene texture T: the texture the game bound at stage 0, or its float16 clone.
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
  native_device->GetPixelShaderConstantF(0, scene_pass.game_c0, 1);

  if (scene_pass.device != native_device) {
    scene_pass.Release();
    scene_pass.device = native_device;
  }
  if (scene_pass.vertex_shader == nullptr) {
    native_device->CreateVertexShader(reinterpret_cast<const DWORD*>(__scene_finish_vs.data()), &scene_pass.vertex_shader);
  }
  if (scene_pass.sdr_pixel_shader == nullptr) {
    native_device->CreatePixelShader(reinterpret_cast<const DWORD*>(__scene_sdr_ps.data()), &scene_pass.sdr_pixel_shader);
  }
  if (scene_pass.finish_pixel_shader == nullptr) {
    native_device->CreatePixelShader(reinterpret_cast<const DWORD*>(__scene_finish_ps.data()), &scene_pass.finish_pixel_shader);
  }
  if (scene_pass.vertex_declaration == nullptr) {
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END(),
    };
    native_device->CreateVertexDeclaration(elements, &scene_pass.vertex_declaration);
  }
  if (scene_pass.state_block == nullptr) {
    native_device->CreateStateBlock(D3DSBT_ALL, &scene_pass.state_block);
  }

  bool drawn = false;
  IDirect3DSurface9* scene_surface = nullptr;
  D3DSURFACE_DESC scene_desc = {};
  if (targets_back_buffer
      && scene_texture != nullptr
      && scene_texture->GetType() == D3DRTYPE_TEXTURE
      && scene_pass.vertex_shader != nullptr
      && scene_pass.sdr_pixel_shader != nullptr
      && scene_pass.finish_pixel_shader != nullptr
      && scene_pass.vertex_declaration != nullptr
      && scene_pass.state_block != nullptr
      && SUCCEEDED(static_cast<IDirect3DTexture9*>(scene_texture)->GetSurfaceLevel(0, &scene_surface))
      && SUCCEEDED(scene_surface->GetDesc(&scene_desc))
      && EnsureSceneTexture(native_device, scene_desc, &scene_pass.hdr_scene)
      && CopySurface(native_device, scene_surface, scene_pass.hdr_scene)) {
    drawn = DrawScenePass(native_device, clone_surface, scene_pass.sdr_pixel_shader, scene_texture, nullptr);
    scene_pass.finish_pending = drawn;
  }
  SafeRelease(&scene_surface);
  SafeRelease(&bound_texture);
  SafeRelease(&clone_surface);
  return drawn;
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
  bool source_is_back_buffer = false;
  bool dest_is_back_buffer = false;
  auto resolve = [](reshade::api::resource resource, bool* is_back_buffer) {
    reshade::api::resource target = resource;
    renodx::utils::resource::GetResourceInfo(resource, [&](const renodx::utils::resource::ResourceInfo& info) {
      if (!info.clone_enabled || info.clone.handle == 0u) return;
      target = info.clone;
      *is_back_buffer = (info.desc.type == reshade::api::resource_type::surface);
    });
    return target;
  };
  const reshade::api::resource source_target = resolve(source, &source_is_back_buffer);
  const reshade::api::resource dest_target = resolve(dest, &dest_is_back_buffer);
  if (!source_is_back_buffer && !dest_is_back_buffer) return false;

  IDirect3DSurface9* src_surface = GetD3D9Surface(source_target);
  IDirect3DSurface9* dst_surface = GetD3D9Surface(dest_target);
  bool handled = false;
  if (src_surface != nullptr && dst_surface != nullptr) {
    auto* native_device = reinterpret_cast<IDirect3DDevice9*>(device->get_native());
    // Final copy T -> backbuffer without a draw after glow B (no HUD): finish the scene now.
    if (scene_pass.finish_pending && dest_is_back_buffer && !source_is_back_buffer) {
      RunSceneFinish(native_device, src_surface);
    }
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

bool OnScenePassDraw(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, uint32_t) {
  return OnScenePassCheck(cmd_list);
}

bool OnScenePassDrawIndexed(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  return OnScenePassCheck(cmd_list);
}

void OnScenePassPresent(reshade::api::command_queue*, reshade::api::swapchain* swapchain, const reshade::api::rect*,
                          const reshade::api::rect*, uint32_t, const reshade::api::rect*) {
  if (swapchain->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  scene_pass.back_buffer = swapchain->get_current_back_buffer().handle;
  scene_pass.done_this_frame = false;
  scene_pass.finish_pending = false;
}

void OnScenePassDestroySwapchain(reshade::api::swapchain* swapchain, bool) {
  if (swapchain->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  scene_pass.Release();
}

void OnScenePassDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  scene_pass.Release();
}

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
        // scene_finish_ps. No PsychoV: the game has no tone curve to match (hard clip at 8-bit targets).
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
      reshade::register_event<reshade::addon_event::draw>(OnScenePassDraw);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnScenePassDrawIndexed);
      reshade::register_event<reshade::addon_event::present>(OnScenePassPresent);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnScenePassDestroySwapchain);
      reshade::register_event<reshade::addon_event::destroy_device>(OnScenePassDestroyDevice);

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

        // The scene render target (3840x2160) and the glow chain targets (512 px down to 8 px) are D3DFMT_A8R8G8B8,
        // which clipped every blend at 1.0. Float16 keeps the game's own above-white blending (fires, lanterns, lit
        // decals: up to ~2x encoded measured before the glow) and keeps StretchRect copies between them and the
        // float16 backbuffer clone format-compatible. ignore_size covers the differently sized glow targets.
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
      reshade::unregister_event<reshade::addon_event::draw>(OnScenePassDraw);
      reshade::unregister_event<reshade::addon_event::draw_indexed>(OnScenePassDrawIndexed);
      reshade::unregister_event<reshade::addon_event::present>(OnScenePassPresent);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnScenePassDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnScenePassDestroyDevice);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
