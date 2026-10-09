# Dead Cells — RenoDX HDR mod

**Status:** working, tested, shared with testers as a zip of this folder. Not submitted upstream, by choice.

## Setup
- DX11, **32-bit** (Heaps engine, Haxe). Use the x86 preset: build the `deadcells` target to get `renodx-deadcells.addon32`.
- The Devkit for this game is also the 32-bit build (`clang-x86` preset).

## How the game renders
Dead Cells has no conventional tone mapper. It is a 2D Heaps game that composites sprite layers with about 20 generic pixel shaders. None contain gamma, `pow`/`log`/`exp` curves or highlight compression.

The only place HDR information is destroyed is an 8-bit write:
1. Per-light shaders (draws 016-021) add into the only HDR-range buffer of the frame, `r16g16b16a16_float` 642x362.
2. The lighting composite `0x82BDA5F5` reads that buffer plus the scene and does a soft-light/overlay blend, then writes to an `r8g8b8a8_unorm` target. The UNORM write is the clamp.
3. Many sprite draws keep painting into the same 8-bit buffer, then `0x11A4EA78` applies a heat-shimmer distortion.
4. `0x48C1C006` is an upscale blit to the backbuffer. **The same hash is reused for most HUD/UI icons**, so a hash-based replacement has to tell the two roles apart.
5. HUD and UI draw directly onto the backbuffer at native resolution.

## What the mod does
- Swap chain upgraded to `r10g10b10a2_unorm` / HDR10 by default (swap chain proxy with back-buffer clone, no Display Proxy); scRGB selectable. `swapchain_proxy_revert_state` is on (see Resolved). Resource upgrade: `r8g8b8a8_unorm` becomes `r16g16b16a16_float` (any size, render targets, view cloning), set in `DllMain`.
- Active shader replacements:
  - `0x48C1C006` — world composite. Gated so only the first draw of the frame, and only when `t0` is a render target, is replaced. This is what separates Game Brightness (world only) from UI Brightness (UI only).
  - `0x8F0EAF1C` — main sprite/smoke shader. Outputs are clamped (alpha, distortion, glow, light scatter) so smoke stays clear.
  - `0x991A7AE4` — 5-tap glow blur (runs twice on the glow layer). Taps and output clamped to 0..1. In the original the glow layer is 8-bit, so additive sprite blends into it stop at 1; after the float16 upgrade its alpha summed to 3.5-7.2 across the frame, and `0x40BF5761` blends with source alpha, so the glow reached the scene several times too bright (weapon shine looked like a "lightsaber": sword peak 5.3x white). With the clamp the layer's alpha is back to 1.0 (Devkit readback) and the sword peak measured 3.7x white at Glow Strength 100; what remains above white is the sword sprite in the scene itself (1.6x with no glow), i.e. real HDR range.
  - `0x40BF5761` — glow add, multiplied by the **Glow Strength** slider (0-100, default 100 = original).
  - `0x0A271311` — minimap/map. `saturate()` so the map does not hit peak brightness.
- Settings confirmed working: tone mapper type, Game Brightness, UI Brightness, Blowout, Saturation, Glow Strength.

## Tone mapping (2026-10-09)
The game has no tone curve: the scene is hard clipped at white by the 8-bit target written from the lighting composite
`0x82BDA5F5` on. With the float upgrade, values above white reach `0x48C1C006`, where the mod tone maps.

| Tone Mapper | What it does |
|---|---|
| Vanilla | `saturate()` before `ToneMapPass`: the original 8-bit clip, for A/B. Grading sliders still apply. |
| None | Untonemapped; anything above Peak is clamped by `SwapChainPass`. |
| **RenoDRT (default)** | `ToneMapPass`, RenoDRT method **Neutwo**, white clip **7.0** (brightest steady scene value, see `shared.h`). |

- Source range (measured, no tone mapping, Peak 4000): the brightest steady light, a lit window, reaches 1249 nits CLL at
  Game 203 = ~6.8x white in scene units. Character glow is far below that.
- Default Hue Shift (50, clip method) keeps the vanilla hue of clipped lights: the window is pale yellow in vanilla (red and
  green both clip); its unclipped colour is orange. Neutral-by-default = keep the yellow.
- Measured at Peak 1360, Game 203, same spot (HDR Analysis + decoded HDR screenshots):
  - before (RenoDRT Reinhard, default white clip 100): avg 22 nits vs 28 unclamped, max 613. Too dark, highlights cut.
  - now (RenoDRT Neutwo, clip 7): avg 25–27, max ~1011 nits, CLL 1354 (Peak). The window light pulses, so readings vary.
- Evaluated and dropped: an exponential **Roll-off** on the max channel (same avg and peak, but kept the unclipped
  orange instead of the vanilla yellow); ACES (removed from the list, `.parse` maps index 2 to RenoDRT).
- PsychoV: not evaluated in game. The range is small (~7x white, additive light); in the Prince of Persia mods PsychoV
  whitened and flattened coloured highlights from a similar source, and the default must keep the vanilla look.

## Lessons from building it
1. **Frame counter never reset.** The generic template only registers `OnPresent` when Display Proxy is on, so the per-frame counter stayed at frame 0 and only one draw per session was replaced. The reset now lives in a callback registered unconditionally.
2. **Sliders disconnected.** `common.hlsl` must `#include "./shared.h"`, which declares the `shader_injection` cbuffer and the `RENODX_*` macros. Including only `renodx.hlsl` makes every slider silently fall back to defaults.
3. **Input signature must match the original.** The replacement was missing `SV_POSITION`, so every input shifted by one register. A forced-colour test "passed" only because alpha came from `position.w`; do not trust constant-colour tests when inputs may be misaligned.
4. **Do not force alpha = 1.** The world composite is alpha-blended over a background layer drawn by another shader.
5. **Settings do nothing without replacement shaders.** Tone mapper, grading and brightness settings only run inside replacement shader code. With zero custom shaders attached, only the swap chain-level settings (peak brightness, encoding) work.

## Shader roles found (stable across snapshots; draw numbers are not)
`0x8F0EAF1C` smoke / main sprite · `0x82BDA5F5` lighting composite · `0x5D4017CA` per-light accumulator · `0x01A7A161` glow/light-scatter G-buffer (4 render targets) · `0x6AE9A56B` colour-matrix tint · `0xD38C1AE4` radial light pool · `0x11A4EA78` heat distortion · `0x0A271311` minimap · `0x48C1C006` world blit and HUD icons · `0x40BF5761` glow add · `0x991A7AE4` 5-tap glow blur · `0xF3928D92` lights (also draws the character head) · `0x9FBDC40F` character body.

## Open items
- Tester feedback welcome.

## Resolved (2026-10-09)
- Light-blue screen at game start (1-2 s before the main menu) and stray white/blue speckles on the character in gameplay. Cause: the swap chain proxy pass (RenoDX present handler) leaves its own state bound - render targets, shaders, blend, and pixel texture/sampler slot 0. During the startup load stall Heaps issues its first draw of each frame without re-binding t0, so it read the proxy's back-buffer clone; on the HDR10 path that clone is also the render target, so D3D11 unbound it, the draw painted nothing and the light-blue clear colour (0.67, 0.90, 1.0) showed (with scRGB it read a copy of the last frame by luck). **Fix: `swapchain_proxy_revert_state = true`.** Since RenoDX's 2026-09-29 state rewrite this snapshots the game's state before the proxy pass and restores it after, including D3D11 push_descriptors (SRVs, samplers). (On the older base it did not restore push_descriptors, so the add-on had to save/restore pixel slot 0 itself; that code was removed after rebasing.) Startup is now black like vanilla, and the character speckles are gone (same spot A/B: mod before / mod after / vanilla). Found with logs: back-buffer readback before/after the proxy pass, per-draw bound RT and SRVs, RenoDX view-list lookup.
- Light-blue flash between menu and level: caused by the mod's manual re-bind of the original `0x48C1C006` pipeline when a draw was not replaced (the loading-screen draws vanished). Removed; HUD still follows UI Brightness. Found by A/B: replacement off (flash gone), replacement on without the re-bind (flash gone).
- Background: a Devkit snapshot shows every layer, background included, is drawn into the 642x362 scene first; the gated `0x48C1C006` draw that tone maps it is the first, opaque (blend off) draw on the back buffer. In-game check: the background follows Game Brightness, the HUD follows UI Brightness.
- Peak Brightness: tested at 1360 nits (OLED). HDR Analysis in a level: max 430-600 nits, MaxCLL 691; nothing reaches Peak, so nothing is clipped.
- Release build: 32-bit Release builds need the x86 HeapAlloc alignment fix in `src/utils/platform.hpp` (commit on this branch); without it optimized x86 builds can crash at launch.
