/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <atomic>
#include <sstream>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/settings.hpp"
#include "../../utils/shader.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection;

// The final gamma pass 0x4003CC02 decodes the HDR encoding written by our composite
// replacements. Screens drawn through a composite we don't replace (e.g. menus) must not
// be decoded: then the original final shader is used for that frame.
std::atomic<bool> composite_ran_this_frame = false;
// Set once the final gamma pass has run: later full-res copies of the scene target (e.g. the
// damage "tear" overlay) see the final image, not the linear scene.
std::atomic<bool> final_pass_ran_this_frame = false;

bool OnCompositeDraw(reshade::api::command_list* cmd_list) {
  composite_ran_this_frame = true;
  return true;
}


// Heat haze: 0x35B8A99A copies the scene into the 8-bit target #2 and the haze 0xD84F5620
// draws a distorted part of it back over the scene, reading it with sRGB decoding. The
// vanilla 8-bit scene was stored sRGB-encoded; the float16 scene is linear, so the copy came
// out ~2x too dark ("black smoke"). The full-resolution copy re-encodes to sRGB in HDR mode;
// the same shader also does small 128x128 copies, which must stay untouched.
reshade::api::resource_view current_render_target = {0u};

void OnBindRenderTargets(reshade::api::command_list*, uint32_t count, const reshade::api::resource_view* rtvs,
                         reshade::api::resource_view) {
  current_render_target = count > 0 ? rtvs[0] : reshade::api::resource_view{0u};
}

bool OnSceneCopyDraw(reshade::api::command_list* cmd_list) {
  float full_res = 0.f;
  if (current_render_target.handle != 0u && !final_pass_ran_this_frame) {
    auto* device = cmd_list->get_device();
    auto res = device->get_resource_from_view(current_render_target);
    if (res.handle != 0u) {
      auto desc = device->get_resource_desc(res);
      if (desc.texture.width >= 1024) {
        // 1 = copy into an 8-bit target (heat haze), 2 = copy into the upgraded float16 scene
        // target #1 (damage "tear" refraction source, composite output).
        full_res = (desc.texture.format == reshade::api::format::r16g16b16a16_float) ? 2.f : 1.f;
      }
    }
  }
  shader_injection.copy_full_res = full_res;
  return true;
}

void RebindOriginalPixelShader(reshade::api::command_list* cmd_list) {
  auto* shader_state = renodx::utils::shader::GetCurrentState(cmd_list);
  if (shader_state == nullptr) return;
  auto* pixel_state = renodx::utils::shader::GetCurrentPixelState(shader_state);
  if (pixel_state->pipeline.handle == 0u) return;
  cmd_list->bind_pipeline(pixel_state->applied_stage, pixel_state->pipeline);
}

bool OnFinalGammaDraw(reshade::api::command_list* cmd_list) {
  final_pass_ran_this_frame = true;
  if (composite_ran_this_frame.exchange(false)) return true;
  RebindOriginalPixelShader(cmd_list);
  return false;
}

void OnPresentFrameReset(reshade::api::command_queue* queue,
                         reshade::api::swapchain* swapchain,
                         const reshade::api::rect* source_rect,
                         const reshade::api::rect* dest_rect,
                         uint32_t dirty_rect_count,
                         const reshade::api::rect* dirty_rects) {
  composite_ran_this_frame = false;
  final_pass_ran_this_frame = false;
}

// TEMPORARY: blend state of the sun-disc draw 0x795E3B26 (is it additive or alpha blended?).
// Logged to ReShade.log as "SUNBLEND ..." (first 5 draws, then every 900th).
std::atomic<uint32_t> tmp_states[256] = {};

void OnBindPipelineStatesTemp(reshade::api::command_list*, uint32_t count, const reshade::api::dynamic_state* states,
                              const uint32_t* values) {
  for (uint32_t i = 0; i < count; ++i) {
    auto st = static_cast<uint32_t>(states[i]);
    if (st < 256) tmp_states[st] = values[i];
  }
}

bool OnSunDiscDraw(reshade::api::command_list*) {
  static std::atomic<uint32_t> count = 0;
  uint32_t n = count++;
  if (n < 5 || n % 900 == 0) {
    using DS = reshade::api::dynamic_state;
    auto st = [](DS d) { return tmp_states[static_cast<uint32_t>(d)].load(); };
    std::stringstream ss;
    ss << "SUNBLEND draw " << n << " blend_enable " << st(DS::blend_enable) << " color src/dst "
       << st(DS::source_color_blend_factor) << "/" << st(DS::dest_color_blend_factor) << " alpha src/dst "
       << st(DS::source_alpha_blend_factor) << "/" << st(DS::dest_alpha_blend_factor) << " write_mask "
       << st(DS::render_target_write_mask) << " srgb_write " << st(DS::srgb_write_enable)
       << " (factors: 0 zero, 1 one, 6 src_alpha, 7 1-src_alpha)";
    reshade::log::message(reshade::log::level::info, ss.str().c_str());
  }
  return true;
}

renodx::mods::shader::CustomShaders custom_shaders = {
    CustomShaderEntryCallback(0x4003CC02, &OnFinalGammaDraw),
    CustomShaderEntryCallback(0x795E3B26, &OnSunDiscDraw),
    CustomShaderEntry(0x3848A019),
    CustomShaderEntry(0x41AE4161),
    CustomShaderEntryCallback(0x35B8A99A, &OnSceneCopyDraw),
    CustomShaderEntry(0x03AF484E),
    CustomShaderEntry(0x1CF69E72),
    CustomShaderEntry(0x773CC21F),
    CustomShaderEntryCallback(0x001F451B, &OnCompositeDraw),
    CustomShaderEntryCallback(0x04E01654, &OnCompositeDraw),
    CustomShaderEntryCallback(0x07BB9390, &OnCompositeDraw),
    CustomShaderEntryCallback(0x09F8CAF8, &OnCompositeDraw),
    CustomShaderEntryCallback(0x1ECEB053, &OnCompositeDraw),
    CustomShaderEntryCallback(0x2ED8B94F, &OnCompositeDraw),
    CustomShaderEntryCallback(0x2F3DCE6A, &OnCompositeDraw),
    CustomShaderEntryCallback(0x310E685A, &OnCompositeDraw),
    CustomShaderEntryCallback(0x34E96937, &OnCompositeDraw),
    CustomShaderEntryCallback(0x378B0930, &OnCompositeDraw),
    CustomShaderEntryCallback(0x3865D913, &OnCompositeDraw),
    CustomShaderEntryCallback(0x39D77EB2, &OnCompositeDraw),
    CustomShaderEntryCallback(0x40A30215, &OnCompositeDraw),
    CustomShaderEntryCallback(0x462A1F05, &OnCompositeDraw),
    CustomShaderEntryCallback(0x46B381C2, &OnCompositeDraw),
    CustomShaderEntryCallback(0x4761B27F, &OnCompositeDraw),
    CustomShaderEntryCallback(0x4A81411D, &OnCompositeDraw),
    CustomShaderEntryCallback(0x507DCC48, &OnCompositeDraw),
    CustomShaderEntryCallback(0x54D3D4B8, &OnCompositeDraw),
    CustomShaderEntryCallback(0x59F018C2, &OnCompositeDraw),
    CustomShaderEntryCallback(0x5F06CE55, &OnCompositeDraw),
    CustomShaderEntryCallback(0x63A564B8, &OnCompositeDraw),
    CustomShaderEntryCallback(0x6B3F60A0, &OnCompositeDraw),
    CustomShaderEntryCallback(0x7336A671, &OnCompositeDraw),
    CustomShaderEntryCallback(0x75771E7A, &OnCompositeDraw),
    CustomShaderEntryCallback(0x7C37DC48, &OnCompositeDraw),
    CustomShaderEntryCallback(0x7FA3041A, &OnCompositeDraw),
    CustomShaderEntryCallback(0x80C1F10C, &OnCompositeDraw),
    CustomShaderEntryCallback(0x81580623, &OnCompositeDraw),
    CustomShaderEntryCallback(0x8A7A7779, &OnCompositeDraw),
    CustomShaderEntryCallback(0x9209FC07, &OnCompositeDraw),
    CustomShaderEntryCallback(0x93580769, &OnCompositeDraw),
    CustomShaderEntryCallback(0x970A48F0, &OnCompositeDraw),
    CustomShaderEntryCallback(0xA17C9960, &OnCompositeDraw),
    CustomShaderEntryCallback(0xA2079816, &OnCompositeDraw),
    CustomShaderEntryCallback(0xA31546FA, &OnCompositeDraw),
    CustomShaderEntryCallback(0xAA8A7660, &OnCompositeDraw),
    CustomShaderEntryCallback(0xB10574E4, &OnCompositeDraw),
    CustomShaderEntryCallback(0xB36C5C28, &OnCompositeDraw),
    CustomShaderEntryCallback(0xB8846E56, &OnCompositeDraw),
    CustomShaderEntryCallback(0xBD80EAE0, &OnCompositeDraw),
    CustomShaderEntryCallback(0xBE36C389, &OnCompositeDraw),
    CustomShaderEntryCallback(0xC0A97E66, &OnCompositeDraw),
    CustomShaderEntryCallback(0xC353D967, &OnCompositeDraw),
    CustomShaderEntryCallback(0xC3BCCD1C, &OnCompositeDraw),
    CustomShaderEntryCallback(0xC3EDB339, &OnCompositeDraw),
    CustomShaderEntryCallback(0xC5ECA26A, &OnCompositeDraw),
    CustomShaderEntryCallback(0xC63F3B62, &OnCompositeDraw),
    CustomShaderEntryCallback(0xCD4A7D86, &OnCompositeDraw),
    CustomShaderEntryCallback(0xCF3D618F, &OnCompositeDraw),
    CustomShaderEntryCallback(0xD2C19E56, &OnCompositeDraw),
    CustomShaderEntryCallback(0xD2CD2C54, &OnCompositeDraw),
    CustomShaderEntryCallback(0xD58D386E, &OnCompositeDraw),
    CustomShaderEntryCallback(0xE20AB5D1, &OnCompositeDraw),
    CustomShaderEntryCallback(0xE4A8C037, &OnCompositeDraw),
    CustomShaderEntryCallback(0xE55FFEE5, &OnCompositeDraw),
    CustomShaderEntryCallback(0xE88DDDE7, &OnCompositeDraw),
    CustomShaderEntryCallback(0xE8CDF0C1, &OnCompositeDraw),
    CustomShaderEntryCallback(0xEA7F6AB5, &OnCompositeDraw),
    CustomShaderEntryCallback(0xEDA3CA37, &OnCompositeDraw),
    CustomShaderEntryCallback(0xEE5049A5, &OnCompositeDraw),
    CustomShaderEntryCallback(0xEFAC5450, &OnCompositeDraw),
    CustomShaderEntryCallback(0xFB9AEC46, &OnCompositeDraw),
    CustomShaderEntryCallback(0xFECF4F1D, &OnCompositeDraw),
};


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
        .tooltip = "Sets the tone mapper type. Extended = the game's grade applied to the unclipped scene (HDR from real scene values). Only the sky uses Sky HDR Boost / HDR Highlight Start; World HDR Boost is unused.",
        // ACES is not offered: it renders a white screen under DX9 / ps_3_0.
        .labels = {"Vanilla", "None", "RenoDRT", "Extended"},
        .parse = [](float value) { return value == 2.f ? 3.f : (value == 3.f ? 4.f : value); },
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
        .default_value = 0.f,
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
        .default_value = 2.f,
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
        .default_value = 0.f,
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
        .key = "FxHDRBoost",
        .binding = &shader_injection.hdr_boost,
        .default_value = 10.f,
        .label = "World HDR Boost",
        .section = "Effects",
        .tooltip = "How far above paper white the brightest parts of the world go (100 = Peak Brightness).",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxSkyHDRBoost",
        .binding = &shader_injection.sky_hdr_boost,
        .default_value = 22.f,
        .label = "Sky HDR Boost",
        .section = "Effects",
        .tooltip = "How far above paper white the brightest parts of the sky/clouds go (100 = Peak Brightness).",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxHighlightStart",
        .binding = &shader_injection.highlight_start,
        .default_value = 50.f,
        .label = "HDR Highlight Start",
        .section = "Effects",
        .tooltip = "Brightness (% of white, linear) above which the image is expanded to HDR. Below it stays vanilla.",
        .max = 95.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxSunBrightness",
        .binding = &shader_injection.sun_brightness,
        .default_value = 100.f,
        .label = "Sun Brightness",
        .section = "Effects",
        .tooltip = "Brightness of the sun disc (100 = Peak Brightness, 0 = like the rest of the image).",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxShadowLift",
        .binding = &shader_injection.shadow_lift,
        .default_value = 150.f,
        .label = "Shadow Lift",
        .section = "Effects",
        .tooltip = "Brightens dark and mid tones (like the game's brightness option); white is unchanged.",
        .max = 250.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxDepthOfField",
        .binding = &shader_injection.dof_strength,
        .default_value = 0.f,
        .label = "Depth of Field",
        .section = "Effects",
        .tooltip = "Strength of the game's depth-of-field blur (100 = vanilla, 0 = off).",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxGlow",
        .binding = &shader_injection.glow_strength,
        .default_value = 100.f,
        .label = "Glow",
        .section = "Effects",
        .tooltip = "Strength of the game's glow (bloom)",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "SunCoreShape",
        .binding = &shader_injection.sun_profile,
        .default_value = 50.f,
        .label = "Sun Core Shape",
        .section = "Sun (temporary)",
        .tooltip = "Radial brightness of the sun disc: peak at the centre fading to the rim. 0 = broad dome, 100 = small hot core.",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "SunReach",
        .binding = &shader_injection.sun_reach,
        .default_value = 200.f,
        .label = "Sun Mask Reach",
        .section = "Sun (temporary)",
        .tooltip = "How far the sun's soft brightening mask reaches beyond the disc (100 = current).",
        .max = 300.0f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "SunFalloff",
        .binding = &shader_injection.sun_falloff,
        .default_value = 75.f,
        .label = "Sun Mask Falloff",
        .section = "Sun (temporary)",
        .tooltip = "Shape of the sun mask edge (100 = current; lower = fuller and harder, higher = tighter and softer core).",
        .min = 10.f,
        .max = 300.0f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "SunHalo",
        .binding = &shader_injection.sun_halo,
        .default_value = 50.f,
        .label = "Sun Halo",
        .section = "Sun (temporary)",
        .tooltip = "Strength of a wide faint glow around the sun in the sun's own colour (0 = off = current).",
        .max = 100.0f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "SunHaloReach",
        .binding = &shader_injection.sun_halo_radius,
        .default_value = 100.f,
        .label = "Sun Halo Reach",
        .section = "Sun (temporary)",
        .tooltip = "How far the halo reaches (100 = about 12% of the screen width).",
        .max = 300.0f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxHighlightGain",
        .binding = &shader_injection.highlight_gain,
        .default_value = 100.f,
        .label = "Highlight Gain",
        .section = "Effects",
        .tooltip = "Extended tone mapper only: scales the part of the image above game white (100 = the game's own values, higher = brighter highlights). Everything below white is unchanged.",
        .max = 500.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 3.5f; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "DebugView",
        .binding = &shader_injection.debug_view,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = true,
        .label = "Debug View (temporary)",
        .section = "Debug",
        .tooltip = "Raw float16 scene before the composite's clip: 1 = display-mapped, 2 = clipped like vanilla, 3 = false-colour map of values above white, 4 = where Extended differs from vanilla below white.",
        .labels = {"Off", "Raw scene (display-mapped)", "Raw scene (clipped)", "Over-white map", "Extended vs vanilla diff", "Curve slope (R|G|B bands)", "Sun sprite value"},
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .is_visible = []() { return current_settings_mode >= 2; },
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
  renodx::utils::settings::UpdateSetting("ToneMapType", 0.f);
  renodx::utils::settings::UpdateSetting("ToneMapPeakNits", 203.f);
  renodx::utils::settings::UpdateSetting("ToneMapGameNits", 203.f);
  renodx::utils::settings::UpdateSetting("ToneMapUINits", 203.f);
  renodx::utils::settings::UpdateSetting("FxSkyHDRBoost", 0.f);
  renodx::utils::settings::UpdateSetting("FxSunBrightness", 0.f);
  renodx::utils::settings::UpdateSetting("SunHalo", 0.f);
  renodx::utils::settings::UpdateSetting("FxHighlightGain", 100.f);
  renodx::utils::settings::UpdateSetting("FxShadowLift", 0.f);
  renodx::utils::settings::UpdateSetting("FxDepthOfField", 100.f);
  renodx::utils::settings::UpdateSetting("FxGlow", 100.f);
}

bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Call of Juarez: Gunslinger";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::register_event<reshade::addon_event::present>(OnPresentFrameReset);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::register_event<reshade::addon_event::bind_pipeline_states>(OnBindPipelineStatesTemp);

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;
        // DX9: injection lives in pixel shader constants c50+ (see shared.h)
        renodx::mods::shader::constant_buffer_offset = 50 * 4;

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

        // DX9 has no HDR swap chain: always present through the D3D11 display proxy.
        renodx::mods::swapchain::use_device_proxy = true;
        renodx::mods::swapchain::set_color_space = false;

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
          renodx::mods::swapchain::device_proxy_wait_idle_destination = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        // DEBUG: global toggle (needs a game restart) to test what the upgrade changes.
        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "DebugUpgradeTargets",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 1.f,
              .label = "DEBUG: Upgrade Render Targets (restart)",
              .section = "Debug",
              .labels = {"Off", "On"},
              .is_global = true,
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          settings.push_back(setting);
          if (setting->GetValue() == 0.f) {
            reshade::log::message(reshade::log::level::info, "callofjuarezgunslinger: DEBUG render target upgrades OFF");
            initialized = true;
            break;
          }
        }

        // Full-resolution intermediates are D3DFMT_A8R8G8B8 (as in Bound in Blood). Upgrade only
        // output-sized targets: "output ratio" also catches smaller buffers that rely on
        // 8-bit clamping and tints the whole image pink.
        // Full-resolution b8g8r8a8 targets, by creation order: #0 G-buffer depth, #1 scene
        // colour (+ final gamma output), #2 G-buffer RT0 (+ composite output), #3 G-buffer
        // RT1, #4. #2 stays 8-bit: geometry/decals blend into it relying on 8-bit clamping,
        // as float16 sunlit ground blows out to white. The composite therefore writes HDR
        // into it with an invertible RGB encoding (see common.hlsli / 0x4003CC02).
        for (int i = 0; i < 32; ++i) {
          if (i == 2) continue;
          renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
              .old_format = reshade::api::format::b8g8r8a8_unorm,
              .new_format = reshade::api::format::r16g16b16a16_float,
              .index = i,
              .use_resource_view_cloning = true,
              .aspect_ratio = static_cast<float>(renodx::mods::swapchain::SwapChainUpgradeTarget::ANY),
              .usage_include = reshade::api::resource_usage::render_target,
          });
        }
        // The D3D9 back buffer is D3DFMT_X8R8G8B8.
        renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
            .old_format = reshade::api::format::b8g8r8x8_unorm,
            .new_format = reshade::api::format::r16g16b16a16_float,
            .use_resource_view_cloning = true,
            .aspect_ratio = static_cast<float>(renodx::mods::swapchain::SwapChainUpgradeTarget::ANY),
            .usage_include = reshade::api::resource_usage::render_target,
        });
        reshade::log::message(reshade::log::level::info, "callofjuarezgunslinger: upgrading b8g8r8a8/b8g8r8x8 render targets (output size)");

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::present>(OnPresentFrameReset);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::bind_pipeline_states>(OnBindPipelineStatesTemp);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
