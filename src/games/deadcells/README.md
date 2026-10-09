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
- Display Proxy and resource upgrade: `r8g8b8a8_unorm` becomes `r16g16b16a16_float` (any size, render targets, view cloning), set in `DllMain`.
- Active shader replacements:
  - `0x48C1C006` — world composite. Gated so only the first draw of the frame, and only when `t0` is a render target, is replaced. This is what separates Game Brightness (world only) from UI Brightness (UI only).
  - `0x8F0EAF1C` — main sprite/smoke shader. Outputs are clamped (alpha, distortion, glow, light scatter) so smoke stays clear.
  - `0x0A271311` — minimap/map. `saturate()` so the map does not hit peak brightness.
  - `0x40BF5761` — glow add, multiplied by the **Glow Strength** slider (0-100, default 100). At 100, metal shine reaches about 730 nits; at 50, about 350.
- Settings confirmed working: tone mapper type, Game Brightness, UI Brightness, Blowout, Saturation, Glow Strength.

## Lessons from building it
1. **Frame counter never reset.** The generic template only registers `OnPresent` when Display Proxy is on, so the per-frame counter stayed at frame 0 and only one draw per session was replaced. The reset now lives in a callback registered unconditionally.
2. **Sliders disconnected.** `common.hlsl` must `#include "./shared.h"`, which declares the `shader_injection` cbuffer and the `RENODX_*` macros. Including only `renodx.hlsl` makes every slider silently fall back to defaults.
3. **Input signature must match the original.** The replacement was missing `SV_POSITION`, so every input shifted by one register. A forced-colour test "passed" only because alpha came from `position.w`; do not trust constant-colour tests when inputs may be misaligned.
4. **Do not force alpha = 1.** The world composite is alpha-blended over a background layer drawn by another shader.
5. **Settings do nothing without replacement shaders.** Tone mapper, grading and brightness settings only run inside replacement shader code. With zero custom shaders attached, only the swap chain-level settings (peak brightness, encoding) work.

## Shader roles found (stable across snapshots; draw numbers are not)
`0x8F0EAF1C` smoke / main sprite · `0x82BDA5F5` lighting composite · `0x5D4017CA` per-light accumulator · `0x01A7A161` glow/light-scatter G-buffer (4 render targets) · `0x6AE9A56B` colour-matrix tint · `0xD38C1AE4` radial light pool · `0x11A4EA78` heat distortion · `0x0A271311` minimap · `0x48C1C006` world blit and HUD icons · `0x40BF5761` glow add · `0x991A7AE4` 5-tap glow blur · `0xF3928D92` lights (also draws the character head) · `0x9FBDC40F` character body.

## Open items
- A light-blue loading screen and a split-second flash between menu and level. It persists with the resource upgrade off and disappears with the add-on removed; the cause is probably the swap chain/proxy layer, not a shader. Low priority.

## Resolved (2026-10-09)
- Background: a Devkit snapshot shows every layer, background included, is drawn into the 642x362 scene first; the gated `0x48C1C006` draw that tone maps it is the first, opaque (blend off) draw on the back buffer. In-game check: the background follows Game Brightness, the HUD follows UI Brightness.
- Peak Brightness: tested at 1360 nits (OLED). HDR Analysis in a level: max 430-600 nits, MaxCLL 691; nothing reaches Peak, so nothing is clipped.
- Release build: 32-bit Release builds need the x86 HeapAlloc alignment fix in `src/utils/platform.hpp` (commit on this branch); without it optimized x86 builds can crash at launch.
