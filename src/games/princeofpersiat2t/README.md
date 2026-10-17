# Prince of Persia: The Two Thrones (RenoDX)

GOG release, `POP3TT.EXE`, Direct3D 9, 32-bit, build `renodx-princeofpersiat2t.addon32` (x86 preset, `build32`).
Tested with ReShade 6.8 (32-bit, full add-on support) as `d3d9.dll`, and the common community fixes installed (Ultimate
ASI Loader, 4K/widescreen fix, dxwrapper with `D3d9to9Ex = 1`, Xidi, dsoal). The game ships as an exclusive-fullscreen
3840x2160 A8R8G8B8 device; the log shows `CreateDeviceEx` (D3D9Ex via dxwrapper), `BehaviorFlags = 0x46` (not a pure device).

## What this mod is, and what it is not

It gives the game a correct HDR10 output path: Game Brightness / UI Brightness, a Peak Brightness limit, user color grading,
and highlight handling that keeps whatever range above white the game really produces. **The game does not produce much.**
In the scenes measured (a large fire, and 15 s of play through scenes the author chose as bright: sand powers, torches, lava,
sun, Dark Prince; which scenes were visited was not recorded) the
scene never exceeded about 1.0 of white; only a few frames reached about 1.28 (about 260 nits at Game Brightness 203). With
Tone Mapper None the image therefore looks like the original at Game Brightness, not like an HDR remaster. Brighter
highlights would have to be invented (see "Possible later work"), and this mod does not do that.

## How the game renders (Devkit snapshots, gameplay)

| Step | What | Shaders |
|---|---|---|
| 1 | 3D scene (sky, world, characters, particles) straight into the backbuffer | fixed function, vs only (e.g. 0x8EBECB9C, 0x670FCC7E, particles 0x4A1DBD26, 0x0A899C67) |
| 2 | Copy backbuffer → render-target texture T (3840x2160) | StretchRect |
| 3 | Offset blit T → backbuffer: `rgb = t0 + (2 * c0 - 1)`, `a = t0.a` (c0 = 0.5 is identity) | vs 0x344B89F7, ps_1_1 0x10B45210 |
| 4 | Glow A: copy backbuffer → T, mask, blur chain down to 8 px, combine into the backbuffer | ps_1_1 0x6820B8A4, 0x5EA85978, 0xCC6D19AE, 0x8B23F50B |
| 5 | Glow B: copy backbuffer → T, blur, composite into T, copy T → backbuffer | 0x5EA85978, 0xCC6D19AE, 0x3D277874 |
| 6 | Character shadows into the backbuffer (1024x1024 b5g6r5 shadow map) | 0x9E341159 + 0xDCD78A74, then 0xC1A6D837 / 0x0C4DE03F + 0xEAA1F8EA |
| 7 | Copy backbuffer → T, HUD and menus drawn into T, copy T → backbuffer (end of frame) | fixed function (HUD vs 0x4A1DBD26, menu vs 0x4211A7E8) |

The main menu differs: a distortion draw (0xCB5EF258, samples T) before step 3 and a colour lookup (0xC6946932, a 256x1
table indexed by green and blue) between glow A and glow B. There is no tone mapper and no scene LUT in gameplay: vanilla
clips at the 8-bit targets. ps_1_x shaders clamp their output to [0, 1].

## What the mod does

1. **Display Proxy** (D3D9 → D3D11 HDR10 swapchain) and all `b8g8r8a8_unorm` render targets upgraded to float16
   (`ignore_size`, so the 800x600 and glow targets follow). The 1024x1024 b5g6r5 shadow map is another format and stays.
2. **Copies with the backbuffer surface redirected** to the float16 clones with a native StretchRect (RenoDX only handles
   texture copies; the D3D9 backbuffer is a surface). The game does six backbuffer ↔ T copies per frame.
3. **Graded SDR bridge** around the game's own glow, colour effects and shadows. Those shaders need values ≤ 1.0:
   - pass 1 replaces the first offset blit of the frame: it saves T (the HDR scene) aside and writes the SDR scene with the
     game's own offset applied (`saturate(sdr + (2 * c0 - 1))`, c0 read from the game's draw). Identical to the original for
     everything ≤ 1.0; above white the colour is scaled by its max channel (hue kept), or clipped per channel in Vanilla mode;
   - the glow B composite draw arms pass 2, which runs at the next copy out of the backbuffer (the one that hands the finished
     scene to T before the HUD): `UpgradeToneMap(untonemapped, neutral_sdr, graded_sdr)` rebuilds HDR on top of the game's
     result, then user grading, then `RenderIntermediatePass` (Game/UI brightness split) and the tone mapper. The HUD, drawn
     into T afterwards, stays at UI Brightness.
   - There is deliberately no fallback at the T → backbuffer copy: it also happens at the end of glow B, before the shadows.
     A frame without the glow B composite is shown as the SDR bridge at UI Brightness.
4. **Tone Mapper**: Vanilla (original clip), None, **Roll-off** (default): `ExponentialRollOff` on the max channel taken in the
   swap chain encoding colour space (BT.2020 for HDR10), colour scaled so hue is kept; identity up to 0.6 × Peak, white clip
   7.5 × Game Brightness. **The 7.5 clip is copied from Warrior Within, where fire reached ~1350 nits; it is not measured in
   this game** (and, since this game's content stays near white, it is irrelevant until something brighter is shown).
5. **PsychoV**: not used. The game has no tone curve to match (hard clip at 8-bit targets), and its content has no range to
   compress. RenoDRT and Neutwo were rejected in the sister mods Warrior Within and Sands of Time on the same glow chain
   (they compressed the fire well below Peak); not re-measured here.
6. Settings: Tone Mapper, Peak / Game / UI Brightness, Color Grading (Exposure, Highlights, Shadows, Contrast, Saturation,
   Highlight Saturation, Blowout, Flare), Gamma Correction, Display Output options.

## Measurements

- Back buffer, fire region (480x270), `devkit_watch_region`, 164 writers measured, CPU cross-check matched: no scene writer
  ever took the region above 1.0. The fire particle draws (vs 0x4A1DBD26) peak at 0.985. Pass 1 raises the maximum to exactly
  1.000 (the game's offset, then saturate). Pass 2 scales by the Game/UI ratio (1.184 for Game 203 / UI 140) and adds no range.
- Texture T, whole frame, 900 frames (Game 203 = UI 203): peak before the glow B composite 0.52..1.02, after 0.37..1.28.
- Full frame in the fire scene: 20 pixels above 1.0 (maximum channel 1.17), none above 2.
- The blend states of the fire draws are not readable in the Devkit (DX9), so **why** the fire stays at or below white is an
  inference: it fits ordinary alpha blending, not additive stacking as in Warrior Within and Sands of Time.

## Verification performed

- Build: `renodx-princeofpersiat2t` (x86 RelWithDebInfo; new or removed shader files need CMake Configure).
- Runtime: the add-on loads next to the Devkit, HDR10 proxy runs, HUD and menus show, the RenoDX menu has all settings,
  Game Brightness changes the scene but not the HUD, glow/blur and character shadows look like the original.

## Known limitations / not tested

- No real HDR range in the scenes measured (see above). Not measured: other levels, sand-power frames individually.
- Not tested: main menu (distortion and colour lookup run before/inside the bridge; the distortion clamps HDR where it
  draws, which is irrelevant while the scene has none), pause menu, videos and loading screens (frames without the offset
  blit stay at UI Brightness), Alt-Tab / Reset, other resolutions, `BlurFX=0` or `ShadowsEnabled=0` in `Hardware.ini`
  (without the glow B composite pass 2 never runs), fades and flashes (the offset c0 is applied as part of the SDR grade),
  shadows on surfaces brighter than white.
- The game's own Gamma / Brightness / Contrast options were not checked.
- The "mouse guard" setting works around two ImGui contexts reading one mouse with the Display Proxy (inherited from the
  sister mods; it ignores the mouse when the settings window is narrower than 600 px).

## Possible later work (not implemented)

An optional, default-off "Fire enhancement" that makes bright, saturated effect pixels brighter than the game's white. That is
invented luminance, not recovered HDR, and would be labelled as such in the add-on. It would need its own design and a
mask so the character's white clothes, sky and UI are not lifted.
