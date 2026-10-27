/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

#include "../../../utils/mcp/types.hpp"

namespace renodx::addons::devkit::mcp::tool_catalog {
inline constexpr std::uint32_t MAX_CAPTURE_DELAY_SECONDS = 30u;
inline constexpr std::uint32_t MAX_CAPTURE_KEY_WAIT_SECONDS = 60u;
// A snapshot frame took 30-60 s in Sleeping Dogs (about 4400 draws); twice that leaves room for heavier frames.
inline constexpr std::uint32_t MAX_SNAPSHOT_COMPLETION_WAIT_SECONDS = 120u;

inline renodx::utils::mcp::InputSchemaProperty MakeFlexibleHandleProperty() {
  return renodx::utils::mcp::InputSchemaProperty{
      .types = {"string"},
  };
}

inline renodx::utils::mcp::InputSchemaProperty MakeFlexibleUploadPathsProperty() {
  return renodx::utils::mcp::InputSchemaProperty{
      .types = {"array"},
      .item_types = {"string"},
      .item_enum_values = {
          "create_resource",
          "update_texture_region",
          "copy_buffer_to_texture",
      },
  };
}

inline renodx::utils::mcp::InputSchemaProperty MakeTextureReplaceRulesProperty() {
  using InputSchemaProperty = renodx::utils::mcp::InputSchemaProperty;

  return InputSchemaProperty{
      .types = {"array"},
      .item_types = {"object"},
      .item_properties = {
          {"name", {.types = {"string"}}},
          {"enabled", {.types = {"boolean"}}},
          {"uploadPaths",
           {
               MakeFlexibleUploadPathsProperty(),
           }},
          {"subresource", {.types = {"integer"}, .minimum = 0}},
          {"requireFullUpdate", {.types = {"boolean"}}},
          {"requireUploadHeapSource", {.types = {"boolean"}}},
          {"format", {.types = {"integer"}, .minimum = 0}},
          {"usageInclude", {.types = {"integer"}, .minimum = 0}},
          {"usageExclude", {.types = {"integer"}, .minimum = 0}},
          {"width", {.types = {"integer"}}},
          {"height", {.types = {"integer"}}},
          {"depthOrLayers", {.types = {"integer"}}},
          {"allowDynamic", {.types = {"boolean"}}},
          {"allowMultisampled", {.types = {"boolean"}}},
      },
  };
}

inline const renodx::utils::mcp::ToolCatalog METADATA = {
    {
        "devkit_status",
        {
            .title = "Devkit Status",
            .description = "Summarize tracked devices, selected device state, and snapshot activity. serverInfo.modulePath and moduleLastWriteUtc name the loaded Devkit file and its last write time, to tell which build is installed. With a RenoDX Display Proxy (a DX9 or OpenGL proxy mod, or the Devkit's Device Proxy) the D3D11 device that presents the final output is listed after the game's device with isProxy true and backBufferViews (view handles on its back buffers, with size and format): it is measure-only, for devkit_analyze_resource (use measurePoint frame_end), devkit_watch_region (end-of-frame numbers; it has no game writers) and devkit_dump_resource_with_hash; devkit_select_device, snapshots, shader, live-shader, clone and texture-replace tools refuse it, and the snapshot listings are empty for it.",
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_select_device",
        {
            .title = "Select Device",
            .description = "Change the currently selected devkit device by index.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                },
                .required = {"deviceIndex"},
            },
        },
    },
    {
        "devkit_list_shaders",
        {
            .title = "List Shaders",
            .description = "List tracked shaders for a device, optionally filtered by stage.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"stage", {.types = {"string"}}},
                    {"limit", {.types = {"integer"}, .minimum = 1, .maximum = 5000}},
                    {"offset", {.types = {"integer"}, .minimum = 0}},
                },
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_get_shader",
        {
            .title = "Get Shader",
            .description = "Return metadata for a tracked shader and optionally include disassembly or decompilation text via a named view or a views array. DXBC uses configured cmd_Decompiler.exe; DXIL uses the internal DXC decompiler.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"shaderHash", MakeFlexibleHandleProperty()},
                    {"view", {.types = {"string"}, .enum_values = {"summary", "disassembly", "decompilation", "all"}}},
                    {"views", {.types = {"array"}, .item_types = {"string"}, .item_enum_values = {"disassembly", "decompilation"}}},
                    {"maxTextLength", {.types = {"integer"}, .minimum = 1}},
                },
                .required = {"shaderHash"},
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_dump_shader",
        {
            .title = "Dump Shader",
            .description = "Write a tracked shader's original bytecode to disk for external decompilers or archiving.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"shaderHash", MakeFlexibleHandleProperty()},
                    {"outputPath", {.types = {"string"}}},
                },
                .required = {"shaderHash"},
            },
        },
    },
    {
        "devkit_set_tools_path",
        {
            .title = "Set Tools Path",
            .description = "Query or change the directory used for DirectX and Vulkan shader tools from the repo bin directory.",
            .input_schema = {
                .properties = {
                    {"path", {.types = {"string"}}},
                },
            },
        },
    },
    {
        "devkit_set_live_shader_path",
        {
            .title = "Set Live Shader Path",
            .description = "Query or change the directory used by the devkit live shader compiler.",
            .input_schema = {
                .properties = {
                    {"path", {.types = {"string"}}},
                },
            },
        },
    },
    {
        "devkit_load_live_shaders",
        {
            .title = "Load Live Shaders",
            .description = "Compile and activate shader replacements from the configured live shader directory for the selected device. Each file that failed to compile lists the compiler's message as compilationError (failedCount counts them); that shader is then left with no replacement at all, so the game's original shader runs even where a game mod replaces it (devkit_unload_live_shaders brings the mod's version back). With the Devkit's Live Reload setting on, the Devkit already loads changed files at each present and this call finds nothing new: compile errors are then only in ReShade.log.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                },
            },
        },
    },
    {
        "devkit_unload_live_shaders",
        {
            .title = "Unload Live Shaders",
            .description = "Remove the live shader file replacements from the selected device. Only shaders whose source is a live file are reset; a shader a game mod also replaces goes back to the mod's version, so the mod keeps running. (The overlay's Unload Shaders menu item still removes every runtime replacement, the mod's included.)",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                },
            },
        },
    },
    {
        "devkit_set_shader_source",
        {
            .title = "Set Shader Source",
            .description = "Choose what one shader hash draws with from its next bind: 'original' (the game's shader, no replacement), 'addon' (the game mod's replacement, refused when no add-on replaces this hash or the Devkit has not synced its replacements yet) or 'file' (the compiled live shader file, refused without one). Same as the Original/Add-on/File choice in the Devkit overlay; use it to find which replaced shader causes an artifact, and switch back to 'addon' afterwards. Only that hash changes: the mod's resource upgrades, clones and swap-chain proxy stay, so 'original' is not a vanilla view of the game (use the mod's Off preset for that). The switch destroys the replacement pipeline at once: on D3D12/Vulkan the present queue is waited idle first, which does not cover other queues (async compute) or command lists already recorded for the next frame, so a crash or device loss is possible there; the overlay's combo has the same risk. Returns previousSource, source, addonReplacesShader and hasLiveFile.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"shaderHash", MakeFlexibleHandleProperty()},
                    {"source", {.types = {"string"}, .enum_values = {"original", "addon", "file"}}},
                },
                .required = {"shaderHash", "source"},
            },
        },
    },
    {
        "devkit_set_draw_bypass",
        {
            .title = "Set Draw Bypass",
            .description = "Skip (bypass true) or draw again (bypass false) the draws (draw, draw_indexed, dispatch, indirect draw/dispatch; not mesh or ray dispatches) that have this shader hash bound in any stage, like the overlay's per-shader Draw button (both set the same flag). It is applied at the next present, so it needs a presenting game (minimized or loading: the call times out after 15 s), and it affects command lists recorded after that: D3D12 bundles and command lists the game records once and executes again keep their draws. It applies in every pass the shader draws in (devkit_frame_overview sharedShaders shows where one shader is shared) and stays until set back or the game restarts. Use it to find which shader produces an artifact by removing it. The result lists every bypassed shader (bypassedShaders); devkit_list_shaders shows bypassDraw per shader. A bypassed draw still appears as a writer in devkit_watch_region, which measures before each draw, with no change to the region. Refused for a hash the device has not seen. A draw another add-on already skipped is not drawn either way.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"shaderHash", MakeFlexibleHandleProperty()},
                    {"bypass", {.types = {"boolean"}}},
                },
                .required = {"shaderHash", "bypass"},
            },
        },
    },
    {
        "devkit_shader_coverage",
        {
            .title = "Shader Coverage",
            .description = "Record which replaced shaders (a game mod's replacements and live shader files the Devkit knows on this device) actually draw while the person plays. action 'start' takes the current set of replaced shaders and starts counting; play through what the mod must cover (menus, loading screens, videos, fades, gameplay); 'status' reports without stopping; 'stop' ends the recording and reports. The report lists neverDrawn first (replacements this session did not exercise: test those scenes), then drawn with draws, framesSeen and first/lastFrame (presents since start); at most 64 per group, the counts are complete. Counts cover draws, indexed draws, dispatches and indirect calls with the shader bound in any stage; on D3D12/Vulkan a command list recorded once and executed again is counted when recorded. While recording, every draw looks up its bound shaders and takes a shared lock (an exclusive one only when a watched shader draws). start is refused while a recording runs (stop first). A recording ends with its device: a game that recreates its device needs a new start. Not on a Display Proxy device.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"action", {.types = {"string"}, .enum_values = {"start", "status", "stop"}}},
                },
                .required = {"action"},
            },
        },
    },
    {
        "devkit_write_shader_stub",
        {
            .title = "Write Shader Stub",
            .description = "Write a tracked shader's decompiled HLSL (cmd_Decompiler output, the same text as devkit_get_shader's decompilation) to a file named the way the live-shader loader expects, 0x<HASH>.<stage>_<major>_<minor>.hlsl (e.g. 0x67843125.ps_5_0.hlsl), as the starting point of a replacement: the decompiler keeps the shader's own input/output signature and constant buffer layout. DXBC shader model 4 and 5 only (D3D10/11, D3D12 SM5; SM 5.1 stubs are untested and may lose register spaces, the result says so); refused for SM6 (DXIL), D3D9, OpenGL and Vulkan shaders (the shader model is read from the bytecode, so no snapshot is needed). DXBC decompilation needs cmd_Decompiler.exe and its d3dcompiler_46.dll in the tools path (devkit_set_tools_path, e.g. the repo bin folder); a failed decompilation is tried again on the next call. outputDirectory (absolute; created if missing) defaults to the Devkit's dump folder, outside the default live folder; refused when the live path was set to a folder that contains it. Refused when the folder already holds any file named for this hash (0x<HASH>.), so nothing is overwritten. Then edit it, copy it into the live shader folder and run devkit_load_live_shaders (or point outputDirectory at the live folder: with the Devkit's Live Reload on, a file written there is compiled and activated at once, replacing a game mod's own shader for that hash). Check a fresh stub first with an A/B (devkit_analyze_resource abShaderHash, abSourceA 'original', abSourceB 'file') on a small region of the target the shader writes, after an A-against-A run for the noise floor: overlapping ranges mean no measurable difference in the region's statistics, not a pixel-exact match.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"shaderHash", MakeFlexibleHandleProperty()},
                    {"outputDirectory", {.types = {"string"}}},
                },
                .required = {"shaderHash"},
            },
        },
    },
    {
        "devkit_list_draws",
        {
            .title = "List Draws",
            .description = "List draw records from the selected device's current snapshot cache, with optional filters for method, shader hash, stage, and binding/render-target counts.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"method", {.types = {"string"}}},
                    {"shaderHash", MakeFlexibleHandleProperty()},
                    {"activeShaderStage", {.types = {"string"}}},
                    {"minSrvCount", {.types = {"integer"}, .minimum = 0}},
                    {"minUavCount", {.types = {"integer"}, .minimum = 0}},
                    {"minConstantCount", {.types = {"integer"}, .minimum = 0}},
                    {"minRenderTargetCount", {.types = {"integer"}, .minimum = 0}},
                    {"minPipelineCount", {.types = {"integer"}, .minimum = 0}},
                    {"limit", {.types = {"integer"}, .minimum = 1, .maximum = 1000}},
                    {"offset", {.types = {"integer"}, .minimum = 0}},
                },
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_list_replacements",
        {
            .title = "List Replacements",
            .description = "List every shader hash an add-on registered a replacement for on the device (compileTime: registered for replacement when the game creates a pipeline; runtime: registered for replacement when a pipeline is bound. RenoDX mods usually register both; the Devkit's live shaders are runtime only), with how the current snapshot's draws used it, recorded when each draw was captured: snapshotDraws (draws whose pipeline uses the original hash) and snapshotDrawsReplaced (of those, draws whose pipeline had the replacement in effect at that moment: created with it, or a bind-time replacement pipeline built for that stage). A registered hash with snapshotDraws 0 did not draw in the snapshot frame; snapshotDraws above snapshotDrawsReplaced means the original shader still drew. Counts need a finished snapshot (devkit_queue_snapshot); snapshotPending is true while one is queued or capturing. The list is paged (limit, default 100, max 5000; offset), in hash order; drawnOnly keeps only hashes the snapshot drew, which keeps a generic mod with hundreds of registered shaders readable. totalRegistered, totalDrawn and totalFiltered give the counts before paging.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"drawnOnly", {.types = {"boolean"}}},
                    {"limit", {.types = {"integer"}, .minimum = 1, .maximum = 5000}},
                    {"offset", {.types = {"integer"}, .minimum = 0}},
                },
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_frame_overview",
        {
            .title = "Frame Overview",
            .description = "Summarize the current snapshot in one call. Commands are grouped into passes (consecutive draws or dispatches of the same kind writing the same render targets or UAVs with the same blend state; each copy is its own pass, destination as target, source as input), each with its draw range (indexes for devkit_get_draw), targets (the resource the game created: type, format, size, isSwapchain, upgradedFrom when a mod upgraded it in place, cloneFormat/cloneActive for a mod's clone), blendEnabled (null when the API recorded none), input count and shader hashes; orderUnknown marks D3D12/Vulkan commands without a known submission order. writtenTextures list, latest first, the passes that write each texture and the draws that read it. Candidate hints, each with the rule that matched: tonemap (a non-blended draw or a dispatch whose large first target is SDR as rendered or as the game created it, reading a same-size float texture as rendered, so a mod's float clone counts; also a pass that fills a float swap chain from a same-size float texture, since a swap chain a mod upgraded at creation, or an HDR-native output, leaves no SDR format to compare with; the rule notes when the pass writes the swap chain, which may make it the output pass, and when its float input is float only through a mod's clone or in-place upgrade (created as a non-float format by the game); both together mark the game's final AA/upscale/composite pass reading an intermediate the mod upgraded; passes an add-on draws through the ReShade API, such as a mod's swap-chain proxy pass, are never in a snapshot), video (a pass reading two or more R8/R8G8 textures, one half the size of another: YUV 4:2:0 planes of decoded video, so check video brightness there; noise or mask textures of that shape also match, and video decoded into one NV12 texture is not found), LUT textures (3D cubes of 16 to 65, or N*N x N strips with N = 16/32/33/48/64) with their builder passes and their reader passes (only large targets that are not square, so scene, shadow and probe draws that sample noise or lighting volumes are left out; without a swap chain only the size is checked; non-square shadow atlases still match), UI (blended draws onto large swap-chain or game-SDR targets after the last tonemap candidate that is not such a final pass from a mod-upgraded intermediate, or after the last candidate when every candidate is one; a candidate that only reads textures written earlier in the frame is noted as a possible composite of post-processing or of UI drawn into an earlier target) and output (the last pass writing a swap-chain back buffer). sharedShaders lists each shader (pixel or compute stage) of a tonemap, UI or output candidate that also draws in a pass of another role (ui, tonemap, output, other; a pass that is both tonemap and output counts as tonemap), with up to 8 of those passes (the first pass of each role, then the others in frame order) and the first draw of that shader in each (firstDrawOfShader, for devkit_get_draw): a replacement of that hash changes all of them, as when one shader draws both UI and scene. sharedShaderCount counts them all; at most 16 are listed, latest first candidate pass first. Hints are candidates, not proof: confirm with devkit_get_draw, the shader and devkit_watch_region. Snapshots record no viewports, vertex counts or depth targets: large means at least a quarter of the swap chain's back-buffer area. D3D9 and OpenGL blend state is read from the device or context state at each draw. Readers come from the views recorded per command and copy sources; reads through descriptor tables the snapshot did not walk and bindless reads are missing. maxPasses (default 40) goes to candidates first, then the latest passes; maxResources (default 32) caps writtenTextures. Needs a finished snapshot (devkit_queue_snapshot); snapshotPending is true while one is queued or capturing. saveBaseline (absolute path ending in .json, overwritten) writes every pass's structure (targets and inputs with the format the game created and the format rendered, size, swap chain; shader hashes; command count; hint types) to a file, which survives a game restart, and returns its path as baselineSaved; compareWith (a saveBaseline file) compares this snapshot with it and adds comparison: passes aligned in frame order by kind, targets as the game created them (a swap chain by size alone, since a mod upgrades it at creation) and blend, then per aligned pass the changes (targetGameFormat: a swap chain created in another format, as after a mod's swap-chain upgrade; targetRenderedFormat: a target upgraded, cloned or no longer, left out when it only repeats targetGameFormat; inputRenderedFormats added/removed; shaderHashes for passes of at most 4 shaders, since scene passes vary per frame; commandCount when it changed by half or more; hints), plus passes only in the baseline or only now; changedPasses, onlyInBaseline and onlyInCurrent list at most 32 entries each, the latest in the frame (tonemap, UI and output come last), earlier entries are left out and the counts are complete. Use it before/after a resource upgrade or mod change (with a restart in between), on the same spot; first compare two snapshots of an unchanged still scene to see what varies on its own.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"maxPasses", {.types = {"integer"}, .minimum = 1, .maximum = 2000}},
                    {"maxResources", {.types = {"integer"}, .minimum = 0, .maximum = 2000}},
                    {"saveBaseline", {.types = {"string"}}},
                    {"compareWith", {.types = {"string"}}},
                },
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_get_draw",
        {
            .title = "Get Draw",
            .description = "Return full details for a single captured draw, including bindings and blend state. includeConstantValues adds the constant values the draw saw, copied when the snapshot recorded it: per bound constant buffer, valuesSource (cpu_cache: the bytes the CPU last wrote through map/unmap or update_buffer_region, at most 4096 bytes from the bound offset, valuesTruncated when that cap was reached; untracked: the Devkit setting 'Trace Constant Buffers' was off when the game started, the buffer is not a tracked constant buffer, or it was mapped at that moment; not_read_by_shader: the active shader's reflected bindings do not include it; unsupported_api: D3D12/Vulkan, whose buffers are written through persistent maps or copies the CPU cache cannot see) with floatValues and uintValues (32-bit words); and pushConstants for the active stage and, on D3D12/Vulkan, the bound layout (D3D12 root constants, Vulkan push constants, D3D9 Set*ShaderConstantF/I with register N at word 4*N and Set*ShaderConstantB at word N (layoutParam 5/6/7 for the pixel shader's F/I/B), OpenGL glUniform* at word 4*location, only those set while the draw's program was current; at most 1024 words each, firstValue = word of values[0]). valueNames (D3D10/11 cbuffers and D3D12 root constants from the shader's buffer definitions, D3D9 pixel shader F/I/B constants from its constant table, Vulkan push constants from the SPIR-V push constant block (word = byte Offset / 4), OpenGL uniforms of the draw's program from glGetActiveUniform (pushConstants then hold only the values set while that program, glProgram, was current; glProgramUniform* is not seen); empty when the game stripped the reflection or the active shader declares none of the pushed values) labels values with the active pixel or compute shader's own variable names (on OpenGL, the whole linked program's): name, index into floatValues/uintValues, count in words. Each pushConstants entry also lists unwrittenRanges (index, count: words inside the span that no call recorded in snapshot frames wrote; they read 0) and namesWithoutValues (named variables with no word in the captured span, so no call recorded for this entry set them: name, absolute word, count when known; per entry, so a register set with no push recorded lists nothing, and entries sharing a register set each list the others' variables) and valuesTruncated (the range passed the 1024-word cap; names past it are not listed). Values come only from calls the game makes during snapshot frames: a constant set before the snapshot frame is missing, or on D3D9 and immediate contexts can be left over from an earlier snapshot. D3D9 draws the game issued without a pixel shader add fixedFunction, read from the device at the draw: the texture stages up to the first whose colorOp is disable (stage, colorOp and alphaOp as D3DTOP names in snake case, colorArg1/2 and alphaArg1/2 with source (diffuse, current, texture, texture_factor, specular, temp, constant) and the complement and alphaReplicate modifiers, colorArg0/alphaArg0 for multiply_add and lerp, resultArg when it is not current), textureFactor (RGBA 0-1) with textureFactorHex (ARGB), specularEnable, and devicePixelShaderBound (an add-on drew the draw with its own pixel shader, bound without a ReShade event; the stages are still the game's). modulate2x, modulate4x and add stages clamp at white in hardware. Not on a pure device (D3DCREATE_PUREDEVICE).",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"drawIndex", {.types = {"integer"}, .minimum = 0}},
                    {"includeConstantValues", {.types = {"boolean"}}},
                },
                .required = {"drawIndex"},
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_analyze_resource",
        {
            .title = "Analyze Resource",
            .description = "Copy a tracked resource view, or its instantiated clone when requested, into a temporary readback texture, summarize its current contents, and optionally save a PNG preview or RGBA16F EXR dump, or (outputPath ending in .json) the finished result itself, also with frames and abShaderHash, so two runs (for example mod on and off, or before and after a rebuild, at the same spot) can be compared afterwards. Optionally restrict everything to a pixel region (regionX/regionY/regionWidth/regionHeight, all four together) and add a display-light summary in nits (signal 'pq' for HDR10 ST 2084 BT.2020 targets, 'scrgb' for linear 1.0 = 80 nits BT.709 targets) with luminance percentiles and shares above 203/400/1000/4000 nits, plus colour statistics of pixels at or above 1 nit in CIE 1976 u'v': saturation s_uv (13 x the distance from the D65 white point, independent of brightness) with min/max/mean and percentiles, the share of chromatic pixels (s_uv >= 0.05) and a 12-bin hue histogram (angle around D65) of those pixels; compare two captures of the same spot (for example the mod's vanilla and HDR presets; check that pixelsCounted is similar) to check saturation and hue shifts. waitForKey (F1 to F12) first waits, at most 60 s, until the person presses that key in the game (the call fails without capturing if they do not; only one call can wait for a key at a time); delaySeconds (0-30) then waits that long before queueing the request, so the capture can land on gameplay after the game is unpaused; other Devkit tool calls wait until it is done. measurePoint 'present' (default) reads in the Devkit's own present handler, before ReShade's effects and overlay and possibly before present work of add-ons loaded after the Devkit; 'frame_end' reads in reshade_present, after every add-on's present handler and after ReShade's visible effects and overlay (menu, FPS/clock, Lilium), so disable those for clean numbers (D3D9/D3D11/D3D12/OpenGL/Vulkan; on Vulkan a back buffer is copied in reshade_present and analyzed after the present, other textures are read as with present; needs a swap chain with a ReShade effect runtime). Multisampled textures are resolved (averaged) into a single-sample copy first on D3D9/D3D10/D3D11/OpenGL (resolvedFromSampleCount); refused on D3D12/Vulkan, for depth-stencil, for formats D3D10/11 cannot resolve, and for a multisampled OpenGL default framebuffer. With a RenoDX Display Proxy (DX9 or OpenGL games) the final HDR10 output is on the proxy device (devkit_status isProxy): pass its deviceIndex and one of its backBufferViews as resourceViewHandle, with measurePoint 'frame_end' (present may run before the proxy pass is drawn). Every analysis also has pixelsAnyChannelExactly1 and signalHints: rule-labelled facts (unormStorage, aboveOne, atOne for a float format with at least 1% of pixels exactly at 1.0, negative, nonFinite; colour rules skipped for one- and two-channel data), candidates for the signal classification that still has to be proven with the snapshot, the shader and a watch; linear and gamma encoding are not told apart. banding (true; one frame, no A/B) adds analysis.banding: per channel the steps between neighbouring pixels in the stored values (UNORM in codes, an sRGB format re-encoded; float relative and absolute): smoothShare (pairs at most 4 codes apart, for floats 2% of the value or 4/255), equalShare, medianStepCodes (UNORM) or medianStepRelative/medianStepAbsolute/medianStepIn8BitCodes (float), and bands (horizontal runs of at least two equal values bounded by small non-zero steps on both sides) with meanBandLength, longestBand and bandsAtLeast8px. Use a region on a smooth gradient (sky; a full float frame costs time on the game's thread): long bands bounded by 1-code steps are visible banding, many short ones are dither or noise, and on a mod's float clone, medianStepIn8BitCodes as coarse as the RGBA8 original's medianStepCodes (read it with preferClone false; only comparable when both hold the same encoding) means the quantisation comes from upstream. frames (1-32, default 1) measures that many consecutive frames, one per present on the game's own thread (the game stalls while each frame is analyzed, so a 4K frame with a signal can take seconds in all; other Devkit tool calls wait), and adds frames: per-frame headline numbers (luminance mean/max, nan and negative ratios; with a signal nits mean/max/p50/p90/p99/p99.9, shares above 203/400/1000 nits, saturation mean/p90) and their summary (n, mean, stddev, min, max; per side instead in an A/B); analysis is then the last frame. The call gives up after 180 s (a game that stops presenting, e.g. minimized), canceling the rest. Samples follow the device's presents: with several swap chains on one device they interleave windows. Use it where single frames vary (scene motion, frame-to-frame flicker) before comparing two captures. frames above 1 needs measurePoint 'present' and no image outputPath (a .json one is allowed). abShaderHash runs an A/B on that shader: frames per side, alternating A, B, B, A, ... on consecutive frames (balanced against a scene that flips between two states each frame); the game's thread switches the shader's source between abSourceA (default 'original') and abSourceB (default 'addon'), each 'original', 'addon' or 'file' as in devkit_set_shader_source, after one warm-up frame, and puts the shader's previous source back at the end, also on failure. The result adds ab: each side's summary, difference (B minus A of each mean, rangesOverlap when the sides' min-max ranges overlap, i.e. within frame noise) and restoredSource; frames.samples carry their side. Run A against A (abSourceA = abSourceB) to see the noise floor. Needs measurePoint 'present', no image outputPath (a .json one is allowed), the game's device (not a Display Proxy) and D3D9, D3D10, D3D11 or OpenGL (on D3D12/Vulkan a switch destroys a pipeline frames in flight may still use). Only that shader changes: the mod's upgrades and proxy stay. Refused while the Devkit's Live Reload is on (it would switch the shader mid-run). The previous source is restored on completion, failure and cancel; if it cannot be (e.g. 'file' after a failed compile) the shader falls back to 'addon', then 'original', reported in ab.restoredSource and ab.restoreNote. A frame measured right after a switch reflects the new source on the immediate context; effects that carry earlier frames (TAA history, eye adaptation) and command lists recorded ahead (D3D11 deferred contexts) mix in the other side.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"resourceViewHandle", MakeFlexibleHandleProperty()},
                    {"outputPath", {.types = {"string"}}},
                    {"previewMode", {.types = {"string"}, .enum_values = {"clamped", "neutwo"}}},
                    {"preferClone", {.types = {"boolean"}}},
                    {"regionX", {.types = {"integer"}, .minimum = 0}},
                    {"regionY", {.types = {"integer"}, .minimum = 0}},
                    {"regionWidth", {.types = {"integer"}, .minimum = 1}},
                    {"regionHeight", {.types = {"integer"}, .minimum = 1}},
                    {"signal", {.types = {"string"}, .enum_values = {"raw", "pq", "scrgb"}}},
                    {"delaySeconds", {.types = {"integer"}, .minimum = 0, .maximum = MAX_CAPTURE_DELAY_SECONDS}},
                    {"waitForKey", {.types = {"string"}, .enum_values = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"}}},
                    {"measurePoint", {.types = {"string"}, .enum_values = {"present", "frame_end"}}},
                    {"frames", {.types = {"integer"}, .minimum = 1, .maximum = 32}},
                    {"banding", {.types = {"boolean"}}},
                    {"abShaderHash", MakeFlexibleHandleProperty()},
                    {"abSourceA", {.types = {"string"}, .enum_values = {"original", "addon", "file"}}},
                    {"abSourceB", {.types = {"string"}, .enum_values = {"original", "addon", "file"}}},
                },
                .required = {"resourceViewHandle"},
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_watch_region",
        {
            .title = "Watch Region",
            .description = "Measure a pixel region of a texture on the GPU before every command that writes it (draws with it as render target, compute dispatches with it as UAV, copies, render target and UAV clears, resolves, updates; on D3D12/Vulkan render passes with it as attachment count as one writer listing their draws) and once at the end of every frame (ReShade's reshade_present, after every add-on's present handler; on the back buffer this final measurement also includes ReShade's visible effects and everything drawn on its overlay (menu, FPS/clock, add-on on-screen drawing), so disable them for clean final numbers), then report which writers changed the region and how. frames=1 returns a per-writer trace in command order (commandIndex is the ordinal among draws, dispatches and copies of the frame, in submission order on D3D12/Vulkan; it is offset from devkit_list_draws indexes, so match snapshot draws by shader hash and relative order); frames>1 watches that many frames and aggregates per writer (shader hashes): executions, how often the region changed, min/mean/max of the region peak before and after, how often the peak was lowered or values went from above 1.0 to at most 1.0, and the most repeated after-peak (a ceiling); every measurement also has maxAlpha and pixelsAlphaAboveOne (alpha as stored; formats without alpha read 1; a writer that changes only alpha counts as changing the region), and frames>1 counts raisedAlphaAboveOneExecutions (no pixel's alpha above 1.0 before the writer, some after: a blend or upgrade side effect) with afterMaxAlpha. Writers whose state before or after could not be measured are never dropped: they are counted per reason (unmeasured.reasons, unmeasuredExecutions per writer, unmeasured.frames). The region (regionX/regionY/regionWidth/regionHeight, all four together) defaults to the whole texture; signal 'pq' or 'scrgb' reports luminance in nits. One- and two-channel formats (R8, R16, R16F, R32F, R8G8, R16G16, R16G16F, R32G32F) and R9G9B9E5 (not on OpenGL) are watched except on D3D9: their values are in R (and G), the peak is the largest channel (peakMeasure maxChannel; means stay BT.709 luminance of (R, G, 0) and mean nothing there) and a signal is refused. shaderHash limits measuring to writers using that shader. includeUnchanged also lists writers that did not change the region (frames=1). withSnapshot (frames=1 only) also captures a snapshot of the watched frame and gives each traced draw, dispatch and copy (and each draw of a D3D12/Vulkan render pass) its snapshotDraw index (for devkit_get_draw) with its targets and inputs as devkit_frame_overview describes them; clears, resolves and updates are not snapshot commands and get none, nor do D3D12/Vulkan commands recorded before the watched frame; snapshotLinkedCommands counts the linked commands, snapshotLinkError says why none could be linked (another snapshot was running, replaced it or had not finished). A snapshot frame is slow, so the call can take up to two minutes longer. verify also reads the region back to the CPU at one measurement point at the end of the watch and checks the GPU statistics of that same point against it. scratchBudgetMB (D3D9/OpenGL/D3D12/Vulkan, default 512) caps the VRAM used for the region copies taken inside the game's command lists. On D3D12/Vulkan a swap-chain image is followed: all images of its swap chain count as the watched texture. Vulkan images without copy-source usage are refused unless the Devkit setting 'Watch Region: Copyable Vulkan Images' was enabled before the game created them. OpenGL (GL 4.3 compute; only the context that presents the swap chain is measured; the region rectangle is exactly the one devkit_analyze_resource reads, which on the default framebuffer may count rows from the bottom, OpenGL's origin (check with a known bright pixel); colour textures, renderbuffers and the default framebuffer, no multisampled, depth or three-channel formats; draws into texture views of the watched texture are not seen). D3D9Ex (DX9 games running a RenoDX proxy mod, a wrapper that creates the device with CreateDeviceEx such as dxwrapper's D3d9to9Ex, or the Devkit's Force DX9Ex), D3D11, D3D12 and Vulkan; blocks until the watch completes. On a RenoDX Display Proxy device (devkit_status isProxy, watched through one of its backBufferViews) the proxy pass is drawn through the ReShade API and reports no writer, so only the end-of-frame numbers count there, and withSnapshot links nothing. waitForKey (F1 to F12) first waits, at most 60 s, until the person presses that key in the game (the call fails without capturing if they do not; only one call can wait for a key at a time); delaySeconds (0-30) then waits that long before queueing the watch, so it can start on gameplay after the game is unpaused; other Devkit tool calls wait until it is done.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"resourceViewHandle", MakeFlexibleHandleProperty()},
                    {"preferClone", {.types = {"boolean"}}},
                    {"regionX", {.types = {"integer"}, .minimum = 0}},
                    {"regionY", {.types = {"integer"}, .minimum = 0}},
                    {"regionWidth", {.types = {"integer"}, .minimum = 1}},
                    {"regionHeight", {.types = {"integer"}, .minimum = 1}},
                    {"signal", {.types = {"string"}, .enum_values = {"raw", "pq", "scrgb"}}},
                    {"frames", {.types = {"integer"}, .minimum = 1, .maximum = 3600}},
                    {"shaderHash", MakeFlexibleHandleProperty()},
                    {"includeUnchanged", {.types = {"boolean"}}},
                    {"withSnapshot", {.types = {"boolean"}}},
                    {"verify", {.types = {"boolean"}}},
                    {"scratchBudgetMB", {.types = {"integer"}, .minimum = 1, .maximum = 8192}},
                    {"delaySeconds", {.types = {"integer"}, .minimum = 0, .maximum = MAX_CAPTURE_DELAY_SECONDS}},
                    {"waitForKey", {.types = {"string"}, .enum_values = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"}}},
                },
                .required = {"resourceViewHandle"},
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_dump_resource_with_hash",
        {
            .title = "Dump Resource With Hash",
            .description = "Read back a tracked resource view, compute a raw-byte CRC32, prefer a matching tracked upload CRC when available, and dump a hash-named PNG for replacement workflows. Multisampled textures are resolved first, as in devkit_analyze_resource.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"resourceViewHandle", MakeFlexibleHandleProperty()},
                    {"outputDirectory", {.types = {"string"}}},
                    {"namePrefix", {.types = {"string"}}},
                    {"preferClone", {.types = {"boolean"}}},
                },
                .required = {"resourceViewHandle", "outputDirectory"},
            },
        },
    },
    {
        "devkit_analyze_lut",
        {
            .title = "Analyze LUT",
            .description = "Read a texture as a colour lookup table, at the Devkit's next present, and summarize it without dumping images. Layouts: '3d' (an N x N x N 3D texture; red along x, green along y, blue across slices), 'strip_x' (a 2D N*N x N strip, N slices side by side, blue selects the slice: Unreal 256x16, Unity 1024x32) and 'strip_y' (N x N*N, slices stacked); 'auto' (default) picks one from the shape (any N from 2 to 256; devkit_frame_overview lists the usual LUT sizes). flipY when the image is stored bottom-up (green increases upward; for strip_y the slice order reverses too). Input point (r, g, b) / (N - 1) is in the LUT's own coordinates, whatever encoding the game uses (often log, PQ or gamma), so the numbers describe the LUT, not display light. Returns size, format, srgbFormat; corners (black, red, green, blue, yellow, magenta, cyan, white: a primary at the wrong corner means the layout or flipY is wrong); neutralAxis (output along (i, i, i)) and whether each channel is monotonic there; identityDeviation (|output - input| per channel, mean and max, p99 of the per-point maximum, and the worst point; for sRGB formats measured on the stored, encoded values) and isIdentity (max within 1.5/255); outputRange, valuesAboveOne and nonFiniteValues (channel values, not points; any non-finite value makes isIdentity false) and allZero (all zero usually means the LUT is built later in the frame or its texture is reused after the LUT pass: read it from a pass that samples it, or with another snapshot). outputPath (absolute .csv) writes every lattice point (indices, input, output as read) for N up to 65 (larger LUTs: csvError, the statistics are still returned). preferClone reads a mod's active clone. The view must select the top level of a single texture. 3D LUTs on D3D10/11/12 and OpenGL (refused on D3D9, where ReShade does not copy volumes, and on Vulkan, whose 3D readback needs LINEAR tiling drivers need not support); strips on every API. Like devkit_analyze_resource, the copy issues no state transition on D3D12/Vulkan, so a LUT in a shader-read state is copied as it is.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"resourceViewHandle", MakeFlexibleHandleProperty()},
                    {"layout", {.types = {"string"}, .enum_values = {"auto", "3d", "strip_x", "strip_y"}}},
                    {"flipY", {.types = {"boolean"}}},
                    {"outputPath", {.types = {"string"}}},
                    {"preferClone", {.types = {"boolean"}}},
                },
                .required = {"resourceViewHandle"},
            },
        },
    },
    {
        "devkit_set_resource_clone",
        {
            .title = "Set Resource Clone",
            .description = "Enable or disable devkit clone hotswap on a tracked texture resource for live comparison.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"resourceHandle", MakeFlexibleHandleProperty()},
                    {"enabled", {.types = {"boolean"}}},
                },
                .required = {"resourceHandle", "enabled"},
            },
        },
    },
    {
        "devkit_replace_resource_with_file",
        {
            .title = "Replace Resource With File",
            .description = "Enable clone hotswap for a tracked resource or view, create a clone if needed, and upload a PNG into that clone for immediate live replacement.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"resourceViewHandle", MakeFlexibleHandleProperty()},
                    {"resourceHandle", MakeFlexibleHandleProperty()},
                    {"path", {.types = {"string"}}},
                },
                .required = {"path"},
            },
        },
    },
    {
        "devkit_set_texture_replace_enabled",
        {
            .title = "Set Texture Replace Enabled",
            .description = "Enable or disable devkit boot/create texture replacement interception globally.",
            .input_schema = {
                .properties = {
                    {"enabled", {.types = {"boolean"}}},
                },
                .required = {"enabled"},
            },
        },
    },
    {
        "devkit_reload_texture_replace_boot_cache",
        {
            .title = "Reload Texture Replace Boot Cache",
            .description = "Reload pre-decoded PNG replacements from the devkit boot directory for future create_resource initial-data replacements.",
        },
    },
    {
        "devkit_set_texture_replace_rules",
        {
            .title = "Set Texture Replace Rules",
            .description = "Replace the texture-replacement rule list for a device. Rule fields are descriptor/path filters used before any replacement provider is invoked.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"rules", MakeTextureReplaceRulesProperty()},
                },
                .required = {"rules"},
            },
        },
    },
    {
        "devkit_get_texture_replace_state",
        {
            .title = "Get Texture Replace State",
            .description = "Return boot/create texture-replacement enable state, provider state, active rules, observation count, and observed upload stats for a device.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                },
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_list_texture_replace_observations",
        {
            .title = "List Texture Replace Observations",
            .description = "List identified texture upload observations with CRC, format, dimensions, upload path, and hit counters.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"limit", {.types = {"integer"}, .minimum = 1, .maximum = 5000}},
                    {"offset", {.types = {"integer"}, .minimum = 0}},
                },
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_clear_texture_replace_observations",
        {
            .title = "Clear Texture Replace Observations",
            .description = "Clear captured texture upload observations for a device and reset observation hit counters.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                },
            },
        },
    },
    {
        "devkit_dump_texture_replace_observation",
        {
            .title = "Dump Texture Replace Observation",
            .description = "Dump captured bytes for an identified texture observation to disk for offline editing. Use .png for RGBA8/BGRA8 families, .bin for raw bytes, or pass the boot directory to use the canonical boot/create replacement filename.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"observationIndex", {.types = {"integer"}, .minimum = 0}},
                    {"outputPath", {.types = {"string"}}},
                },
                .required = {"observationIndex", "outputPath"},
            },
        },
    },
    {
        "devkit_snapshot_summary",
        {
            .title = "Snapshot Summary",
            .description = "Summarize cached snapshot counts for a device.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                },
            },
            .annotations = {
                .read_only_hint = true,
            },
        },
    },
    {
        "devkit_queue_snapshot",
        {
            .title = "Queue Snapshot",
            .description = "Queue a new snapshot capture for a device on the next present. waitForKey (F1 to F12) first waits, at most 60 s, until the person presses that key in the game (the call fails without capturing if they do not; only one call can wait for a key at a time); delaySeconds (0-30) then waits that long before queueing, so the snapshot can land on gameplay after the game is unpaused; the call returns once it is queued, and other Devkit tool calls wait until then. waitForCompletion also waits, at most 120 s, until the captured frame and its processing are finished and returns that device's snapshot summary (as devkit_snapshot_summary), so no polling is needed; a capture already running when the call comes also ends the wait. It fails when the device is destroyed meanwhile, and at the limit (the request stays queued when the game presented no frame; a running capture keeps capturing; with neither pending, queue a new one). Other Devkit tool calls wait until this call returns.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"delaySeconds", {.types = {"integer"}, .minimum = 0, .maximum = MAX_CAPTURE_DELAY_SECONDS}},
                    {"waitForKey", {.types = {"string"}, .enum_values = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"}}},
                    {"waitForCompletion", {.types = {"boolean"}}},
                },
            },
        },
    },
    {
        "devkit_measure_tone_curve",
        {
            .title = "Measure Tone Curve",
            .description = "Measure the effective transfer of one pass from the current snapshot: drawIndex (a draw with a render target, e.g. the tonemap candidate of devkit_frame_overview), its input (inputSlot, or the first shader resource of the target's size, preferring a float one) and its first render target are read at the same present, each resolved to a mod's active clone when it has one (else the texture itself, so it also works without a mod), and paired pixel by pixel (same size; a region applies to both). Input luminance (inputSignal: raw = BT.709 luminance of the decoded values, scrgb/pq = nits) is split into log-spaced bins (bins 4-128, default 24, between the input's 0.5th and 99.5th percentile; bins with fewer than 16 pixels are left out) and each bin reports its median input and median output (outputSignal: raw, or nits for pq/scrgb). Also: midGray (output at mid grey, a linear 0.18 for a raw input or 14.4 nits for scrgb, none for pq, and the log-log slope there), peakOutputP99_9, highestMedianOutput and shoulderInput (where the median output first reaches 90% of its highest value, when that is positive; null when only the top bin reaches it: the curve rises into the last bin, and a clip or knee inside that bin is not resolved; more bins or a smaller region can). Pixels with any non-finite channel, alpha included, are left out. Depth buffers are never picked as the input. Both textures are read at the end of the frame, so the call refuses when the snapshot shows a later draw, dispatch or copy writing either again (ping-pong chains, UI drawn onto the output; laterInputWriters/laterOutputWriters list them; clears, resolves and uploads are not seen), unless allowLaterWriters (use a region away from what they touch); it also refuses while a snapshot is queued or capturing. Grading, LUTs and local effects (bloom, vignette) also move pixels: it is the pass's overall mapping, not a formula. The textures are read live, so take the snapshot on the same scene. waitForKey/delaySeconds as in devkit_analyze_resource.",
            .input_schema = {
                .properties = {
                    {"deviceIndex", {.types = {"integer"}, .minimum = 0}},
                    {"drawIndex", {.types = {"integer"}, .minimum = 0}},
                    {"inputSlot", {.types = {"integer"}, .minimum = 0}},
                    {"inputSignal", {.types = {"string"}, .enum_values = {"raw", "pq", "scrgb"}}},
                    {"outputSignal", {.types = {"string"}, .enum_values = {"raw", "pq", "scrgb"}}},
                    {"bins", {.types = {"integer"}, .minimum = 4, .maximum = 128}},
                    {"allowLaterWriters", {.types = {"boolean"}}},
                    {"regionX", {.types = {"integer"}, .minimum = 0}},
                    {"regionY", {.types = {"integer"}, .minimum = 0}},
                    {"regionWidth", {.types = {"integer"}, .minimum = 1}},
                    {"regionHeight", {.types = {"integer"}, .minimum = 1}},
                    {"delaySeconds", {.types = {"integer"}, .minimum = 0, .maximum = MAX_CAPTURE_DELAY_SECONDS}},
                    {"waitForKey", {.types = {"string"}, .enum_values = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"}}},
                },
                .required = {"drawIndex"},
            },
        },
    },
};

}  // namespace renodx::addons::devkit::mcp::tool_catalog
