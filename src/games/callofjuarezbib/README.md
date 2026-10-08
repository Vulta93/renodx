# Call of Juarez: Bound in Blood — RenoDX HDR mod

**Status:** working HDR build (work in progress). Gameplay and the character-select screen are covered; see [To do](#to-do).

## Setup
- Steam app 21980, Chrome Engine 4, **DX9 only** (no DX10 path), **32-bit**. Use the x86 preset: build the `callofjuarezbib` target to get `renodx-callofjuarezbib.addon32`.
- Exe `CoJBiBGame_x86.exe`. The game has a hidden **`-windowed`** launch option (set it in Steam launch options). **Always test windowed:** exclusive fullscreen with a broken add-on can hang the whole PC.
- Upstream's `src/games/callofjuarez` is for the first game (DX10) and is a good reference for the same tone-map family. DX9 references in the repository: `clivebarkersjericho`, `batmanaa`, `alicemadnessreturns`, `borderlands2`, `dishonored`.
- No other RenoDX work for this game was found (web and Nexus search).

## Mod layout
- `addon.cpp` — generic template plus `__ALL_CUSTOM_SHADERS`, `constant_buffer_offset = 50 * 4`, Display Proxy and Force Borderless on by default. Tone Mapper options are Vanilla, None and RenoDRT (ACES removed: it gives a white screen on `ps_3_0`). The render target upgrade `b8g8r8a8_unorm` → `rgba16f` is hard-coded.
- `shared.h` — DX9 branch `float4 shader_injection[8] : register(c50)` with index-based `RENODX_*` defines; SM5 branch for the proxy shaders.
- `common.hlsli` — helpers: `CoJReinhard`, `CoJUntonemapped`, `CoJBlurHDR` (re-adds the HDR part of the sharp image over the blur), `CoJBrightPass`, and the macro `COJ_FINAL_HDR` (vanilla grade on `sqrt(NeutralSDR(hdr))`, then `ToneMapPass(hdr, graded, neutral)`).
- Eight shader replacements:
  - Tone-map composites (HDR: untonemapped, gamma-2 encoded, no clamps, glow saturated, blur HDR re-add): `0xF3522EE8` (gameplay), `0xC980C651` (no desaturation), `0x0556B86C` (plus two alpha-masked grades and an overlay/vignette multiply).
  - Pre-passes: `0xDA9C7047` (HDR for the depth-of-field blur), `0x17887120` (bright-pass, kept vanilla, plus a 16-tap mask average).
  - Final: `0x1F6ABA21` (curve, brightness/contrast/saturation and gamma in gameplay). On the character-select screen the final grade is split into `0xF43A6FF8` (curve and BCS, still gamma-2) followed by `0x4550ED56` (gamma, `COJ_FINAL_HDR`, `RenderIntermediatePass`).

## How the original tone mapping works
- `x = colour · CONST_70.y · avg.x; y = x · (1 + x · CONST_100.w) / (1 + x)` (extended Reinhard), then **sqrt** (gamma 2.0), with `_sat` everywhere.
- Gameplay order: `0xDA9C7047` pre-pass → blur chain (8-bit, smaller) → `0xF3522EE8` → `0x1F6ABA21` → UI.
- Character-select order: `0x17887120` → … → `0xF3522EE8` → `0x86AA913B` (mask distortion copy) → `0x49A8A546` (mask) → `0x9D68212C` (splat overlay) → `0xF43A6FF8` → `0x4550ED56` → UI.

## Results
- Gameplay: fire reaches 1000–1200 nits (peak 1360), the sky is HDR, no pink tint. In a dark scene Vanilla and RenoDRT average the same. UI is correct and the Game/UI brightness sliders work.
- The depth-of-field blur re-add makes bright backgrounds sharper than vanilla. Considered a plus; it could become a slider later.
- Character-select screen is now HDR (max about 740 nits) and matches Vanilla except for sky brightness.
- Known quirk: the output-ratio upgrade tints the whole image pink, even in Vanilla. Some smaller 8-bit target depends on clamping. The cause was not identified and the upgrade is not needed.

## Findings about the add-on and Display Proxy
1. The generic add-on (x86) with **Display Proxy on** works: the game renders in DX9 and RenoDX presents through a D3D11 HDR10 proxy swap chain. There are two ReShade runtimes: DX9 (`ReShade.ini`, where Lilium shaders fail to compile on SM3) and DX11 for the proxy (`ReShade2.ini`).
2. **The Devkit and the RenoDX add-on together cause a black screen/hang.** Use the Devkit alone with `-windowed` for shader hunting.
3. ReShade rewrites `ReShade.ini` while the game runs: edit it only with the game closed.
4. There is no DX9 disassembler in the build tooling; a small Python SM3 disassembler that reads the CTAB constant names was used. Remember `lrp d, s0, s1, s2` = `s0·s1 + (1−s0)·s2`.

## To do
- More coverage: cutscenes, night and indoor levels, menus, slow-motion/"concentration" mode, graphics settings (depth of field and glow off), and anything capped near 203 nits.
- Release tidy: remove leftovers in `addon.cpp` (unused includes, preset-off comments), finish the metadata, and test a build with other users.
