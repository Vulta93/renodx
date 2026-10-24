/*
 * Wolfenstein II: The New Colossus - RenoDX HDR mod (Vulkan)
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

// DEBUG test mode, read from ReShade.ini at startup (missing = 2):
//   [WolfensteinTNC]
//   TestMode=<bits>
//   1 = patch B8G8R8A8 render passes to R16G16B16A16 (vkCreateRenderPass detour)
//   2 = HDR swap chain: cloned back buffer + proxy pass, HDR10 output
//   4 = scRGB output instead of HDR10 (only together with 2)
// TODO: remove before release.

#include <Windows.h>

#include <deps/imgui/imgui.h>
#include <embed/shaders.h>
#include <include/reshade.hpp>

#include <array>
#include <mutex>
#include <sstream>
#include <vector>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../templates/settings.hpp"
#include "../../utils/date.hpp"
#include "../../utils/detour.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/swapchain.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection;

// Some game layouts carry 16 bytes of their own push constants before this struct; 128 bytes is the Vulkan minimum.
static_assert(16 + sizeof(ShaderInjectData) <= 128);

constexpr int TEST_RENDER_PASS_PATCH = 1;
constexpr int TEST_HDR_SWAPCHAIN = 2;
constexpr int TEST_SCRGB = 4;
int test_mode = TEST_HDR_SWAPCHAIN;

// 0xF3E7B60A: compute, main post-process + Hable-style tonemap -> r11g11b10 (sRGB-encoded 0..1)
// 0x612E95DB: fragment, final pass (upscale + UI composite) -> swap chain
renodx::mods::shader::CustomShaders custom_shaders = {__ALL_CUSTOM_SHADERS};

renodx::utils::settings::Settings settings = renodx::templates::settings::JoinSettings({
    renodx::templates::settings::CreateDefaultSettings({
        {"ToneMapType", {.binding = &shader_injection.tone_map_type}},
        {"ToneMapPeakNits", {.binding = &shader_injection.peak_white_nits}},
        {"ToneMapGameNits", {.binding = &shader_injection.diffuse_white_nits}},
        {"ToneMapUINits", {.binding = &shader_injection.graphics_white_nits}},
        {"ToneMapGammaCorrection", {.binding = &shader_injection.gamma_correction}},
        {"ColorGradeExposure", {.binding = &shader_injection.tone_map_exposure}},
        {"ColorGradeHighlights", {.binding = &shader_injection.tone_map_highlights}},
        {"ColorGradeShadows", {.binding = &shader_injection.tone_map_shadows}},
        {"ColorGradeContrast", {.binding = &shader_injection.tone_map_contrast}},
        {"ColorGradeSaturation", {.binding = &shader_injection.tone_map_saturation}},
        {"ColorGradeHighlightSaturation", {.binding = &shader_injection.tone_map_highlight_saturation}},
        {"ColorGradeBlowout", {.binding = &shader_injection.tone_map_blowout}},
        {"ColorGradeFlare", {.binding = &shader_injection.tone_map_flare}},
    }),
    {
        new renodx::utils::settings::Setting{
            .value_type = renodx::utils::settings::SettingValueType::BUTTON,
            .label = "RenoDX Discord",
            .section = "Links",
            .group = "button-line-1",
            .tint = 0x5865F2,
            .on_change = []() {
              renodx::utils::platform::LaunchURL("https://discord.gg/", "Ce9bQHQrSV");
            },
        },
        new renodx::utils::settings::Setting{
            .value_type = renodx::utils::settings::SettingValueType::BUTTON,
            .label = "Github",
            .section = "Links",
            .group = "button-line-1",
            .on_change = []() {
              renodx::utils::platform::LaunchURL("https://github.com/clshortfuse/renodx");
            },
        },
        new renodx::utils::settings::Setting{
            .value_type = renodx::utils::settings::SettingValueType::TEXT,
            .label = std::string("Build: ") + renodx::utils::date::ISO_DATE_TIME,
            .section = "About",
        },
    },
});

void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"ToneMapType", 0.f},
      {"ToneMapPeakNits", 203.f},
      {"ToneMapGameNits", 203.f},
      {"ToneMapUINits", 203.f},
      {"ToneMapGammaCorrection", 0.f},
      {"ColorGradeExposure", 1.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"ColorGradeHighlightSaturation", 50.f},
      {"ColorGradeBlowout", 0.f},
      {"ColorGradeFlare", 0.f},
  });
}

// The game uses classic VkRenderPass objects, which bake in the attachment format. With the cloned back buffer
// in R16G16B16A16_SFLOAT, render passes that still declare B8G8R8A8 no longer match it. ReShade does not expose
// render pass creation to add-ons, so vkCreateRenderPass is detoured. Minimal Vulkan 1.0 ABI structs below.
namespace render_pass_patch {

constexpr int32_t VK_FORMAT_B8G8R8A8_UNORM = 44;
constexpr int32_t VK_FORMAT_B8G8R8A8_SRGB = 50;
constexpr int32_t VK_FORMAT_R16G16B16A16_SFLOAT = 97;

struct VkAttachmentDescriptionAbi {
  uint32_t flags;
  int32_t format;
  int32_t samples;
  int32_t load_op;
  int32_t store_op;
  int32_t stencil_load_op;
  int32_t stencil_store_op;
  int32_t initial_layout;
  int32_t final_layout;
};

struct VkRenderPassCreateInfoAbi {
  int32_t s_type;
  const void* p_next;
  uint32_t flags;
  uint32_t attachment_count;
  const VkAttachmentDescriptionAbi* p_attachments;
  uint32_t subpass_count;
  const void* p_subpasses;
  uint32_t dependency_count;
  const void* p_dependencies;
};

using PFN_CreateRenderPass = int32_t(__stdcall*)(void* device, const VkRenderPassCreateInfoAbi* info, const void* allocator, uint64_t* render_pass);
using PFN_GetDeviceProcAddr = void*(__stdcall*)(void* device, const char* name);

PFN_CreateRenderPass original_create_render_pass = nullptr;
std::once_flag install_once;
uint32_t patched_count = 0;

int32_t __stdcall HookCreateRenderPass(void* device, const VkRenderPassCreateInfoAbi* info, const void* allocator, uint64_t* render_pass) {
  if (info == nullptr || info->p_attachments == nullptr || info->attachment_count == 0) {
    return original_create_render_pass(device, info, allocator, render_pass);
  }

  std::vector<VkAttachmentDescriptionAbi> attachments(info->p_attachments, info->p_attachments + info->attachment_count);
  bool needs_patch = false;
  for (auto& attachment : attachments) {
    if (attachment.format == VK_FORMAT_B8G8R8A8_UNORM || attachment.format == VK_FORMAT_B8G8R8A8_SRGB) {
      attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      needs_patch = true;
    }
  }
  if (!needs_patch) return original_create_render_pass(device, info, allocator, render_pass);

  VkRenderPassCreateInfoAbi new_info = *info;
  new_info.p_attachments = attachments.data();
  if (patched_count < 32) {
    std::stringstream s;
    s << "wolfensteintnc::HookCreateRenderPass(patched B8G8R8A8 -> R16G16B16A16_SFLOAT, attachments: "
      << info->attachment_count << ", final_layout[0]: " << info->p_attachments[0].final_layout << ")";
    reshade::log::message(reshade::log::level::info, s.str().c_str());
  }
  ++patched_count;
  return original_create_render_pass(device, &new_info, allocator, render_pass);
}

void Install(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::vulkan) return;
  std::call_once(install_once, [device]() {
    HMODULE vulkan_module = GetModuleHandleW(L"vulkan-1.dll");
    if (vulkan_module == nullptr) {
      reshade::log::message(reshade::log::level::error, "wolfensteintnc::render_pass_patch(vulkan-1.dll not loaded)");
      return;
    }
    auto get_device_proc_addr = reinterpret_cast<PFN_GetDeviceProcAddr>(GetProcAddress(vulkan_module, "vkGetDeviceProcAddr"));
    if (get_device_proc_addr == nullptr) return;
    original_create_render_pass = reinterpret_cast<PFN_CreateRenderPass>(
        get_device_proc_addr(reinterpret_cast<void*>(device->get_native()), "vkCreateRenderPass"));
    if (original_create_render_pass == nullptr) return;

    try {
      renodx::utils::detour::Install(std::array{renodx::utils::detour::Function(&original_create_render_pass, HookCreateRenderPass)});
      reshade::log::message(reshade::log::level::info, "wolfensteintnc::render_pass_patch(installed vkCreateRenderPass detour)");
    } catch (const std::exception& e) {
      reshade::log::message(reshade::log::level::error, e.what());
    }
  });
}

// Earliest game-driven events after vkCreateDevice has fully returned.
void OnInitSwapchainInstall(reshade::api::swapchain* swapchain, bool resize) { Install(swapchain->get_device()); }
void OnInitPipelineLayoutInstall(reshade::api::device* device, uint32_t, const reshade::api::pipeline_layout_param*, reshade::api::pipeline_layout) {
  Install(device);
}

}  // namespace render_pass_patch

bool fired_on_init_swapchain = false;

// Default Peak/Game brightness from the display's reported capabilities.
void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  if (fired_on_init_swapchain) return;

  auto peak = renodx::utils::swapchain::GetPeakNits(swapchain);
  settings[2]->can_reset = true;
  settings[2]->default_value = peak.has_value() ? roundf(peak.value()) : 1000.f;
  settings[3]->default_value = fmin(renodx::utils::swapchain::ComputeReferenceWhite(settings[2]->default_value), 203.f);
  fired_on_init_swapchain = true;
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Wolfenstein II: The New Colossus";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);

      reshade::get_config_value(nullptr, "WolfensteinTNC", "TestMode", test_mode);
      {
        std::stringstream s;
        s << "wolfensteintnc(TestMode: " << test_mode << ", custom shaders: " << custom_shaders.size() << ")";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }

      // The game's own push constants stay at offset 0; RenoDX data is appended after them.
      renodx::mods::shader::allow_multiple_push_constants = true;

      if ((test_mode & TEST_RENDER_PASS_PATCH) != 0) {
        reshade::register_event<reshade::addon_event::init_swapchain>(render_pass_patch::OnInitSwapchainInstall);
        reshade::register_event<reshade::addon_event::init_pipeline_layout>(render_pass_patch::OnInitPipelineLayoutInstall);
      }

      if ((test_mode & TEST_HDR_SWAPCHAIN) != 0) {
        shader_injection.swap_chain_output_preset = ((test_mode & TEST_SCRGB) != 0 ? 2.f : 1.f);
        renodx::mods::swapchain::use_resource_cloning = true;
        renodx::mods::swapchain::swapchain_proxy_compatibility_mode = false;
        renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader;
        renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader;
        renodx::mods::swapchain::SetUseHDR10((test_mode & TEST_SCRGB) == 0);
      }
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_swapchain>(render_pass_patch::OnInitSwapchainInstall);
      reshade::unregister_event<reshade::addon_event::init_pipeline_layout>(render_pass_patch::OnInitPipelineLayoutInstall);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  if ((test_mode & TEST_HDR_SWAPCHAIN) != 0) {
    renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
