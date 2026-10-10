# Fable Anniversary — RenoDX mod

- Game: Fable Anniversary (Steam app 288470), Unreal Engine 3 (Wellington), **DX9, 32-bit** (`Fable Anniversary.exe`).
- Target: `fableanniversary` → `renodx-fableanniversary.addon32` (CMake preset `clang-x86`, build preset `clang-x86-release`, build dir `build32`).
- Status: experimental. Tested at 3840x2160 and 2560x1440 (16:9) on an OLED HDR10 display.

## Architecture

DX9 game → RenoDX **Display Proxy** (the game still renders with DX9; RenoDX presents through a D3D11 HDR swapchain, HDR10 by
default). Plumbing (`shader_injection` at `c50` for ps_3_0, SM5 cbuffer branch for the proxy shaders, proxy VS/PS, settings) is
derived from `hatintime`, which uses the same UE3 post-process combine shader.

## Rendering pipeline (proven with the Devkit and the DX9 disassembly)

| Stage | Signal | Notes |
| --- | --- | --- |
| Scene colour (`s0`) | linear float16, real values > 1 | HDR source |
| Half-res DOF buffer (`s3`) | linear, stored `/4`, alpha = sharp-scene weight | blended into the scene |
| Bloom (`s1`) | linear, stored `/4` | added with a luma-dependent screen blend |
| **`0x63ACB381` (combine)** | `untonemapped` → `pow(x, 1/2.2)` → **saturate (hard clip)** → 16x16x16 grading LUT (256x16 strip, `s2`) | no tone curve; the clip + LUT is the whole "tonemap" |
| Output of `0x63ACB381` | 8-bit, gamma-encoded `graded_sdr`, alpha = luma | read by the FXAA pass |
| FXAA `0xA65C92AC` | reads the target above, writes the swapchain | |
| UI | drawn on the swapchain | |

The **main menu does not use the combine shader or FXAA at all**: its ~156 draws go straight to the 3840x2160 swapchain buffer.

## Approach (follows `handle-sdr-tonemap-lut`: hard clip + LUT)

`0x63ACB381` is replaced (`0x63ACB381.ps_3_0.hlsl`, same interface as the original: TEXCOORD0/1, c0, c8, s0–s3):

1. Rebuild `untonemapped` exactly as the original does (DOF blend, bloom).
2. `neutral_sdr = renodrt::NeutralSDR(untonemapped)` (Vanilla mode: `saturate(untonemapped)`, bit-identical clip).
3. Run the **vanilla LUT** on the `pow(1/2.2)`-encoded `neutral_sdr` (same slice/bilinear addressing and constants as the original).
4. Decode the LUT output as **sRGB** → `graded_sdr`; `ToneMapPass` gets all three signals in that sRGB-decoded domain
   (`correct::GammaSafe(untonemapped, true)`, `graded_sdr`, `srgb::DecodeSafe(lut_input)`) → `RenderIntermediatePass` → luma in alpha for FXAA.

The LUT is sampled once, outside the branches. Vanilla mode (Tone Mapper = Vanilla) reproduces the original math.

**Gamma:** vanilla encodes with `pow(1/2.2)`. As in `borderlands2`, the LUT output is decoded as sRGB and **Gamma Correction 2.2
(default)** applies the 2.2 display curve once in `RenderIntermediatePass`, so Vanilla + 2.2 equals the unmodded game (Off = sRGB
display curve, lifted shadows). Moving `untonemapped` into the same domain with `correct::GammaSafe(x, true)` is this mod's own step
(borderlands2 passes its scene colour raw), so all three ToneMapPass inputs share one domain. Decoding with 2.2 in the
shader *and* selecting 2.2 corrected twice: shadows ~10x darker and near-black chroma blown up into blue speckles (seen on a dark
lattice wall panel indoors, mistaken at first for a resource-upgrade bug).

**RenoDRT:** Reinhard, white clip **13** (`shared.h`): candle flames measured ~8–13x diffuse white (luminance, ToneMapPass domain)
with Tone Mapper = None at Peak 10000; lamps and fire were at least ~7x. The default clip of 100 compressed them (lamp 1320 → 800 nits
at Peak 1360). Stars are brighter and simply reach peak.

## Resource upgrades

`b8g8r8a8_unorm → r16g16b16a16_float` for exactly two kinds of target: back-buffer sized, and the **post-process output**, matched by
aspect ratio 40:23 (its height is the back-buffer height × 46/45: 3840x2208 at 4K, 2560x1472 at 1440p; found with the Devkit).
Do **not** widen this to "everything the width of the back buffer": the engine also creates 3840x2205 8-bit targets and upgrading those washes
the main menu out (hazy page photo, faded buttons/text). Narrowing the upgrade fixed it (compared against the unmodded screenshot).
The exact mechanism of the wash-out is not proven; the fix is empirical.

## Settings

Tone Mapper: Vanilla / None / **RenoDRT (default)**. Gamma Correction **2.2 (default)**. Peak 1000, Game 203, UI 203 nits (UI brightness also scales the main menu, since
it is drawn as UI). Encoding default **HDR10** (scRGB selectable). Force Borderless on. Standard RenoDX grading sliders.
Preset Off = vanilla (also resets Gamma Correction to 2.2).

## Testing performed

- 4K: neutral by default — A/B RenoDRT vs Vanilla, same spot: median luma 9.1 / 9.1 nits, bin ratios 0.96–1.06 between 2 and 120 nits,
  same chroma (rgb/Y); vanilla clips at ~116–140 nits, HDR reaches ~590–640 nits (lamp), stars > 1000 nits, fire ~800 nits (user HDR Analysis).
- 1440p: before the aspect-ratio rule HDR was lost (max 139.7 nits = vanilla clip); after: 691 nits. Menu fine in both modes.
- Main menu, in-game menus, dialogue, cutscenes, loading screens, dark scenes, fire/particles: no artifacts reported.
- HDR10 output verified in the log (`r10g10b10a2_unorm`, `hdr10_st2084`).
- Gamma fix (2026-10-10), indoor wall at night, Gamma Correction 2.2: Vanilla p10/median 0.22/1.30 nits vs unmodded 0.20/1.28
  (before the fix 0.02/0.71); RenoDRT 0.23/1.35; lattice panel clean, no negative colour values. Candles with RenoDRT: ~1000 nits.

## PsychoV evaluation

Not added, by project decision (skill: "only add PsychoV17 when requested"). The game's highlight range is modest (lamp ~600 nits)
and RenoDRT with the SDR/LUT bridge already tracks vanilla within a few percent; PsychoV could be added later as an extra Tone Mapper option.

## Known limitations / open items

- Ultrawide / non-16:9: the padding formula of the post-process target is unverified there (the 40:23 rule may not match → HDR lost).
- Resolution switched *in-session* keeps the old render targets (still matched by the aspect rule); a fresh start is the normal path.
- The FXAA pass computes luma from the HDR-encoded intermediate (values may exceed 1); no artifacts seen, edge quality not measured.
- Other combine-shader permutations (other areas/cutscene paths) were not seen; Devkit snapshots only covered the village and menu.
- Running the Devkit addon together with this mod froze the picture (audio kept playing): test with the Devkit alone, not both.
- Requires the x86 `platform.hpp` alignment fix (separate commit, outside this folder) or optimized 32-bit builds can crash at launch.

## Manual verification

1. Build `fableanniversary` (x86 release), copy `renodx-fableanniversary.addon32` next to `Fable Anniversary.exe` (ReShade 6.x with add-on support).
2. Launch: main menu must look like the unmodded game (slightly dimmer only if UI Brightness < 203).
3. In game: HDR Analysis max > 300 nits in daylight highlights; Tone Mapper = Vanilla must look like the unmodded game.
