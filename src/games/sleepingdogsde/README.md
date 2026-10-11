# Sleeping Dogs: Definitive Edition

- Steam 307690, `sdhdship.exe`, DirectX 11, 64-bit: build target `sleepingdogsde` → `renodx-sleepingdogsde.addon64`.
- Output: HDR10 through the RenoDX swap chain proxy (`SwapChainPass`), separate Game and UI brightness.

## Install and play

- ReShade with full add-on support as `dxgi.dll`, the addon next to `sdhdship.exe`, Windows HDR on.
- Play borderless or windowed. In exclusive fullscreen the HDR output is lost, so Prevent Fullscreen is on by default.
- SPatch: its own ACES (`aces_enable=1`, shipped default, toggled with P) hooks the same tone map pass. Disable it
  (`aces_enable=0`, `hook_aces_final=0`, `hook_aces_presentbuffer=0`, `hook_aces_display_curve=0`).

## Pipeline (Devkit snapshots, shader disassembly)

| Step | Shader | Notes |
| --- | --- | --- |
| Lighting | many | Light buffers are already RGBA16F. |
| Scene buffer | composite `0x1964CD11`, sky, particles, glows | RGBA8 3840x2160, stores `y = 1.04x / (x + 0.2)`; additive sprites blend in that encoded space. |
| Round trip | copy `0xFFEE7D7A` | The scene is copied to a second full-res RGBA8 target and back. |
| Bloom | bright pass `0xB942D81A`, 8-bit chain | Decodes a 5-tap average of the scene. |
| Tone map | `0x67843125` | Decode, bloom alpha factor `lerp(1, x, bloom.a)`, Hejl-Dawson curve on `x - 0.004`, white scale `Value0.y`, bloom screen blend, brightness `pow(Value0.w)`. Writes the RGBA8 target the UI draws on. |
| Output | `0x7DF69EF0` | Adds `Value0.x` (constant black-level trim, measured -0.025) and writes the swap chain. |

## What the mod does

- **Resource upgrades:** every full-res RGBA8 target (G-buffer, scene, copy, tone map/UI target) has the same format and
  size, so they are only marked for clone hot swap. The clones are activated by the draws that write the right ones:
  the tone mapper (its output/UI target) and the copy shader `0xFFEE7D7A` (scene round trip). Half-resolution targets
  stay 8-bit.
- **Decode (`common.hlsl`):** the float scene keeps additive stacks above `y = 1`, where the vanilla decode returns
  black or inf (black firework cores, coloured rings). The decode saturates `y` first, so the HDR range is the vanilla
  one (up to `x = 5`), at float precision. Used by the tone mapper and the bloom bright pass.
- **Hair copy `0xC7C6CCDF`:** copies the hair specular scratch into the light buffers; its input is saturated to emulate
  vanilla's 8-bit scratch, which clamped the sum of the overlapping additive hair layers at 1 (whether the float sum
  ever exceeds 1 was not measured; hair looks unchanged).
- **Hair `0x4D449B45`:** its Kajiya-Kay `sqrt(1 - d^2)` and `pow` produce NaN, which the 8-bit target wrote as 0. The
  output is saturated to emulate that clamp.
- **Tone mapping (`0x67843125`):** ShortFuse's original structure. The tone mapper replaces only the filmic curve and
  the vanilla post-steps still run on top. `ToneMapPass(untonemapped, vanilla curve, neutral)` uses the vanilla curve
  as the graded reference (toe, contrast and hue stay vanilla). The graded reference is already a full filmic curve,
  so the neutral reference is the scene itself up to half of SDR white, then `ExponentialRollOff` towards white
  (luminance): in the SDR range the bridge returns the vanilla image. The roll-off start 0.5 is a project decision
  (the vanilla curve already compresses there by ~13%, which is kept as part of the look). The bloom screen blend
  afterwards holds back the range above white where the bloom is strong, which keeps fireworks and lamp halos soft.
  Peak Brightness is pre-compensated for the white scale and brightness steps (RenoDRT gamma correction off around the
  explicit 2.2 encode). RenoDRT white clip 6.26 = decode cap 5 × mid grey scale 0.2254 / 0.18, the maximum where bloom
  alpha is 0.
- **World-space icons `0xAD8E2B7C`** (NPC dialogue indicator): the shader writes the texture colour into the encoded
  scene without the encode, so white landed at lamp-core level and the pulse reached peak. In the HDR modes the colour
  is scaled down (max channel) to `y = 0.82`, the scene value the HDR path outputs as SDR white, calibrated for
  RenoDRT at default sliders and white scale ~0.87 (other Peak/Exposure/None or a changing white scale move it).
  Measured: ~203 nits like Vanilla, ~310-320 nits at the end of the pulse (see open items). Being scene content in the
  game too, it follows Game Brightness, not UI Brightness (the scene alpha is already used, no marker).
- **Output pass `0x7DF69EF0`:** the intermediate holds the scene scaled by Game / UI nits, so the game's black trim
  `Value0.x` is added in vanilla's space (inverse intermediate pass, add, intermediate pass). Added directly, a higher
  UI Brightness darkened the scene. UI pixels in the same target get the scene's treatment (they cannot be separated;
  the difference is ~0.007 in gamma at UI 400 / Game 203).
- **Tone Mapper:** Vanilla (original math and 8-bit clamp), None, RenoDRT (default). ACES through the same path crushed
  the shadows and was removed.

## Rejected approaches

- Expanding the range above `y = 1` (any slope) or handing the vanilla shoulder to RenoDRT: the additive sprites are
  stacked in encoded space and only look right merged into white; expanded they turn into thin, white, blocky outlines.
- The vanilla curve as `neutral_sdr`: its toe clips below 0.004, so the reference lifted the shadows and divided
  near-black colours by ~0 (warped clothing).
- RenoDRT's `NeutralSDR` as the neutral reference (`ToneMapPass(untonemapped, graded)`): it brightens the low end
  relative to the scene, so `UpgradeToneMap` scaled the vanilla image down by scene / neutral (None and RenoDRT ~25-35%
  darker than Vanilla in the low mid-tones, measured).
- Rolling the input off at vanilla white: clean but capped highlights at ~300 nits.
- The tone mapper alone (ShortFuse's ACES/RenoDRT on the raw scene): RenoDRT had no filmic toe and looked washed out.
- PsychoV-17/30 (earlier branch): the limited scene range kept the response far below Peak Brightness.

## Known limitations / open

- Measured (percentiles, Vanilla/None/RenoDRT at one spot): None matches Vanilla within ±4% up to the 80th percentile;
  RenoDRT lifts the darkest pixels slightly (0.12 -> 0.17 nits at p40, its own toe) and both HDR modes read ~9% lower
  at p90-p95 (not yet confirmed per pixel on a static frame).
- Highlights are limited to what the game stores up to `x = 5`; fireworks and lamps reach ~750–1100 nits at Peak 1360.
- Not yet verified with the Devkit: which other targets the copy shader activates in the map, phone and loading
  screens (cutscenes and pause menu checked: only the scene, its copy and half-res targets).
- RenoDRT white clip 6.26, checked with a debug overlay: y = 1 pixels with bloom.a > 0 (lamp cores, sparks; 0.05-0.1%
  of the screen) and the dialogue indicator pulse exceed it and map flat to peak.
- The dialogue indicator pulse is a second draw over the icon, not capped yet (~310-320 nits at its end).
- Not tested: exclusive fullscreen with Prevent Fullscreen on, Bink videos, menus over bright scenes, daytime sun.

## Manual verification

1. ReShade.log: `Registered runtime replacement` for `0x67843125`, `0xb942d81a`, `0x4d449b45`, `0x7df69ef0`,
   `0xad8e2b7c`, `0xc7c6ccdf`.
2. Night street with lamps and fireworks: no black cores, rings or blocky white sprites; soft golden glow.
3. Tone Mapper Vanilla / None / RenoDRT at the same spot: shadows and mid-tones look the same (see measured deviations
   above); only highlights extend.
4. Characters' clothes and hair: no warped colours, no black or flickering pixels.
5. UI Brightness 100 / 203 / 400: only the HUD changes, the scene stays the same.
6. NPC dialogue indicator (RenoDRT, default sliders): about the same brightness as Vanilla (~203 nits), no flash to
   peak.
