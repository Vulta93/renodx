# Sleeping Dogs: Definitive Edition

- Game: Sleeping Dogs: Definitive Edition (Steam 307690)
- API: DirectX 11, 64-bit (`renodx-sleepingdogsde.addon64`)
- Status: experimental

## Pipeline

| Item | Shader | Role |
| --- | --- | --- |
| Tonemapper | `0x67843125` (ps_4_0) | Decodes the scene buffer, adds bloom, applies the Hejl-Dawson curve, white scale, bloom screen blend and brightness |
| Output pass | `0x7DF69EF0` (ps_4_0) | Adds the fade/overlay offset `cb0[0].x`, clamps negatives, writes the swapchain |

Facts established from the Devkit and the shader disassembly:

- The scene buffer is r8g8b8a8 (3840x2160 at 4K) and stores `y = 1.04x / (x + 0.2)`, so the shader decodes `x = 0.2y / (1.04 - y)`. With `y` capped at 1 the largest recoverable value is `x = 5`.
- The tonemapper evaluates Hejl-Dawson on `x - 0.004`, divides by the white scale `cb0[0].y`, screen-blends the 8-bit bloom (960x540) in gamma space (`1 - (1 - bloom)(1 - color)`), then raises to `2.2 * cb0[0].w` (in-game Brightness).
- The swapchain is r16g16b16a16_float (scRGB). No `RENODX_SWAP_CHAIN_OUTPUT_PRESET` is defined, so RenoDX's default applies.
- Four r8g8b8a8 render targets (indices 0..3) are upgraded to fp16.

## Tone mappers

| Mode | Behaviour |
| --- | --- |
| Vanilla | Original look, rendered through the standard RenoDX intermediate and output passes |
| None | No tone curve (RenoDX `config` type 1) on the decoded scene, then the vanilla post-steps |
| ACES | RenoDX ACES with the vanilla white scale / bloom / brightness post-steps and peak pre-compensation |
| Hejl-Dawson Extended (default) | See below |

### Hejl-Dawson Extended

This is a **project decision**, not a pattern taken from an existing RenoDX game mod. It is **not** a final-frame inverse tonemap: it works from the decoded scene buffer that feeds the game's own tonemapper, before any output clipping.

- Hue and saturation come from the vanilla SDR image (clipped per channel), so hot lights go to white as in the original instead of turning orange.
- Luminance follows the vanilla image and rises towards an extended Hejl-Dawson curve (`hejldawson_extended.hlsli`, pivot 0.18, linear extension above it) where the scene exceeds white or the vanilla image is already at white.
- Neutwo `MaxChannel` maps the largest value the game can deliver (`x = 5` through the same chain) to Peak Brightness.
- There is deliberately no bloom contribution above white. Vanilla and ACES screen-blend the bloom over highlights, which forces them to exactly SDR white and flattens lamps into a solid disc.
- The extension is computed from a Gaussian blur of the decoded scene (sigma 8 px, 10x10 bilinear taps, 3 px apart). Lamps are hard-edged polygons in the scene buffer; unblurred, the range above white exposes those edges as boxes and stairs.
- A weight (`extension_weight`) fades the extension in from `vanilla_y = 0.85`, so the area around a lamp ramps up instead of ending in a flat 203-nit shelf.

### Alternatives evaluated

- PsychoV 17 was tried and removed. The game's brightest scene value is limited (`x = 5`), so the response stopped short of Peak Brightness. A highlight-boost slider was tried to compensate; every value other than 100 was judged unusable.
- RenoDRT was removed from the final mode list.

## LUT / colour grading

The tonemapper has no LUT. The Exposure / Highlights / Shadows / Contrast / Saturation sliders use `renodx::color::grade::UserColorGrading` in the Hejl-Dawson Extended mode and `renodx::tonemap::config` for ACES/None.

## Known limitations

- Output is scRGB only; there is no HDR10 option or SDR/HDR toggle.
- The extension cannot exceed what the 8-bit scene buffer stores, so very bright sources share the same top value.
- Residual: some lamp glow still looks slightly offset from the lamp compared with ACES; accepted for now.
- `cb0[0].x` in the output pass is passed through as a gamma-space offset. Its exact meaning is unconfirmed.
- Gamma correction is off (`RENODX_GAMMA_CORRECTION 0`, pure gamma 2.2 as the game's own post-steps assume). Whether it should be on is an open question.

## Not tested yet

- That the scene sampler `s0` is bilinear (the blur assumes it).
- GPU cost of the 100-tap blur at 4K.
- Side effects of the four fp16 upgrades on smoke, particles, additive effects, fades, menus, maps, UI and video.

## Build

Configure with the `clang-x64` preset and build the `sleepingdogsde` target. The artifact is `renodx-sleepingdogsde.addon64`; copy it to the game folder (next to `SleepingDogsDefinitiveEdition.exe`, with ReShade full add-on support installed).

## Manual verification

1. Launch the game, open the ReShade overlay (Home) and confirm the RenoDX tab shows `Tone Mapper: Hejl-Dawson Extended`.
2. Switch to Vanilla with Peak and Game Brightness at 203: the image should match the unmodded game.
3. Switch back to Hejl-Dawson Extended: midtones and shadows should be unchanged from Vanilla, only lamps, neon, sun and specular highlights should rise above 203 nits towards Peak.
4. Look at a street lamp at night (hard edges, no stairs, no flat shelf beneath the bulb) and at a bright daylight scene.
5. Check menus, map, fades to black, cutscenes/video and particles/smoke for artefacts (see "Not tested yet").
