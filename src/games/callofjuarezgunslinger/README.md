# Call of Juarez: Gunslinger — RenoDX mod

HDR mod for **Call of Juarez: Gunslinger** (Steam app 204450, Chrome Engine 5).

- API: **DirectX 9 only**, **32-bit** → build the x86 preset, output is `renodx-callofjuarezgunslinger.addon32`.
- HDR output goes through the RenoDX Display Proxy (D3D11 HDR swapchain), `SwapChainEncoding=4`.
- Shader constants are injected in pixel-shader constants `c50..c61` (`shared.h`).
- Tested at 3840x2160 windowed on an OLED (~1360 nits), RTX 5070 Ti.

## What the mod does

The game renders its scene into float16-capable targets that the stock game keeps at 8 bit. The mod upgrades the full-resolution
`b8g8r8a8` render targets (indices 0..31 except #2) and the back buffer to `r16g16b16a16_float`, then replaces the finishing
shaders so the scene is graded **before** the game's hard clip at white, instead of after it.

### Tone Mapper

| Setting | Meaning |
|---|---|
| Vanilla | The game's own look (clip at white, vanilla grade, vanilla gamma, DOF, heat haze). Preset "Off" also resets every effect slider. |
| Extended (default) | The game's grade (`CoJGradeExt`) applied to the **unclipped** float16 scene, display-mapped with a Neutwo shoulder (`CoJDisplayMap`). |

Extended is a deliberate **deviation from the repository's reference approach** for this class of game (hard clip + 1D LUT:
Silksong, `ToneMapPass(untonemapped, graded_sdr, neutral_sdr)`, see `.agents/skills/handle-sdr-tonemap-lut/SKILL.md`). It is
exact wherever vanilla did not clip (verified with a diff view against the vanilla grade: only tiny specks differ) and extends
the 1D curve LUT above white with the slope of its last segment, clamped to [0.5, 1.0]. The clamp is active in the one scene
measured (real slopes 0.25 R / 0.37 G,B), which also equalises the channels. The ToneMapPass / PsychoV route was **not
evaluated** (project decision, 2026-10-03); it remains a possible later experiment.

### Defaults are an "HDR look", not neutral

Repository convention is neutral-by-default. This mod ships a deliberate look: Sky HDR Boost 22, Sun Brightness 100, Sun Halo 50,
Shadow Lift 150, Glow 100. Preset **Off** gives the vanilla look. The game itself renders only ~2x white at most in normal scenes,
so a neutral HDR default would be almost identical to SDR.

## Pipeline (proven with Devkit traces)

| Stage | Shader | Notes |
|---|---|---|
| Sky | `0x3848A019` | Writes alpha `-1 - a` as a sky marker (glow weight `a`); `0x41AE4161` (glow bright pass) decodes it back. |
| Sun sprite | `0x795E3B26` (VS `0xE0DD40E2`) | Vanilla rgb; in HDR its alpha is negated so it adds to the sky marker. The composite finds the sun as *sky pixels above white*. |
| Composite | `0x7C37DC48` and 63 permutations (`composite.hlsli`) | Vanilla: clip + grade. HDR: grade on the unclipped scene, sun/sky boost, writes HDR into the intentionally 8-bit target #2 with an invertible encoding `sqrt(x/(4+x))`. |
| Final gamma | `0x4003CC02` | Decodes the encoding (`4t/(1−t)`), applies the game gamma, Shadow Lift scales the gamma exponent. Only replaced when a composite ran this frame (menus use another composite). |
| Copy | `0x35B8A99A` | Heat-haze copy (8-bit → needs sRGB encode) and damage-tear copy (float16 → needs sRGB decode). |
| Damage tear | `0x1CF69E72`, `0x03AF484E`, `0x773CC21F` | Re-written to work on the HDR-encoded target #2. |

Target #2 (G-buffer RT0 / composite output) **must stay 8-bit**: geometry and decals blend into it relying on 8-bit clamping, and
float16 blows out sunlit ground.

### Sun

- Sun Brightness / Core Shape / Mask Reach / Falloff / Halo / Halo Reach build a soft radial boost from a blur of the sun
  pixels. The blurs divide by the **sky coverage** they collected, so occluders (the gun) leave no ghost copies.
- **Sun culling (performance):** the sun blurs cost ~2 ms at 4K, so they only run near the sun. The sun sprite's vertex shader is
  `r0 = v0 * c4.xxyz + c4.zzzw; pos = (c0..c3) . r0`; the addon tracks vertex constants through the `push_constants` event and,
  in the sun draw callback, projects the quad centre `v0 = (0,0,1,1)` to get the screen position and a conservative radius
  (`sun_uv_x/y`, `sun_disc_radius`). The composite skips pixels farther than blur reach + radius. Radius 0 (no sun this frame,
  or behind the camera) skips the blurs entirely. Verified with a temporary overlay (red dot = centre, ring = radius).
- Measured at 4K, RTX 5070 Ti, Release build: spot without the sun in view 191 → 300 fps, looking at the sun 300 → 432 fps.

### Resource upgrades

All full-resolution `b8g8r8a8` targets except #2, plus the `b8g8r8x8` back buffer, view cloning on (`use_resource_view_cloning`).
With view cloning an upgraded target shows as the float16 clone when bound as a render target and as the *original 8-bit
resource* when bound as a texture. The upgrade costs ~2.6 ms at 4K; a bisect to upgrade fewer targets is possible but not done.

### Settings

Tone Mapper (Vanilla/Extended), Peak/Game/UI nits, Gamma Correction, Sky HDR Boost, HDR Highlight Start (sky only), Sun Brightness,
Highlight Gain (Extended only, 100 = neutral), Shadow Lift, Depth of Field (vanilla DOF strength, default 0), Glow, the Sun
section (advanced mode), Display Output / Display Proxy options (advanced mode). Preset **Off** + 3 user preset slots.

## Build notes (32-bit)

- Build with the VS Code CMake extension (it provides the Visual Studio environment; plain PowerShell fails at `rc.exe`
  `winver.h`). Select the build preset `clang-x86-release`; targets `callofjuarezgunslinger` and `renodx-devkit`.
- **Profile with Release.** Debug is `/Od /RTC1 /Ob0` and was ~2x slower than Release (85 vs ~190 fps at the same spot).
- Release/RelWithDebInfo launch crashes on x86 were caused by `platform::CreateSharedObject` using `HeapAlloc` (8-byte aligned)
  for a type with cache-line alignment; fixed in `src/utils/platform.hpp` (aligned allocation, commit `e1c8b9a4`).

## Menu mouse guard (Display Proxy)

With the Display Proxy there are two ImGui contexts drawing the Reno menu every frame: the real ReShade window (wide) and a
second, narrow floating "RenoDX" window of the proxy runtime (~286 px wide, ini key `[Window][RenoDX]`). Both read the same
mouse position and edit the same settings, so a click on a reset button in the real menu also hit a slider of the narrow copy
(clicking the reset of Sun Brightness moved HDR Highlight Start, etc.; diagnosed with a per-frame log of ImGui mouse state and
setting changes, which showed the stray change coming from the other context ~3 frames after the press). A hidden, sticky first
entry in the settings list (`mouse guard`) makes any context whose window is narrower than 600 px ignore the mouse. The shared
RenoDX settings code is unchanged; other Display Proxy mods likely have the same issue.

## Known limitations

- The lamp room was not measured for the curve-slope clamp.
- Cutscenes, concentration, fire, night, fog over the sky and every menu were not exhaustively checked with Extended.

## Testing performed

Vanilla vs Extended A/B in several scenes; Extended vs vanilla grade diff view (exact below white); heat haze and damage tear in
both modes; ghost-patch test with the gun in front of the sun (fixed by sky-coverage normalisation); curve-slope check; sun
culling verified visually (no flicker, no visible cut-off) and by fps; 8 consecutive clean launches of the Release build.
