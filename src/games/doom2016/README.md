# DOOM (2016) — RenoDX HDR mod

**Status:** working HDR build for the Vulkan renderer. Not released yet; see [Open items](#open-items).

## Setup
- Steam version, 64-bit, `DOOMx64vk.exe`. The game defaults to OpenGL: set *Settings → Advanced → Graphics API → Vulkan*.
- ReShade 6.7.3 with full add-on support (Vulkan layer build).
- Build the `doom2016` target (x64). It produces `renodx-doom2016.addon64`; copy it next to the game exe.
- Vanilla swap chain: `B8G8R8A8_UNORM`, `SRGB_NONLINEAR`.
- DOOM requests a Vulkan **1.0** instance (ReShade raises it to 1.1); the only device extensions are swapchain and `NV_dedicated_allocation`.

## How the game renders (4K snapshot)
- Scene HDR buffer: `r11g11b10_float`. Bloom chain: `r11g11b10`, 1920 down to 120x67. Auto-exposure: `r16_float`, 128 down to 2x2.
- **`0xF600527E`** (fragment) — main post-process and tone mapper. Grading (luminance-range saturation/gamma/scale), then a Hable-style filmic curve with precomputed coefficients, `pow(gamma)`, white scale, sRGB encode with Bayer dither, `saturate`, and contrast. Writes sRGB-encoded 0..1 into an `r11g11b10` target.
- **`0x49CBC37F`** (fragment) — final pass to the swap chain: bicubic upscale, `saturate`, sRGB decode, UI composite, sRGB encode, `pow(gamma)` (in-game brightness), film grain, colour-blind filters and the "classic 320x240" mode.
- Neither pipeline uses push constants, so RenoDX settings are injected at push-constant offset 0 (16 dwords, pixel|compute).

## How the mod works
- **Shader swap at `vkCreateShaderModule`.** A Detours hook replaces the incoming SPIR-V when its CRC32 matches one of the two hashes. The RenoDX custom shaders are registered with the new code's hash and *empty code*, so RenoDX only pushes settings and never re-creates the pipeline.
  - Why: RenoDX's normal replacement makes ReShade re-create the `VkPipeline`. In DOOM that loses the GPU device (black window, no audio, "Failed to submit immediate command list!"), even with the identical original SPIR-V.
- **Shaders are precompiled SPIR-V 1.0** (`*.frag.spv`), with their sources kept as `*.slang.src` so CMake does not compile them. RenoDX's default Slang output is SPIR-V 1.5, which is invalid on a Vulkan 1.0 device. Recompile with:
  `slangc X.frag.slang -stage fragment -O3 -g0 -entry main -Wno-30056 -Wno-15205 -target spirv -profile spirv_1_0`, then validate with `spirv-val --target-env vulkan1.0`.
  The proxy vertex shader uses `SV_VulkanVertexID`, because `SV_VertexID` needs DrawParameters, which Vulkan 1.0 lacks.
- **Render pass patch** (`vkCreateRenderPass` detour): `B8G8R8A8` colour attachments become `R16G16B16A16_SFLOAT` so render passes match the upgraded swap chain (5 render passes patched).
- **Tone mapping.** "Untonemapped" is the graded scene before the curve, scaled so 0.18 matches the vanilla curve output. The Tone Mapper dropdown offers Vanilla, RenoDRT (default), PsychoV-17 and PsychoV-30. PsychoV uses the repository's shared library (`Doom2016PsychoV` in `shared.h`).
- **Intermediate colour space is BT.2020.** The `r11g11b10` buffer cannot store negative values, so colours beyond BT.709 only survive if stored as BT.2020. The final pass converts the UI to BT.2020 before blending, and the proxy decodes from BT.2020.
- **Swap chain:** `mods::swapchain` v2, `r16g16b16a16_float` scRGB, resource cloning with Slang proxy shaders (SPIR-V 1.0), compatibility mode off.
- **Debug test mode:** `[DOOM2016] TestMode=<bits>` in `ReShade.ini` (1 tonemap, 2 final, 4 swapchain, 8 original SPIR-V, 16 no push constants; default 7). The `original_*.frag.spv` files are the vanilla dumps used for identity tests. Both are leftovers to remove before a release.

## Results
- HDR works and all sliders respond. Peak around 1318 nits on a display rated about 1360, average around 63 nits indoors, BT.709 coverage 100%.
- **Wide gamut:** all of DOOM's own colours are inside BT.709. An Unreal-style gamut expansion did almost nothing and was removed. With the BT.2020 intermediate, the regular **Saturation** slider does push colour beyond BT.709: at 65, about 0.7% of the screen reaches DCI-P3 (fire and blood), with 0% BT.2020 and 0% invalid. Side effect: walls and metal lean slightly steel-blue. Decision: no extra gamut slider; mention in release notes that Saturation above 50 pushes fire and blood past SDR colours.
- **PsychoV-17/30 vs RenoDRT:** look essentially the same in normal gameplay (the grade is transferred first; differences show only in the brightest highlights). Frame rate at 4K is within a few fps, so all are kept as options with RenoDRT as the default.

## Comparison with upstream PR #635 (experimental, Discord build)
| | This mod | PR #635 |
|---|---|---|
| Shader hook | swap SPIR-V at `vkCreateShaderModule`; one pipeline, tone mapper via push constant | detour `vkCreateGraphicsPipelines`; 7 native pipeline variants swapped at bind |
| Render passes | patched to 16F | not patched |
| Output | scRGB 16F | HDR10/PQ `RGB10A2` |
| Tone mappers | Vanilla, RenoDRT, PsychoV-17, PsychoV-30 | adds PsychoV-22/24/25 |
| Build gating | none | GOG exe hash gate |
| Size | ~1,600 lines | ~9,400 lines |

Both use `src/games/doom2016`, so the folder names would clash if upstream merges PR #635.

## Open items
- Look check against vanilla (mid-grey scaling, UI brightness), menus, videos and loading screens, and the in-game Brightness slider.
- Optional: compare with the PR #635 build in-game (it may need ReShade 6.8).
- Optional: upgrade the `r11g11b10` post-process target to `rgba16f` for precision (needs a render pass patch for `B10G11R11` too).
- Release cleanup: remove TestMode and the `original_*.frag.spv` files, tidy the settings text.
