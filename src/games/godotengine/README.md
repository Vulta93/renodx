# RenoDX for Godot Engine

Work in progress: an engine-wide addon for Godot games. On Godot 4.3 to 4.7 Forward+ with D3D12 it replaces
Godot's tone mapper and final blit, keeps a float viewport and outputs HDR10.

## Scope

| Godot renderer / API | Status |
|---|---|
| Godot 4.6, 4.7 Forward+ on D3D12 | tone mapper and final blit replaced, float viewport target, HDR10 output |
| Godot 4.3, 4.4, 4.5 Forward+ on D3D12 | same, but only with the `override.cfg` workaround below (otherwise ReShade does not hook the device) |
| Godot 4.7 with its own HDR output on (`Window.hdr_output_requested`) | left to Godot (swap chain and tone mapper) |
| Godot 4 Forward+ on Vulkan | not handled yet: the addon leaves these games untouched |
| Godot 4 Mobile | not tested |
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
- Tone mapper: `tonemap.glsl` (TonemapShaderRD) has four desktop Forward+ variants per Godot version, all replaced
  by hash (`tonemap/`). 4.6: `0x0656E0CE` (3D LUT, bilinear glow), `0xBF1B5017` (bicubic glow), `0x667C84AD` (1D LUT),
  `0x6291BC2F` (1D LUT, bicubic glow). 4.7: `0xB21EACEC`, `0x3F415A05`, `0x32F7250B`, `0xEF76A934` in the same
  order. 4.3: `0x914EEDB6`, `0x51BDE35D`, `0xC200205E`, `0xA71CD15D`; 4.4: `0xA0DD4706`, `0x4A3080B2`,
  `0x9E1957A5`, `0xFDC2A39E`; 4.5: `0x2D4AED1E`, `0xD44A3465`, `0x19657F6D`, `0xF01F42A0`. The Devkit could not decompile the DXIL, so `tonemap/tonemap.hlsl` is a port of Godot's MIT GLSL source
  (4.3 to 4.7) with the original register layout; each per-hash file only sets `GODOT_VERSION` and Godot's two variant
  defines. Tone mapper, glow mode and adjustments are push-constant branches inside each variant. Before 4.6 the
  operator output is clamped by the sRGB encode and glow (except mix) is blended after it; the port follows that.
- Final blit (`blit/`, 4.3 `0x22687DAC`, 4.4 `0xA252A18D`, 4.5 `0x2845C867`, 4.6 `0x83ADBE18`, 4.7 `0x13955D03`): for HDR 2D viewports Godot encodes linear to sRGB here
  and clamps (4.3/4.4 before the encode, 4.5/4.6 after the dither) or relies on the 8-bit swap chain to clamp (4.7). Outside Vanilla the replacement drops the clamp
  and the 8-bit dither, so HDR 2D viewports get HDR too.
- Signals: `untonemapped` = scene after exposure, auto exposure, FXAA and pre-tonemap glow. `graded_sdr` = Godot's
  operator (Reinhard, Filmic, ACES, AgX) and everything after it (soft-light glow, brightness/contrast/saturation in
  sRGB, 1D/3D colour correction LUT), clamped like the 8-bit target and decoded to linear. The bridge is
  `ToneMapPass(untonemapped, graded_sdr)` with RenoDRT's neutral SDR, so midtones keep Godot's curve and highlights
  extend. Linear and Reinhard with white 1 (4.4+) only clip at the 8-bit write: there the vanilla chain runs on RenoDRT's
  neutral SDR instead of the clipped value. RenoDRT uses Neutwo for both the HDR curve and its neutral SDR.
- Tone mappers: Vanilla, None, ACES, RenoDRT (default) and Godot HDR. Godot HDR is Godot 4.7's own HDR curve
  (Reinhard with white above 1 and AgX take `output_max_value` = Peak / Game Brightness, with the parameters Godot's
  CPU derives from it) and the vanilla grade carried over with `UpgradeToneMap`; it falls back to RenoDRT where Godot
  has no HDR curve (Linear, Filmic, ACES, Reinhard white 1, and every version before 4.6).
- Output: the tone mapper keeps Godot's encoding (`RenderIntermediatePass`; sRGB-shaped on an SDR viewport, linear
  on an HDR 2D one) so the 2D canvas and UI blend over it as in vanilla; the swap chain proxy decodes it with
  `SwapChainPass`. Godot's 8-bit debanding dither is dropped outside Vanilla (the target is float).
- Only tone map draws into a float target are replaced (the viewport clone, Godot's RGBA16F tonemap destination
  before FSR/SMAA, HDR 2D viewports); SubViewports keep 8-bit targets and Godot's original shader.
- Vanilla tone mapper = Godot's chain exactly (debanding dither included), plus the 8-bit clamp; Preset Off selects
  it.
- Godot 4.7 native HDR: once Godot creates a `r16g16b16a16_float` swap chain the addon leaves the game alone for the
  rest of the session (swap chain, blit and tone mapper; restart the game after turning Godot's HDR off), and the
  tone mapper runs Godot's own chain whenever `output_max_value` is above 1. The Vulkan stand-down also disables the
  shader replacements.
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
7. Godot 4.7.2 test project: Vanilla vs Godot's shader within 0.62/255 outside the UI (8-bit reference) on 9 steps
   covering all four variants. Native HDR (`hdr_output_requested`): `ReShade.log` shows `Godot native HDR swap chain
   (r16g16b16a16_float): left to Godot.` and the output equals Godot without the addon (max 337.5, mean 59.95 nits).
8. HDR 2D viewport (`use_hdr_2d`) with RenoDRT: same output as the SDR viewport (peak 999 nits, UI 202.9 nits).
9. Godot HDR tone mapper: AgX identical to Vanilla up to mid grey; Reinhard white 6 lifts mid grey by 18% (Godot's
   own HDR Reinhard does). Its grading sliders do nothing (they belong to RenoDRT).
10. Godot 4.3, 4.4.1, 4.5.1 test exports (with the `override.cfg`): Vanilla vs Godot's shader within 0.62/255
   outside the UI on 8-9 steps each (every operator incl. 4.3's own Reinhard, glow, 3D/1D LUT); RenoDRT peak 999
   nits, UI 202.9 nits, no NaN.

Known limitations:
- 3D SubViewports stay SDR (8-bit targets, original tone mapper), except with SMAA or 3D scaling (FSR): their tone
  mapper then writes Godot's float intermediate, gets the HDR result, and the 8-bit SubViewport target clips it per
  channel (measured on the test project; the image differs from vanilla only in highlights).
- HDR 2D viewports outside Vanilla: 2D content above 1 (bright canvas items, 2D glow) now reaches the display above
  UI Brightness instead of being clipped.
- Godot 4.3-4.5 create their D3D12 device through the Agility SDK device factory (`ID3D12DeviceFactory::
  CreateDevice`), which ReShade 6.8 does not hook: ReShade logs `Skipping swap chain because it was created without
  a proxy Direct3D device.` and no add-on loads. Workaround: an `override.cfg` next to the game exe with
  `[rendering]` / `rendering_device/d3d12/agility_sdk_version=99999` makes the factory fail, Godot falls back to
  `D3D12CreateDevice` and ReShade hooks it (tested on the 4.3, 4.4.1 and 4.5.1 test exports). The 4.6.3 and 4.7.2 exports
  reached `D3D12CreateDevice` without it on the test machine (4.6 no longer swaps in the system factory), but a 4.6+
  game whose Agility factory succeeds would need the same `override.cfg`.
- FSR1 sharpening now leaves small negative values (about -0.01) that the 8-bit target used to clip.
- A D3D12 game is also left untouched when `vulkan-1.dll` is in the process for another reason: Godot's driver
  fallback from Vulkan to D3D12, or a tool that injects Vulkan into a D3D12 game. The Steam overlay did not (Road to
  Vostok demo, D3D12, converted normally).
- Only tested on NVIDIA; on AMD/Intel a Vulkan game should log only `Abort from ignored device api`.
