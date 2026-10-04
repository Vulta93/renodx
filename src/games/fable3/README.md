# Fable III (PC, 2011) - RenoDX mod

DX9, 32-bit (`.addon32`), Lionhead "Albion" engine, game folder `Fable3.exe`. ReShade 6.x runs as `d3d9.dll`; the mod uses the Display Proxy
(swapchain back buffer `b8g8r8x8_unorm` upgraded to f16). Build with the x86 target; the branch must contain the x86 `platform.hpp` alignment fix.

## Rendering pipeline (FACT, Devkit snapshots)
HDR scene f16 -> luminance histogram (`0x30B75811`) -> 512x1 r32_float tone-curve texture (`0xE65D10CF`) -> optional DoF chain ->
**one composite draw into the 8-bit swapchain** -> HUD drawn afterwards. The composite is the tone mapper: an adaptive, luma-driven gain
(extended Reinhard blended with histogram equalisation) used as a colour scale, per-channel saturate, bloom screen blend, saturation,
`pow(x, g_GlobalGammaAdjustment)`. There is no grading LUT.

The composite exists as five compiled permutations; each has its own constants/samplers (thin `0x<HASH>.ps_3_0.hlsl` files include `composite.hlsli`):

| Hash | Variant |
| --- | --- |
| `0x79E9C4FC` | no DoF (dialogue camera), **no depth buffer** |
| `0xE76D7068` | DoF |
| `0x1902BEBB` | DoF + dust |
| `0x794A5A08` | DoF + displacement |
| `0x5A6CF82E` | DoF + dust + displacement |

Missing: displacement without DoF (re-capture with the Devkit if it shows up). Devkit and the mod addon cannot be loaded together (black screen);
swap them.

## Signals
- `color`: raw HDR scene before exposure and curve. Measured in game: most surfaces < 1, droplets/stained glass 1-3, sunlit white cloth 2-4
  (thin specular edges 6-8), sky 2-5.
- `untonemapped` = `color * exposure_gain`, the exposure part of the curve (texel 0 of the curve texture times the brightness constant).
- `vanilla_linear` = the whole vanilla result (curve, per-channel clip, bloom, saturation, gamma) decoded to linear. Used as the graded SDR reference.

## Tone mapper options (setting `ToneMapType`, default RenoDRT)
- **Vanilla**: `RenderIntermediatePass(vanilla_linear)`; neutral 1:1 reproduction of the game, paper white applied.
- **RenoDRT** (default): `ToneMapPass(untonemapped, vanilla_linear)`. The ratio vanilla / neutral SDR carries the vanilla look onto the unclipped
  scene; RenoDRT (Reinhard method, the most common choice in the repo) maps to the user's peak. Faithful to the scene: highlights are only as
  bright as the scene makes them (fountain spray ~3x paper white is about 430 nits at 1360 peak). Button "Modder Preference" (Presets) selects RenoDRT with Exposure 1.10, Highlights 55, Scene Grading 45.
- **Vanilla+**: no ToneMapPass. Vanilla up to a knee, then the brightest channel climbs toward the peak:
  `u = max(driver - knee, 0) / range; r = u^2 / (1 + u^2) * far_weight; max' = max + (peak - max) * r; out = vanilla_linear * max'/max`.
  C1 at the knee, monotonic, bounded by peak, hue ratios of vanilla kept. `driver` = max channel of the raw `color`.
  `far_weight = smoothstep(8, 12, view_depth)` protects what is close to the camera (characters). Sliders: Highlight Start (1.5), Highlight Range (4).
  Button "Vanilla Like" selects Vanilla+ with those values and the effects on.

### Why a depth weight
Characters' sunlit cloth (raw 2-4) is as bright as fountain spray and sky, so no brightness threshold, neutral-colour test or scene alpha separates them.
Measured view depth: sky >= 200, trees/rooftops 64-200, characters 2-8, fountain spray 8-32. Depth is the only signal found that separates the two.
Known costs: effects very close to the camera (own spells, a nearby campfire) stay vanilla; distant NPCs are lifted; the 8-12 range was tuned
from the balcony scene only; the dialogue composite has no depth buffer so Vanilla+ lifts nothing there.

### Rejected Vanilla+ drivers (all tried in game)
Exposed scene luminance (never reached the knee), vanilla output brightness (sky always 1.0, characters at peak), high raw threshold (loses the
little range the game has), neutral-colour rejection (also rejects white spray). A depth-gated Highlights slider for RenoDRT was prototyped and
reverted (not needed: the user is happy with plain RenoDRT).

## PsychoV evaluation
Not used. The game's curve is adaptive and its scene signal is only meaningful through the vanilla grade bridge, and RenoDRT with the vanilla
reference already preserves the look; no PsychoV path was tested. (Decision, not measured.)

## DX9 notes
`float4 shader_injection[10] : register(c200)`, `constant_buffer_offset = 200 * 4` (the game uses c0-c136). Bool constants declared plainly (`b0` AA,
`b1` DoF). Mouse guard in `addon.cpp` for the menu. Video options in the user's config disable Bloom/MotionBlur/TAA, so the bloom texture is a dummy.

## Testing performed / open
Tested: balcony and fountain (Vanilla, RenoDRT, Vanilla+), peak 1360. Not tested: HUD, menus, videos, cutscenes, bloom-enabled config, other
permutations (inventory/caves/rain). The game's own auto exposure makes brightness drift with camera movement; not addressed.
