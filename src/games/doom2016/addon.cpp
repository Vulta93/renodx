/*
 * DOOM (2016) - RenoDX HDR mod (Vulkan)
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

// DEBUG test mode, read from ReShade.ini at startup:
//   [DOOM2016]
//   TestMode=<bits>   (missing = 7 = normal mod)
//   1 = replace tonemap 0xF600527E        2 = replace final 0x49CBC37F
//   4 = HDR swap chain upgrade             8 = swap in the ORIGINAL game SPIR-V (identity test)
//  16 = no RenoDX push constants (only valid together with 8)
// Shaders are swapped at vkCreateShaderModule (see shader_module_patch) instead of through
// ReShade's pipeline re-creation, which crashes DOOM (GPU device lost) even with identical code.
// TODO: remove before release.

#include <Windows.h>

#include <detours.h>

#include <cstring>
#include <iomanip>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <vector>

#include <deps/imgui/imgui.h>
#include <embed/shaders.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../templates/settings.hpp"
#include "../../utils/date.hpp"
#include "../../utils/hash.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/swapchain.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection;

// 0xF600527E: main post-process + filmic tonemap (writes r11g11b10 scene)
// 0x49CBC37F: final pass (upscale + UI composite) -> swap chain
renodx::mods::shader::CustomShaders custom_shaders = {};

constexpr int TEST_TONEMAP = 1;
constexpr int TEST_FINAL = 2;
constexpr int TEST_SWAPCHAIN = 4;
constexpr int TEST_ORIGINAL_SPIRV = 8;
constexpr int TEST_NO_INJECTION = 16;
int test_mode = TEST_TONEMAP | TEST_FINAL | TEST_SWAPCHAIN;

// original game shader hash -> SPIR-V words we swap in at vkCreateShaderModule
std::unordered_map<uint32_t, std::vector<uint32_t>> module_swaps;

std::vector<uint32_t> ToWords(std::span<const uint8_t> bytes) {
  std::vector<uint32_t> words((bytes.size() + 3) / 4, 0u);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

void AddSwap(uint32_t original_hash, std::span<const uint8_t> code) {
  module_swaps[original_hash] = ToWords(code);
  // ReShade will see the swapped code, so RenoDX tracks the pipeline by the NEW code's hash.
  // Empty code = no pipeline re-creation; RenoDX only pushes the settings (push constants).
  const uint32_t new_hash = renodx::utils::hash::ComputeCRC32(code.data(), code.size());
  custom_shaders[new_hash] = renodx::mods::shader::CreateCustomShader(new_hash, {});
}

void BuildCustomShaders() {
  custom_shaders.clear();
  module_swaps.clear();
  const bool original = (test_mode & TEST_ORIGINAL_SPIRV) != 0;
  if ((test_mode & TEST_TONEMAP) != 0) {
    AddSwap(0xF600527E, original ? __original_tonemap : __0xF600527E);
  }
  if ((test_mode & TEST_FINAL) != 0) {
    AddSwap(0x49CBC37F, original ? __original_final : __0x49CBC37F);
  }
}

renodx::utils::settings::Settings settings = renodx::templates::settings::JoinSettings({
    renodx::templates::settings::CreateDefaultSettings({
        {"ToneMapType", {
                            .binding = &shader_injection.tone_map_type,
                            .labels = {"Vanilla", "RenoDRT", "PsychoV-17", "PsychoV-30"},
                            // shader values: 0 = vanilla, 3 = RenoDRT, 10/11 = PsychoV (see shared.h)
                            .parse = [](float value) {
                              if (value >= 3.f) return 11.f;
                              if (value >= 2.f) return 10.f;
                              return value * 3.f;
                            },
                        }},
        {"ToneMapPeakNits", {.binding = &shader_injection.peak_white_nits}},
        {"ToneMapGameNits", {.binding = &shader_injection.diffuse_white_nits}},
        {"ToneMapUINits", {.binding = &shader_injection.graphics_white_nits}},
        {"ToneMapGammaCorrection", {.binding = &shader_injection.gamma_correction}},
        {"ToneMapHueCorrection", {.binding = &shader_injection.tone_map_hue_correction}},
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
            .key = "ColorGradeScene",
            .binding = &shader_injection.scene_grade_strength,
            .default_value = 100.f,
            .label = "Scene Grading",
            .section = "Color Grading",
            .tooltip = "How much of the game's own filmic look is kept.",
            .max = 100.f,
            .parse = [](float value) { return value * 0.01f; },
        },
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
            .label = "- Vulkan renderer only (DOOMx64vk.exe).\n"
                     "- Leave the in-game Brightness at its default.",
            .section = "About",
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
      {"ToneMapHueCorrection", 0.f},
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

// ---------------------------------------------------------------------------
// Vulkan render pass patch
// DOOM uses classic VkRenderPass objects, which bake in the attachment format.
// When the swap chain is upgraded to R16G16B16A16_SFLOAT, render passes that still
// declare B8G8R8A8 no longer match the swap chain images (invalid usage -> device lost).
// ReShade doesn't expose render pass creation to add-ons, so we detour
// vkCreateRenderPass and swap B8G8R8A8 colour attachments (only the swap chain uses
// that format in DOOM) to R16G16B16A16_SFLOAT. Minimal Vulkan 1.0 ABI structs below.
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
bool patch_render_passes = false;

struct VkShaderModuleCreateInfoAbi {
  int32_t s_type;
  const void* p_next;
  uint32_t flags;
  size_t code_size;
  const uint32_t* p_code;
};
using PFN_CreateShaderModule = int32_t(__stdcall*)(void* device, const VkShaderModuleCreateInfoAbi* info, const void* allocator, uint64_t* shader_module);
PFN_CreateShaderModule original_create_shader_module = nullptr;

int32_t __stdcall HookCreateShaderModule(void* device, const VkShaderModuleCreateInfoAbi* info, const void* allocator, uint64_t* shader_module) {
  if (info != nullptr && info->p_code != nullptr && info->code_size != 0 && !module_swaps.empty()) {
    const uint32_t hash = renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(info->p_code), info->code_size);
    if (auto it = module_swaps.find(hash); it != module_swaps.end()) {
      VkShaderModuleCreateInfoAbi new_info = *info;
      new_info.p_code = it->second.data();
      new_info.code_size = it->second.size() * sizeof(uint32_t);
      std::stringstream s;
      s << "doom2016::HookCreateShaderModule(swapped " << PRINT_CRC32(hash) << ", " << info->code_size << " -> " << new_info.code_size << " bytes)";
      reshade::log::message(reshade::log::level::info, s.str().c_str());
      return original_create_shader_module(device, &new_info, allocator, shader_module);
    }
  }
  return original_create_shader_module(device, info, allocator, shader_module);
}
std::once_flag install_once;
int patched_count = 0;

int32_t __stdcall HookCreateRenderPass(void* device, const VkRenderPassCreateInfoAbi* info, const void* allocator, uint64_t* render_pass) {
  if (info != nullptr && info->p_attachments != nullptr && info->attachment_count != 0) {
    bool needs_patch = false;
    for (uint32_t i = 0; i < info->attachment_count; ++i) {
      const int32_t format = info->p_attachments[i].format;
      if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) needs_patch = true;
    }
    if (needs_patch) {
      std::vector<VkAttachmentDescriptionAbi> attachments(info->p_attachments, info->p_attachments + info->attachment_count);
      for (auto& attachment : attachments) {
        if (attachment.format == VK_FORMAT_B8G8R8A8_UNORM || attachment.format == VK_FORMAT_B8G8R8A8_SRGB) {
          attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        }
      }
      VkRenderPassCreateInfoAbi new_info = *info;
      new_info.p_attachments = attachments.data();
      if (patched_count < 32) {
        std::stringstream s;
        s << "doom2016::HookCreateRenderPass(patched B8G8R8A8 -> R16G16B16A16_SFLOAT, attachments: "
          << info->attachment_count << ", final_layout[0]: " << info->p_attachments[0].final_layout << ")";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }
      ++patched_count;
      return original_create_render_pass(device, &new_info, allocator, render_pass);
    }
  }
  return original_create_render_pass(device, info, allocator, render_pass);
}

void Install(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::vulkan) return;
  std::call_once(install_once, [device]() {
    HMODULE vulkan_module = GetModuleHandleW(L"vulkan-1.dll");
    if (vulkan_module == nullptr) {
      reshade::log::message(reshade::log::level::error, "doom2016::render_pass_patch(vulkan-1.dll not loaded)");
      return;
    }
    auto get_device_proc_addr = reinterpret_cast<PFN_GetDeviceProcAddr>(GetProcAddress(vulkan_module, "vkGetDeviceProcAddr"));
    if (get_device_proc_addr == nullptr) return;
    void* native_device = reinterpret_cast<void*>(device->get_native());
    original_create_shader_module = reinterpret_cast<PFN_CreateShaderModule>(get_device_proc_addr(native_device, "vkCreateShaderModule"));
    if (patch_render_passes) {
      original_create_render_pass = reinterpret_cast<PFN_CreateRenderPass>(get_device_proc_addr(native_device, "vkCreateRenderPass"));
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (original_create_shader_module != nullptr) {
      DetourAttach(reinterpret_cast<PVOID*>(&original_create_shader_module), reinterpret_cast<PVOID>(&HookCreateShaderModule));
    }
    if (original_create_render_pass != nullptr) {
      DetourAttach(reinterpret_cast<PVOID*>(&original_create_render_pass), reinterpret_cast<PVOID>(&HookCreateRenderPass));
    }
    const LONG result = DetourTransactionCommit();
    std::stringstream s;
    s << "doom2016::render_pass_patch(installed detours, shader module: " << (original_create_shader_module != nullptr)
      << ", render pass: " << (original_create_render_pass != nullptr) << ", result: " << result << ")";
    reshade::log::message(result == NO_ERROR ? reshade::log::level::info : reshade::log::level::error, s.str().c_str());
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
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for DOOM (2016)";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);

      reshade::get_config_value(nullptr, "DOOM2016", "TestMode", test_mode);
      BuildCustomShaders();
      {
        std::stringstream s;
        s << "doom2016(TestMode: " << test_mode << ", custom shaders: " << custom_shaders.size() << ")";
        reshade::log::message(reshade::log::level::info, s.str().c_str());
      }

      // Vulkan: RenoDX data travels as push constants.
      renodx::mods::shader::allow_multiple_push_constants = true;

      reshade::register_event<reshade::addon_event::init_swapchain>(render_pass_patch::OnInitSwapchainInstall);
      reshade::register_event<reshade::addon_event::init_pipeline_layout>(render_pass_patch::OnInitPipelineLayoutInstall);

      if ((test_mode & TEST_SWAPCHAIN) != 0) {
      render_pass_patch::patch_render_passes = true;

      // Swap chain: 8-bit sRGB -> 16-bit float scRGB. The game renders into a clone of the
      // back buffer; the proxy pass (swap_chain_proxy_*.slang) converts it for the display.
      renodx::mods::swapchain::target_format = reshade::api::format::r16g16b16a16_float;
      renodx::mods::swapchain::use_resource_cloning = true;
      renodx::mods::swapchain::swapchain_proxy_compatibility_mode = false;
      renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader;
      renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader;
      }
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(render_pass_patch::OnInitSwapchainInstall);
      reshade::unregister_event<reshade::addon_event::init_pipeline_layout>(render_pass_patch::OnInitPipelineLayoutInstall);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  if ((test_mode & TEST_SWAPCHAIN) != 0) {
    renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  }
  if ((test_mode & TEST_NO_INJECTION) != 0) {
    renodx::mods::shader::Use(fdw_reason, custom_shaders, static_cast<ShaderInjectData*>(nullptr));
  } else {
    renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);
  }

  return TRUE;
}
