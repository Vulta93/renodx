/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../../utils/mcp/types.hpp"
#include "resource_view_summary.hpp"

namespace renodx::addons::devkit::mcp::draw_summary {

using json = renodx::utils::mcp::json;

struct ResourceBindSummary {
  std::string type;
  std::uint32_t slot = 0u;
  std::uint32_t space = 0u;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const ResourceBindSummary& value) {
  j = {
      {"type", value.type},
      {"slot", value.slot},
      {"space", value.space},
  };
}

struct SlotResourceBindingSummary {
  std::uint32_t slot = 0u;
  std::uint32_t space = 0u;
  bool used_by_active_shader = false;
  resource_view_summary::ResourceViewSummary resource_view;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const SlotResourceBindingSummary& value) {
  j = value.resource_view;
  j["slot"] = value.slot;
  j["space"] = value.space;
  j["usedByActiveShader"] = value.used_by_active_shader;
}

// A shader variable over values[index, index + count) of the floatValues/uintValues it labels.
struct ValueNameSummary {
  std::string name;
  std::uint32_t index = 0u;
  std::uint32_t count = 0u;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const ValueNameSummary& value) {
  j = {
      {"name", value.name},
      {"index", value.index},
      {"count", value.count},
  };
}

// Words [index, index + count) of floatValues/uintValues.
struct ValueRangeSummary {
  std::uint32_t index = 0u;
  std::uint32_t count = 0u;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const ValueRangeSummary& value) {
  j = {
      {"index", value.index},
      {"count", value.count},
  };
}

// A shader variable with no captured value: its absolute first word (in firstValue's numbering) and, when the
// reflection sizes it, its count in words.
struct ValueWordsSummary {
  std::string name;
  std::uint32_t word = 0u;
  std::optional<std::uint32_t> count = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const ValueWordsSummary& value) {
  j = {
      {"name", value.name},
      {"word", value.word},
  };
  if (value.count.has_value()) {
    j["count"] = value.count.value();
  }
}

struct ConstantBufferSummary {
  std::uint32_t slot = 0u;
  std::uint32_t space = 0u;
  std::string buffer_handle;
  std::uint64_t offset = 0u;
  std::optional<std::uint64_t> size = std::nullopt;
  bool full_range = false;
  bool used_by_active_shader = false;
  // Only with includeConstantValues: the bytes the draw saw, as 32-bit words read as float and as uint.
  std::optional<std::string> values_source = std::nullopt;
  std::optional<std::vector<float>> float_values = std::nullopt;
  std::optional<std::vector<std::uint32_t>> uint_values = std::nullopt;
  bool values_truncated = false;
  std::optional<std::vector<ValueNameSummary>> value_names = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const ConstantBufferSummary& value) {
  j = {
      {"slot", value.slot},
      {"space", value.space},
      {"bufferHandle", value.buffer_handle},
      {"offset", value.offset},
      {"size", value.size.has_value() ? json(value.size.value()) : json(nullptr)},
      {"fullRange", value.full_range},
      {"usedByActiveShader", value.used_by_active_shader},
  };
  if (value.values_source.has_value()) {
    j["valuesSource"] = value.values_source.value();
    j["valuesTruncated"] = value.values_truncated;
  }
  if (value.float_values.has_value()) {
    j["floatValues"] = value.float_values.value();
  }
  if (value.uint_values.has_value()) {
    j["uintValues"] = value.uint_values.value();
  }
  if (value.value_names.has_value()) {
    j["valueNames"] = value.value_names.value();
  }
}

// Root constants (D3D12), push constants (Vulkan), D3D9 shader constants and OpenGL uniforms, as the snapshot saw them
// for the active stage. firstValue is the index of values[0] in 32-bit words: D3D9 register cN starts at word 4 * N,
// an OpenGL uniform at location L at word 4 * L.
struct PushConstantSummary {
  std::string layout_handle;
  std::uint32_t layout_param = 0u;
  std::string stages;
  std::uint32_t first_value = 0u;
  std::vector<float> float_values;
  std::vector<std::uint32_t> uint_values;
  std::optional<std::vector<ValueNameSummary>> value_names = std::nullopt;
  // OpenGL: the program that was current when these uniforms were set (the draw's own program).
  std::optional<std::uint32_t> gl_program = std::nullopt;
  // The values were cut at the capture cap; words past it may have been set.
  bool values_truncated = false;
  // Words in the captured span that no push wrote; they read 0.
  std::vector<ValueRangeSummary> unwritten_ranges;
  // Named variables with no word in the captured span (variables past a truncated range are left out).
  std::vector<ValueWordsSummary> names_without_values;
  // "device": D3D9 float registers read from the device at the draw instead of collected from pushes.
  std::optional<std::string> values_source = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const PushConstantSummary& value) {
  j = {
      {"layoutHandle", value.layout_handle},
      {"layoutParam", value.layout_param},
      {"stages", value.stages},
      {"firstValue", value.first_value},
      {"floatValues", value.float_values},
      {"uintValues", value.uint_values},
  };
  if (value.value_names.has_value()) {
    j["valueNames"] = value.value_names.value();
  }
  if (value.gl_program.has_value()) {
    j["glProgram"] = value.gl_program.value();
  }
  if (value.values_truncated) {
    j["valuesTruncated"] = true;
  }
  if (!value.unwritten_ranges.empty()) {
    j["unwrittenRanges"] = value.unwritten_ranges;
  }
  if (!value.names_without_values.empty()) {
    j["namesWithoutValues"] = value.names_without_values;
  }
  if (value.values_source.has_value()) {
    j["valuesSource"] = value.values_source.value();
  }
}

struct PipelineBindSummary {
  std::string pipeline_handle;
  std::string stage;
  std::vector<std::string> shader_hashes;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const PipelineBindSummary& value) {
  j = {
      {"pipelineHandle", value.pipeline_handle},
      {"stage", value.stage},
      {"shaderHashes", value.shader_hashes},
  };
}

struct BlendAttachmentSummary {
  std::uint32_t rtv_index = 0u;
  bool blend_enable = false;
  bool logic_op_enable = false;
  std::string source_color_blend_factor;
  std::string dest_color_blend_factor;
  std::string color_blend_op;
  std::string source_alpha_blend_factor;
  std::string dest_alpha_blend_factor;
  std::string alpha_blend_op;
  std::string logic_op;
  std::uint32_t render_target_write_mask = 0u;
  std::string render_target_write_mask_hex;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const BlendAttachmentSummary& value) {
  j = {
      {"rtvIndex", value.rtv_index},
      {"blendEnable", value.blend_enable},
      {"logicOpEnable", value.logic_op_enable},
      {"sourceColorBlendFactor", value.source_color_blend_factor},
      {"destColorBlendFactor", value.dest_color_blend_factor},
      {"colorBlendOp", value.color_blend_op},
      {"sourceAlphaBlendFactor", value.source_alpha_blend_factor},
      {"destAlphaBlendFactor", value.dest_alpha_blend_factor},
      {"alphaBlendOp", value.alpha_blend_op},
      {"logicOp", value.logic_op},
      {"renderTargetWriteMask", value.render_target_write_mask},
      {"renderTargetWriteMaskHex", value.render_target_write_mask_hex},
  };
}

struct BlendStateSummary {
  bool alpha_to_coverage_enable = false;
  std::array<float, 4> blend_constant = {};
  std::vector<BlendAttachmentSummary> attachments;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const BlendStateSummary& value) {
  j = {
      {"alphaToCoverageEnable", value.alpha_to_coverage_enable},
      {"blendConstant", value.blend_constant},
      {"attachments", value.attachments},
  };
}

struct TextureStageArgumentSummary {
  std::string source;
  bool complement = false;
  bool alpha_replicate = false;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const TextureStageArgumentSummary& value) {
  j = {{"source", value.source}};
  if (value.complement) {
    j["complement"] = true;
  }
  if (value.alpha_replicate) {
    j["alphaReplicate"] = true;
  }
}

struct TextureStageSummary {
  std::uint32_t stage = 0u;
  std::string color_op;
  std::optional<TextureStageArgumentSummary> color_arg0 = std::nullopt;
  TextureStageArgumentSummary color_arg1;
  TextureStageArgumentSummary color_arg2;
  std::string alpha_op;
  std::optional<TextureStageArgumentSummary> alpha_arg0 = std::nullopt;
  TextureStageArgumentSummary alpha_arg1;
  TextureStageArgumentSummary alpha_arg2;
  std::optional<TextureStageArgumentSummary> result_arg = std::nullopt;
  // D3DTSS_CONSTANT (ARGB), when an argument reads it.
  std::optional<std::string> constant_hex = std::nullopt;
  // D3DTSS_BUMPENVLSCALE / BUMPENVLOFFSET, for bump_env_map_luminance.
  std::optional<float> bump_luminance_scale = std::nullopt;
  std::optional<float> bump_luminance_offset = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const TextureStageSummary& value) {
  j = {
      {"stage", value.stage},
      {"colorOp", value.color_op},
      {"colorArg1", value.color_arg1},
      {"colorArg2", value.color_arg2},
      {"alphaOp", value.alpha_op},
      {"alphaArg1", value.alpha_arg1},
      {"alphaArg2", value.alpha_arg2},
  };
  if (value.color_arg0.has_value()) {
    j["colorArg0"] = value.color_arg0.value();
  }
  if (value.alpha_arg0.has_value()) {
    j["alphaArg0"] = value.alpha_arg0.value();
  }
  if (value.result_arg.has_value()) {
    j["resultArg"] = value.result_arg.value();
  }
  if (value.constant_hex.has_value()) {
    j["constantHex"] = value.constant_hex.value();
  }
  if (value.bump_luminance_scale.has_value()) {
    j["bumpLuminanceScale"] = value.bump_luminance_scale.value();
  }
  if (value.bump_luminance_offset.has_value()) {
    j["bumpLuminanceOffset"] = value.bump_luminance_offset.value();
  }
}

struct FixedFunctionSummary {
  std::vector<TextureStageSummary> stages;
  std::string texture_factor_hex;
  std::array<float, 4> texture_factor = {};
  bool specular_enable = false;
  bool device_pixel_shader_bound = false;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const FixedFunctionSummary& value) {
  j = {
      {"stages", value.stages},
      {"textureFactorHex", value.texture_factor_hex},
      {"textureFactor", value.texture_factor},
      {"specularEnable", value.specular_enable},
      {"devicePixelShaderBound", value.device_pixel_shader_bound},
  };
}

struct D3D9VertexElementSummary {
  std::uint32_t stream = 0u;
  std::uint32_t offset = 0u;
  std::string type;
  std::uint32_t method = 0u;
  std::string usage;
  std::uint32_t usage_index = 0u;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const D3D9VertexElementSummary& value) {
  j = {
      {"stream", value.stream},
      {"offset", value.offset},
      {"type", value.type},
      {"usage", value.usage},
      {"usageIndex", value.usage_index},
  };
  // D3DDECLMETHOD_DEFAULT (0) is left out.
  if (value.method != 0u) {
    j["method"] = value.method;
  }
}

struct D3D9AlphaTestSummary {
  std::string function;
  std::uint32_t reference = 0u;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const D3D9AlphaTestSummary& value) {
  j = {
      {"function", value.function},
      {"reference", value.reference},
  };
}

struct D3D9TextureCoordinateIndexSummary {
  std::uint32_t stage = 0u;
  std::uint32_t coordinate_set = 0u;
  std::optional<std::string> generation_hex = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const D3D9TextureCoordinateIndexSummary& value) {
  j = {
      {"stage", value.stage},
      {"coordinateSet", value.coordinate_set},
  };
  if (value.generation_hex.has_value()) {
    j["generationHex"] = value.generation_hex.value();
  }
}

struct D3D9TextureTransformSummary {
  std::uint32_t stage = 0u;
  std::string flags_hex;
  std::uint32_t count = 0u;
  bool projected = false;
  // D3DTS_TEXTUREn, row-major (_11 _12 _13 _14 _21 ...); fixed-function vertex processing only.
  std::optional<std::array<float, 16>> matrix = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const D3D9TextureTransformSummary& value) {
  j = {
      {"stage", value.stage},
      {"flagsHex", value.flags_hex},
      {"count", value.count},
      {"projected", value.projected},
  };
  if (value.matrix.has_value()) {
    j["matrix"] = value.matrix.value();
  }
}

struct D3D9BumpEnvironmentSummary {
  std::uint32_t stage = 0u;
  std::array<float, 4> matrix = {};
  std::string address_u;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const D3D9BumpEnvironmentSummary& value) {
  j = {
      {"stage", value.stage},
      {"matrix", value.matrix},
      {"addressU", value.address_u},
  };
}

struct D3D9SamplerFilterSummary {
  std::uint32_t sampler = 0u;
  std::string mag_filter;
  std::string min_filter;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const D3D9SamplerFilterSummary& value) {
  j = {
      {"sampler", value.sampler},
      {"magFilter", value.mag_filter},
      {"minFilter", value.min_filter},
  };
}

// D3D9 state a port of the draw has to reproduce; empty lists and a zero FVF are left out.
struct D3D9StateSummary {
  std::optional<std::string> fvf_hex = std::nullopt;
  std::vector<D3D9VertexElementSummary> vertex_elements;
  std::optional<D3D9AlphaTestSummary> alpha_test = std::nullopt;
  std::vector<D3D9TextureCoordinateIndexSummary> texture_coordinate_indexes;
  std::vector<D3D9TextureTransformSummary> texture_transforms;
  std::vector<D3D9BumpEnvironmentSummary> bump_environments;
  std::vector<D3D9SamplerFilterSummary> sampler_filters;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const D3D9StateSummary& value) {
  j = json::object();
  if (value.fvf_hex.has_value()) {
    j["fvfHex"] = value.fvf_hex.value();
  }
  if (!value.vertex_elements.empty()) {
    j["vertexElements"] = value.vertex_elements;
  }
  if (value.alpha_test.has_value()) {
    j["alphaTest"] = value.alpha_test.value();
  }
  if (!value.texture_coordinate_indexes.empty()) {
    j["textureCoordinateIndexes"] = value.texture_coordinate_indexes;
  }
  if (!value.texture_transforms.empty()) {
    j["textureTransforms"] = value.texture_transforms;
  }
  if (!value.bump_environments.empty()) {
    j["bumpEnvironments"] = value.bump_environments;
  }
  if (!value.sampler_filters.empty()) {
    j["samplerFilters"] = value.sampler_filters;
  }
}

struct RenderTargetSummary {
  std::uint32_t rtv_index = 0u;
  resource_view_summary::ResourceViewSummary resource_view;
  std::optional<BlendAttachmentSummary> blend = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const RenderTargetSummary& value) {
  j = value.resource_view;
  j["rtvIndex"] = value.rtv_index;
  if (value.blend.has_value()) {
    j["blend"] = value.blend.value();
  }
}

struct DrawSummary {
  std::size_t index = 0u;
  std::string method;
  std::int64_t timestamp_ms = 0;
  std::size_t srv_count = 0u;
  std::size_t uav_count = 0u;
  std::size_t constant_count = 0u;
  std::size_t render_target_count = 0u;
  std::size_t pipeline_count = 0u;
  std::vector<std::string> shader_hashes;
  std::optional<std::string> copy_source_handle = std::nullopt;
  std::optional<std::string> copy_destination_handle = std::nullopt;
  std::optional<std::string> active_shader_stage = std::nullopt;
  std::optional<std::vector<SlotResourceBindingSummary>> srv_bindings = std::nullopt;
  std::optional<std::vector<SlotResourceBindingSummary>> uav_bindings = std::nullopt;
  std::optional<std::vector<ConstantBufferSummary>> constant_buffers = std::nullopt;
  std::optional<std::vector<PushConstantSummary>> push_constants = std::nullopt;
  std::optional<std::vector<RenderTargetSummary>> render_targets = std::nullopt;
  std::optional<std::vector<PipelineBindSummary>> pipelines = std::nullopt;
  std::optional<std::vector<ResourceBindSummary>> shader_resource_binds = std::nullopt;
  std::optional<BlendStateSummary> blend_state = std::nullopt;
  std::optional<FixedFunctionSummary> fixed_function = std::nullopt;
  std::optional<D3D9StateSummary> d3d9_state = std::nullopt;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const DrawSummary& value) {
  j = {
      {"index", value.index},
      {"method", value.method},
      {"timestampMs", value.timestamp_ms},
      {"srvCount", value.srv_count},
      {"uavCount", value.uav_count},
      {"constantCount", value.constant_count},
      {"renderTargetCount", value.render_target_count},
      {"pipelineCount", value.pipeline_count},
      {"shaderHashes", value.shader_hashes},
  };
  if (value.copy_source_handle.has_value()) {
    j["copySourceHandle"] = value.copy_source_handle.value();
  }
  if (value.copy_destination_handle.has_value()) {
    j["copyDestinationHandle"] = value.copy_destination_handle.value();
  }
  if (value.active_shader_stage.has_value()) {
    j["activeShaderStage"] = value.active_shader_stage.value();
  }
  if (value.srv_bindings.has_value()) {
    j["srvBindings"] = value.srv_bindings.value();
  }
  if (value.uav_bindings.has_value()) {
    j["uavBindings"] = value.uav_bindings.value();
  }
  if (value.constant_buffers.has_value()) {
    j["constantBuffers"] = value.constant_buffers.value();
  }
  if (value.push_constants.has_value()) {
    j["pushConstants"] = value.push_constants.value();
  }
  if (value.render_targets.has_value()) {
    j["renderTargets"] = value.render_targets.value();
  }
  if (value.pipelines.has_value()) {
    j["pipelines"] = value.pipelines.value();
  }
  if (value.shader_resource_binds.has_value()) {
    j["shaderResourceBinds"] = value.shader_resource_binds.value();
  }
  if (value.blend_state.has_value()) {
    j["blendState"] = value.blend_state.value();
  }
  if (value.fixed_function.has_value()) {
    j["fixedFunction"] = value.fixed_function.value();
  }
  if (value.d3d9_state.has_value()) {
    j["d3d9State"] = value.d3d9_state.value();
  }
}

struct LoadedDiskShaderSummary {
  std::string hash;
  std::uint32_t hash_value = 0u;
  std::string path;
  bool removed = false;
  bool compilation_ok = false;
  std::string compilation_error;
  bool activated = false;
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline void to_json(json& j, const LoadedDiskShaderSummary& value) {
  j = {
      {"hash", value.hash},
      {"hashValue", value.hash_value},
      {"path", value.path},
      {"removed", value.removed},
      {"isCompilationOk", value.compilation_ok},
      {"activated", value.activated},
  };
  if (!value.compilation_error.empty()) {
    j["compilationError"] = value.compilation_error;
  }
}

}  // namespace renodx::addons::devkit::mcp::draw_summary
