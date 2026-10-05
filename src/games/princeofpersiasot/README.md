# Prince of Persia: The Sands of Time (2003) — RenoDX

GOG build (August 2026 patch), Direct3D 9, 32-bit (`renodx-princeofpersiasot.addon32`). Game exe `gpp.exe`.

## Install
- ReShade with full add-on support as `d3d9.dll` in the game folder.
- The patched GOG exe loads Direct3D 9 through GOG's `dx.dll` wrapper, which bypasses a local `d3d9.dll`.
  Add `'d3d9.dll' = 'd3d9.dll'` under `[DllSubstitution]` in `gog.toml` so ReShade is loaded.
- Copy `renodx-princeofpersiasot.addon32` next to `gpp.exe`.

## Pipeline (vanilla)
- No tone mapper, no LUT, no float scene target. Almost the whole frame is fixed-function (vs_1_1 / ps_1_1).
- Frame order: 3D scene into the A8R8G8B8 backbuffer → marker draw (vs `0x859585C3` clip-space colour quad +
  ps `0x9A0AF728`, used for fades/dimming) → HUD / menus → glow: `StretchRect(backbuffer → 512x512)`, blur chain
  512…8 px (ps `0x5EA85978`, `0xCC6D19AE`), additive composite (ps `0xABB07F2E`, `0x80AAE9DF`) → last overlay draws.
- The only values above 1.0 come from additive / overbright blending once the targets are float16.

## Mod architecture
- Display Proxy: DX9 renders, RenoDX presents through a D3D11 HDR10 swapchain (`SwapChainPass`).
- Upgrade: every `b8g8r8a8_unorm` render target → `r16g16b16a16_float` (backbuffer clone and the glow chain).
- Glow copy fix: RenoDX's copy redirect only handles textures, the D3D9 backbuffer is a surface, so the glow copied
  the stale original backbuffer (it only contained ReShade's overlay → "ghost" menus burned into the image).
  The addon redirects that `copy_texture_region` to the float16 clone with a native `StretchRect`.
- Scene finish pass (game-local, new): before the first marker draw of a frame the addon copies the backbuffer clone
  and redraws it with `scene_finish_ps.ps_3_0` (sRGB decode → tone mapper → user grading → `RenderIntermediatePass`).
  The 3D scene ends up at Game Brightness, everything drawn afterwards (HUD, menus, fades) at UI Brightness.
  Native D3D9 state is saved/restored with a state block; resources are released on swapchain/device destroy (Reset).

## Settings
- Tone Mapper (3D scene only): Vanilla (clip at 1.0 like the 8-bit original), None, RenoDRT, **Neutwo (default)**.
  ACES is omitted (white screen on ps_3_0).
- Peak / Game / UI Brightness, Gamma Correction (scene, default 2.2), Color Grading sliders (Exposure, Highlights,
  Shadows, Contrast, Saturation, Highlight Saturation, Blowout, Flare; disabled for Vanilla).
- Display Output (advanced): keep its Gamma Correction at None — the scene pass already applies the 2.2 correction.

## Tone mapping evaluation (measured from HDR screenshots, peak 1360, game white 203)
- The game's highlights are SDR colours pushed past white, mostly 1–4x paper white (fire ≈ 850 nits with None).
- RenoDRT compressed fire about 2x (max ~405 nits) and lost gradation.
- PsychoV (`psychotm_test17`) whitened and flattened the coloured highlights (saturation 0.88 → 0.68, fewest distinct
  shades) — evaluated and dropped.
- Neutwo (`neutwo::MaxChannel`) is near-identical to None below ~half of peak and only rolls off towards peak.

## Known behaviour / limitations
- Pre-rendered videos have no marker draw and stay at UI Brightness (intended; the videos are very low resolution).
- Main menu and loading screens follow UI Brightness.
- The glow on thin bright shapes (windows) flickers when the camera moves — same in the original game.
- The glow is added after the scene pass, so it is not tone mapped (it is built from the tone-mapped scene).

## Testing done
Gameplay (palace, courtyard fire, windows room, outdoor sky), HUD, pause/profile menus, in-game cutscenes, videos,
dark areas, ReShade overlay open/close. Not yet: dagger time effects (rewind, slow motion).
