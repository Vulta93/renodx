# RenoDX for Godot Engine

Work in progress: an engine-wide addon for Godot games. On Godot 4.6 Forward+ with D3D12 it replaces Godot's tone
mapper, keeps a float viewport and outputs HDR10.

## Scope

| Godot renderer / API | Status |
|---|---|
| Godot 4.6 Forward+ on D3D12 | tone mapper replaced, float viewport target, HDR10 output |
| Godot 4 Forward+ on D3D12, other versions | float viewport and HDR10 only (tone mapper hashes not registered yet) |
| Godot 4 Forward+ on Vulkan | not handled yet: the addon leaves these games untouched |
| Godot 4 Mobile | not tested |
| Godot 4.7 with native HDR output | not handled yet (planned: stand down) |
| Godot 3 GLES3/GLES2, Godot 4 Compatibility (OpenGL) | not handled yet: the addon leaves these games untouched |

## What it does

- Swap chain: RenoDX swap chain proxy, HDR10 (D3D12 only; every other API is in `ignored_device_apis`). A Godot game
  rendering with Vulkan can still reach ReShade as D3D12: NVIDIA's "Vulkan through DXGI" present path shows only the
  driver's D3D12 device, which copies the finished 8-bit image into the swap chain, and modifying that swap chain gives
  a black screen. The addon therefore leaves every device alone once `vulkan-1.dll` is loaded (Godot loads it only
  when it renders with Vulkan).
- Resource upgrade: every `r8g8b8a8_typeless` render target or UAV texture as wide as the swap chain is cloned to
  `r16g16b16a16_float`. In Godot 4 this is the viewport target (written by the tone mapper or by FSR/SMAA, then by the
  2D/UI canvas, read by the final blit) and, at 100% 3D scale with SSAO/SSIL/SSR, the normal/roughness buffer, which
  Godot creates with an identical description. The width (not the size or aspect ratio) is what is matched: some
  windows are 2 px taller than the viewport, and narrower SubViewport targets must stay 8-bit because their views
  force alpha to 1 or decode sRGB, which a D3D12 clone view does not keep.
- Tone mapper: Godot 4.6's `tonemap.glsl` (TonemapShaderRD) has four desktop Forward+ variants, all replaced by
  hash (`tonemap/`): `0x0656E0CE` (3D LUT, bilinear glow), `0xBF1B5017` (bicubic glow), `0x667C84AD` (1D LUT),
  `0x6291BC2F` (1D LUT, bicubic glow). The Devkit could not decompile the DXIL, so `tonemap/tonemap.hlsl` is a port of
  Godot's MIT GLSL source with the original register layout; every per-hash file only sets Godot's two variant defines.
  Tone mapper, glow mode and adjustments are push-constant branches inside each variant.
- Signals: `untonemapped` = scene after exposure, auto exposure, FXAA and pre-tonemap glow. `graded_sdr` = Godot's
  operator (Reinhard, Filmic, ACES, AgX) and everything after it (soft-light glow, brightness/contrast/saturation in
  sRGB, 1D/3D colour correction LUT), clamped like the 8-bit target and decoded to linear. The bridge is
  `ToneMapPass(untonemapped, graded_sdr)` with RenoDRT's neutral SDR, so midtones keep Godot's curve and highlights
  extend. Linear and Reinhard with white 1 only clip at the 8-bit write: there the vanilla chain runs on RenoDRT's
  neutral SDR instead of the clipped value. RenoDRT uses Neutwo for both the HDR curve and its neutral SDR.
- Output: the tone mapper keeps Godot's sRGB-shaped encoding (`RenderIntermediatePass`) so the 2D canvas and UI
  blend over it as in vanilla; the swap chain proxy decodes it with `SwapChainPass`. Godot's 8-bit debanding dither
  is dropped outside Vanilla (the target is float).
- Only tone map draws into a float target are replaced (the viewport clone, Godot's RGBA16F tonemap destination
  before FSR/SMAA, HDR 2D viewports); SubViewports keep 8-bit targets and Godot's original shader.
- Vanilla tone mapper = Godot's chain exactly (debanding dither included), plus the 8-bit clamp; Preset Off selects
  it. HDR 2D viewports always get the Vanilla chain (see limitations).
- Constants: Godot's tonemap root signature already uses 36 of 64 DWORDs, so the RenoDX constants are bound as a root
  CBV (`use_root_signature_cbv`).

## PsychoV

Not offered yet. Godot's curves are analytic (Reinhard, Filmic, ACES, allenwp/AgX), so output-midgray and slope
anchors are solvable for them; Linear and Reinhard white 1 have no curve to anchor to.

## Build

Target `godotengine` → `renodx-godotengine.addon64`.

## Testing

Tested on Godot 4.6.1 (Road to Vostok demo) and on a Godot 4.6.3 test project, D3D12, NVIDIA.

1. Install ReShade with add-on support and the addon next to the game exe.
2. `ReShade.log` should show `mods::swapchain::OnCreateSwapchain(swap: r8g8b8a8_unorm => r10g10b10a2_unorm` and,
   for a Vulkan or OpenGL game, `Abort from ignored device api` instead (with NVIDIA's DXGI present path also
   `Godot renders with Vulkan (vulkan-1.dll is loaded): device left untouched.`), and the game must look like vanilla.
3. With the Devkit: the viewport target (the input of the final blit) reports `cloneActive` with
   `cloneFormat r16g16b16a16_float`; no NaN in it.
4. Compare with the game without the addon at the same spot: the image must match below white, including UI,
   SubViewports, SSAO and anti-aliasing modes.
5. Tone mapper: `ReShade.log` shows `Registered runtime replacement: 0x0656e0ce` (and the other three); with the
   Devkit, `devkit_list_replacements` shows the tonemap draw replaced. Vanilla tone mapper vs the original shader
   (per-pixel EXR of the viewport clone, 25 test-project steps: every operator and white, every glow mode with
   bilinear and bicubic upscale, adjustments, 3D and 1D LUT): max error 0.19/255, no pixel over 1/255, no NaN.
6. RenoDRT (default) vs Vanilla on the test project's ramp (grey row, display light after 2.2): Linear within 1%
   up to scene 0.5; Filmic and ACES within 1% up to mid grey, +8% at 0.5; AgX +15% at 0.5 (its shoulder starts
   early). Peak reaches the Peak setting (999 nits max channel at 1000); UI white stays at UI Brightness (202.9
   nits at 203) in every step; no NaN.

Known limitations:
- HDR 2D viewports (`use_hdr_2d`): Godot's final blit converts those to sRGB and clamps, so they keep the vanilla
  tone mapper (SDR in HDR10) until that blit is replaced.
- 3D SubViewports stay SDR (8-bit targets, original tone mapper). Untested: a SubViewport with FSR or SMAA, whose
  tone mapper may write Godot's float intermediate and so get the HDR result, clipped later by its 8-bit target.
- Godot's own HDR curves (Reinhard/AgX with `output_max_value`, Godot 4.7) are not offered as a tone mapper yet.
- FSR1 sharpening now leaves small negative values (about -0.01) that the 8-bit target used to clip.
- A D3D12 game is also left untouched when `vulkan-1.dll` is in the process for another reason: Godot's driver
  fallback from Vulkan to D3D12, or a tool that injects Vulkan into a D3D12 game. The Steam overlay did not (Road to
  Vostok demo, D3D12, converted normally).
- Only tested on NVIDIA; on AMD/Intel a Vulkan game should log only `Abort from ignored device api`.
