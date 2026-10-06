# Prince of Persia: Warrior Within (RenoDX)

GOG release, `POP2WW.EXE`, Direct3D 9, 32-bit → build `renodx-princeofpersiaww.addon32` (x86 preset, `build32`).
Tested with the common community fixes installed (Ultimate ASI Loader, widescreen/4K fix, dxwrapper with
`D3d9to9Ex = 1`, Xidi, dsoal) and ReShade 6.8 (32-bit, full add-on support) as `d3d9.dll`.

## How the game renders (Devkit snapshots, gameplay)

| Step | What | Shaders |
|---|---|---|
| 1 | 3D scene, post effects and glow A into the render-target texture T (3840x2160 A8R8G8B8) | fixed function, ps_1_1 (glow A: 0x6820B8A4, 0x5EA85978, 0xCC6D19AE, additive combine 0x8B23F50B) |
| 2 | Scene blit T → backbuffer, `r0 = t0 * c0 + (t0 - c0)` | vs 0x344B89F7, ps_1_1 0x2059E26C |
| 3 | Copy backbuffer → T | StretchRect |
| 4 | Glow B / blur / motion trail into T | ps_1_1 0x5EA85978, 0xCC6D19AE, composite 0x3D277874 |
| 5 | HUD into T, copy T → backbuffer | fixed function, StretchRect |

There is no tone mapper and no LUT: vanilla clips at the 8-bit targets. Values above white only come from the game's
own blending (fires, lanterns, lit decals). ps_1_x shaders clamp their output to [0, 1], even into float16 targets.

## What the mod does

1. **Display Proxy** (D3D9 → D3D11 HDR10 swapchain) and all `b8g8r8a8_unorm` render targets upgraded to float16.
2. **Backbuffer copies redirected in both directions** to the float16 clones (RenoDX only redirects texture copies; the
   D3D9 backbuffer is a surface). Without it the HUD and the final image went to the unseen original backbuffer.
3. **Graded SDR bridge around the game's own glow B / blur** (`handle-sdr-tonemap-lut` graded SDR pattern). Those
   effects need values ≤ 1 (their ps_1_1 shaders clamp and their blending relies on it):
   - pass 1 (`scene_sdr_ps`, drawn instead of the blit): the HDR scene is saved to a side texture and the effects get
     an SDR scene, identical to vanilla up to white; above white the colour is scaled by its max channel (hue kept),
     or clipped per channel in Vanilla mode;
   - pass 2 (`scene_finish_ps`, at the first draw after glow B, or at the final copy): HDR is rebuilt on top of the
     game's result with `renodx::tonemap::UpgradeToneMap(untonemapped, neutral_sdr, graded_sdr)`, then tone mapped and
     written with `RenderIntermediatePass`, so the HUD drawn afterwards follows UI Brightness.
4. **Tone Mapper**: Vanilla (original clip), None, RenoDRT, **Neutwo** (default). Neutwo uses a white clip at the
   game's measured steady maximum (`SCENE_MAX_WHITE = 7.5` × SDR white, fire cores) and is the identity when Peak is
   at or above it. Flaming-arrow pile-ups (additive sprites stacking in float16, up to ~8800 nits) are deliberately
   outside that range and are clamped at Peak.
5. PsychoV: not used — the game has no tone curve to match (hard clip at 8-bit targets).

## Settings notes

- The game's Gamma / Brightness / Contrast options have no effect with the mod (also shown in the add-on menu).
- Defaults are neutral: mid-tones match vanilla; Color Grading sliders are user preference.

## Verification performed

- Build: `renodx-princeofpersiaww` (x86 Release); new/removed files need CMake Configure.
- Runtime: HUD present; Game and UI Brightness independent; menus and cutscenes normal.
- avg luminance ≈ 14 nits for Vanilla / None / RenoDRT / Neutwo at the same spot (mid-tones unchanged).
- Deterministic ramp test (temporary debug mode, 0 → 20× white, Peak 1360): None avg 1109 nits (predicted 1116),
  Neutwo 1014 (predicted 1018) with the game's glow on.
- Water reflections, ripples/heat haze, water near bright lights and splashes: identical in SDR and HDR.

## Known limitations / open

- RenoDRT compresses the fire well below Peak (not the default).
- Not yet tested: dagger time powers (rewind / slow motion), smoke-heavy areas, pre-rendered videos, loading screens.
