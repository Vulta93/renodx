# RenoDX for Godot Engine

Work in progress: an engine-wide addon for Godot games. This first milestone gives HDR10 output and a float viewport
on Godot 4 D3D12; it does not replace Godot's tone mapper yet, so highlights still stop at what Godot's own tone
mapper writes.

## Scope

| Godot renderer / API | Status |
|---|---|
| Godot 4 Forward+ on D3D12 | HDR10 output, float viewport target (this milestone) |
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
- No shader replacements yet.

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

Known limitations:
- FSR1 sharpening now leaves small negative values (about -0.01) that the 8-bit target used to clip.
- A D3D12 game is also left untouched when `vulkan-1.dll` is in the process for another reason: Godot's driver
  fallback from Vulkan to D3D12, or a tool that injects Vulkan into a D3D12 game. The Steam overlay did not (Road to
  Vostok demo, D3D12, converted normally).
- Only tested on NVIDIA; on AMD/Intel a Vulkan game should log only `Abort from ignored device api`.
