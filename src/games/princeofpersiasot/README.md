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
  down to 8 px (`0x5EA85978`, `0xCC6D19AE`), 3 additive draws (`0xABB07F2E`, ONE/ONE) and a 512 px blur blended at ~41%
  (`0x80AAE9DF`, SRCALPHA/INVSRCALPHA) onto the backbuffer → overlay draws: menu text, and on the pause screen extra blur
  layers drawn from the glow's 8/32/64 px textures (that is the pause screen's soft look).
- The image is SDR and clipped by the 8-bit target. The only HDR information comes from additive / overbright blending
  (fire, light shafts, glow), which exceeds 1.0 once the targets are float16.

## What the mod does

### 1. Display Proxy and float16 targets
- `use_device_proxy`, HDR10 output through `renodx::draw::SwapChainPass`.
- Every `b8g8r8a8_unorm` render target is upgraded to `r16g16b16a16_float` (`ignore_size`, `usage_include = render_target`):
  the backbuffer clone and the whole glow chain keep values above white.
- On its own this was not enough: the glow chain is `ps_1_1`, which clamps at white even into float16 targets, and its
  composite cut every highlight to ~42% of its above-white part (see 4).

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

### 4. Graded SDR bridge around the glow chain
- Found with a deterministic test (temporary debug option replacing the scene with a 0 → 20× SDR white ramp, predicted in
  Python, measured from HDR screenshots): above white the image came out at ~0.42 × scene + 0.58 × white, because the glow's
  `ps_1_1` shaders clamp and its composite blends the clamped blur over the frame. Unclamping the shaders would change the
  game's look, so the mod uses the graded SDR bridge (`handle-sdr-tonemap-lut`, as in the Warrior Within mod):
  - **bridge in** — the game-local `copy_texture_region` handler runs just before the glow's backbuffer copy: the HDR frame
    (scene + HUD + fades) is saved to a float16 texture and the backbuffer gets its SDR version (identical ≤ 1.0, above
    white scaled by the max channel, hue kept; `scene_bridge_ps`). The glow runs exactly as in vanilla.
  - **bridge out** — before the first draw after the glow composite (`0x80AAE9DF`; fallback at present):
    `scene_upgrade_ps` scales the frame by HDR / SDR luminance of the saved frame (`renodx::color::correct::Luminance`),
    so the composite's mix carries over to the HDR values.
- The bridge-out position matters: run at present, it re-sharpened the pause screen's blur layers (sharp white flames
  over the blurred background, white → red → white while the blur faded in). Run right after the composite, the later
  overlays blend over HDR the way they blend over SDR in vanilla; the pause screen matches the SDR original
  (average 8 nits, brightest pixel 132 nits).
- An additive restore (`renodx::tonemap::UpgradeToneMap`) was tried first; the proportional one keeps the composite's
  blend proportions and was verified with the ramp.
- Ramp result, Tone Mapper None: every position within 1–2% of the prediction.

### 5. Tone mapping (3D scene only)
| Tone mapper | Notes |
|---|---|
| Vanilla | Clips at 1.0 like the original 8-bit game, for A/B comparison. |
| None | Untonemapped; anything above Peak is clamped by `SwapChainPass`. |
| RenoDRT | `ToneMapPass`. |
| PsychoV (`psychotm_test17`) | Whitened and flattened the coloured highlights (saturation 0.88 → 0.68, measured before the bridge). Evaluated and dropped. |
| **Neutwo (default)** | `renodx::tonemap::neutwo::MaxChannel(color, peak, clip)`, hue kept. |

- **Neutwo white clip.** `SCENE_MAX_WHITE = 20` (×203 ≈ 4060 nits) is the brightest steady highlight the game produces:
  the courtyard fire measured with Tone Mapper None, Peak 10000, Game Brightness 203, default grading — its frame
  maximum ranged ~2600–3750 nits max channel (CLL) / ~1550–2300 nits luminance; 20× adds a little margin. Rare one-frame
  spikes (up to ~4600 CLL) are not part of the range; they are clamped at Peak. `clip = max(SCENE_MAX_WHITE, peak)`, so
  with Peak at or above ~4060 nits Neutwo is the identity (it never expands). Peak and clip are moved into the
  pre-gamma-correction domain.
- Measured at Peak 1360, Game Brightness 203:
  - ramp with Neutwo: average 980 nits (predicted 992), 4× white 680 (682), 10× 1125 (1142), 15× 1299 (1275);
  - courtyard fire, same camera: whole-image average 11.2 / 11.5 / 11.5 / 11.2 nits (Vanilla / None / RenoDRT / Neutwo).
    Neutwo matches None up to the 99.5th percentile (~100 nits) and rolls off above it as predicted (99.99th percentile:
    None 1856, Neutwo 1054, predicted 1154; the fire flickers between screenshots).
- ACES is left out (it renders white on ps_3_0).

### 6. Settings
- Peak, Game and UI Brightness, Gamma Correction (scene, 2.2), Tone Mapper.
- Color Grading: Exposure, Highlights, Shadows, Contrast, Saturation, Highlight Saturation, Blowout, Flare — the same
  mapping as `ToneMapPass` (RenoDRT) and `renodx::color::grade::config::ApplyUserColorGrading` (None / Neutwo); disabled in
  Vanilla.
- Display Output (advanced): keep its Gamma Correction at None — the scene pass already applies the 2.2 correction.
- Display Proxy "mouse guard": with the proxy two ImGui contexts draw the menu; a hidden sticky setting stops a click from
  landing in both.

## Testing
Palace, courtyard fire, windows room, outdoor sky, dark areas, HUD, pause / profile / main menus, loading screens, in-game
cutscenes, pre-rendered videos, ReShade overlay open/close, tone mapper A/B with measurements, test ramp (None and
Neutwo), pause screen against an SDR screenshot.

## Known behaviour and limitations
- Pre-rendered videos, main menu and loading screens follow UI Brightness (intended; the videos are very low resolution and
  have no marker draw).
- The glow on thin bright shapes (windows) flickers when the camera moves — same in the original game.
- The glow is built from the tone-mapped scene after the scene pass (through the SDR bridge), so it is not tone mapped
  itself.
- Hint text drawn over very bright areas (e.g. the 20× test ramp) looks pale; in normal play it looks like vanilla.
- Not tested yet: the dagger's time effects (rewind, slow motion).

## Upstream candidates
1. x86 `HeapAlloc` alignment fix in `src/utils/platform.hpp`.
2. `copy_texture_region` redirect for D3D9 `surface` sources (backbuffer clone).
3. Display Proxy: two ImGui contexts receive the same mouse input.
