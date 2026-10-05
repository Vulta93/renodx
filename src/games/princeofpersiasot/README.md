# Prince of Persia: The Sands of Time (2003) — RenoDX HDR mod

HDR mod for the GOG release of *Prince of Persia: The Sands of Time* (August 2026 patch, `gpp.exe`), Direct3D 9, 32-bit
(`renodx-princeofpersiasot.addon32`). Built on RenoDX's Display Proxy: the game keeps rendering in D3D9 and RenoDX presents
the image through a D3D11 HDR10 swapchain.

## Install
- ReShade with full add-on support, installed as `d3d9.dll` in the game folder.
- The patched GOG exe loads D3D9 through GOG's `dx.dll` wrapper, which bypasses a local `d3d9.dll`.
  Add `'d3d9.dll' = 'd3d9.dll'` under `[DllSubstitution]` in `gog.toml` so ReShade is loaded.
- Copy `renodx-princeofpersiasot.addon32` next to `gpp.exe`.
- Recommended: `SkipLoadingDisabledEffects=1` in the DX9 runtime's `ReShade.ini` (it has no use for effects with the proxy).

## Build
Place these files in `src/games/princeofpersiasot` of a RenoDX checkout, run CMake Configure and build the
`princeofpersiasot` target with the x86 preset (`clang-x86-release`, output in `build32`). The checkout must contain an
x86 alignment fix for `src/utils/platform.hpp`: the Windows heap only guarantees 8-byte alignment on 32-bit, and RenoDX's
shared objects assume 16, which crashes optimized 32-bit builds at launch.

## How the original game renders
Established with the RenoDX Devkit (snapshots, vs_1_1/ps_1_1 disassembly) and one-frame traces logged from ReShade events.
- No tone mapper, no LUT and no floating-point scene buffer. About 95% of the ~750 draws per frame are fixed-function; the
  rest use tiny `vs_1_1` / `ps_1_1` shaders.
- Frame order: 3D scene into the 8-bit `A8R8G8B8` backbuffer → marker draw (vs `0x859585C3` clip-space colour quad +
  20-byte ps `0x9A0AF728`, used for fades/dimming) → HUD and menus → glow: `StretchRect(backbuffer → 512×512)`, blur chain
  down to 8 px (`0x5EA85978`, `0xCC6D19AE`), additive composite onto the backbuffer (`0xABB07F2E`, `0x80AAE9DF`) → last
  overlay draws.
- The image is SDR and clipped by the 8-bit target. The only HDR information comes from additive / overbright blending
  (fire, light shafts, glow), which exceeds 1.0 once the targets are float16.

## What the mod does

### 1. Display Proxy and float16 targets
- `use_device_proxy`, HDR10 output through `renodx::draw::SwapChainPass`.
- Every `b8g8r8a8_unorm` render target is upgraded to `r16g16b16a16_float` (`ignore_size`, `usage_include = render_target`):
  the backbuffer clone and the whole glow chain keep values above white.
- Result: fire and torches reach about 850–1000 nits instead of being clipped at paper white.

### 2. Fix: ReShade overlay "burned" into the image
- Symptom: after opening the ReShade menu (or at the startup loading bar) a blurred copy of it stayed in the picture.
- Root cause: with the Display Proxy the game draws into a float16 clone of the swapchain backbuffer. RenoDX's
  `copy_texture_region` redirect only handles `texture_2d` / `texture_3d` resources, but the D3D9 backbuffer is a `surface`.
  The glow's `StretchRect(backbuffer → 512×512)` therefore read the original backbuffer, which only held what ReShade's
  overlay last drew, and the glow blurred that stale image and added it to every frame (the vanilla glow was missing too).
- Fix: a game-local `copy_texture_region` handler, registered before RenoDX's: when the source is a surface with an active
  clone, it copies natively with `StretchRect(clone → destination)`, the technique `utils::device_proxy` uses for its own
  DX9 handoff.

### 3. Separate Game and UI brightness (scene finish pass)
- RenoDX normally applies `RenderIntermediatePass` in the game's last scene shader. This game has none (scene and HUD share
  one buffer, the HUD is fixed-function), so this is a small, game-local addition.
- On the first marker draw of each frame (both hashes must match; the pixel shader is reused later) the addon copies the
  backbuffer clone into a float16 texture, saves the D3D9 state in a `D3DSBT_ALL` state block, redraws the scene with
  `scene_finish_vs.vs_3_0` + `scene_finish_ps.ps_3_0` (sRGB decode → tone mapper → user grading →
  `renodx::draw::RenderIntermediatePass`) and restores the state.
- Everything drawn afterwards (HUD, menus, fades) stays at UI Brightness; `SwapChainPass` scales by UI white.
- SM3 constants follow the DX9 pattern of `batmanaa`: `float4 shader_injection[8] : register(c50)`, uploaded by the addon.
- D3DPOOL_DEFAULT resources and the state block are released on `destroy_swapchain` / `destroy_device` (device Reset).

### 4. Tone mapping (3D scene only)
Measured from HDR ReShade screenshots (PQ decoded to nits), peak 1360 nits, game white 203 nits:

| Tone mapper | Fire, brightest pixel | Notes |
|---|---|---|
| None | ~856 nits | Nothing reaches peak in normal scenes; full gradation. |
| RenoDRT | ~405 nits | Compresses highlights about 2× and loses gradation. |
| PsychoV (`psychotm_test17`) | ~484 nits | Whitens and flattens coloured highlights (saturation 0.88 → 0.68). Evaluated and dropped. |
| **Neutwo (default)** | ≈ None | `renodx::tonemap::neutwo::MaxChannel`: near-identity below about half of peak, then rolls off to peak, hue kept. |

Vanilla clips at 1.0 like the original 8-bit game, for A/B comparison. ACES is left out (it renders white on ps_3_0).

### 5. Settings
- Peak, Game and UI Brightness, Gamma Correction (scene, 2.2), Tone Mapper.
- Color Grading: Exposure, Highlights, Shadows, Contrast, Saturation, Highlight Saturation, Blowout, Flare — the same
  mapping as `ToneMapPass` (RenoDRT) and `renodx::color::grade::config::ApplyUserColorGrading` (None / Neutwo); disabled in
  Vanilla.
- Display Output (advanced): keep its Gamma Correction at None — the scene pass already applies the 2.2 correction.
- Display Proxy "mouse guard": with the proxy two ImGui contexts draw the menu; a hidden sticky setting stops a click from
  landing in both.

## Testing
Palace, courtyard fire, windows room, outdoor sky, dark areas, HUD, pause / profile / main menus, loading screens, in-game
cutscenes, pre-rendered videos, ReShade overlay open/close, tone mapper A/B with measurements.

## Known behaviour and limitations
- Pre-rendered videos, main menu and loading screens follow UI Brightness (intended; the videos are very low resolution and
  have no marker draw).
- The glow on thin bright shapes (windows) flickers when the camera moves — same in the original game.
- The glow is added after the scene pass, so it is not tone mapped itself (it is built from the tone-mapped scene).
- Not tested yet: the dagger's time effects (rewind, slow motion).

## Upstream candidates
1. x86 `HeapAlloc` alignment fix in `src/utils/platform.hpp`.
2. `copy_texture_region` redirect for D3D9 `surface` sources (backbuffer clone).
3. Display Proxy: two ImGui contexts receive the same mouse input.
