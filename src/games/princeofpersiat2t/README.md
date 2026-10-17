# Prince of Persia: The Two Thrones (RenoDX)

GOG release, `POP3TT.EXE`, Direct3D 9, 32-bit, build `renodx-princeofpersiat2t.addon32` (x86 preset, `build32`).
Tested with ReShade 6.8 (32-bit, full add-on support) as `d3d9.dll`, and the common community fixes installed (Ultimate
ASI Loader, 4K/widescreen fix, dxwrapper with `D3d9to9Ex = 1`, Xidi, dsoal). The game ships as an exclusive-fullscreen
3840x2160 A8R8G8B8 device; the log shows `CreateDeviceEx` (D3D9Ex via dxwrapper), `BehaviorFlags = 0x46` (not a pure device).

## What this mod is, and what it is not

It gives the game a correct HDR10 output path: Game Brightness / UI Brightness, a Peak Brightness limit, user color grading,
and highlight handling that keeps the range above white that the game really produces. **How much range there is depends on
the content.** A CPU readback of the saved scene (one frame in 20, about 2 minutes of play through fire, torches, sand
powers, lava and sun; which scene each sample came from was not recorded) found pixels above the game's white in 68 of 172
sampled frames (1.25x encoded in 30, 2x in 16, 4x in 3; maximum 8.7x), but only in a small part of the picture (at most about
0.7% of the pixels). Most likely they come from additive effects that stack in the float16 targets (about 6% of the draws are additive; the
source of each peak was not attributed). The large fire plume that was measured in detail stays just below white (peak 0.985), so with Tone Mapper None that scene looks like the
original at Game Brightness. Nothing is invented: where the game produced no range, there is none. Brighter fire would have
to be synthesised (see "Possible later work"), and this mod does not do that.
(Values are in the game's own sRGB-like encoding with 1.0 = game white; 2.0 encoded is about 5x linear, about 1000 nits at
Game Brightness 203.)

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
   7.5 × Game Brightness. The 7.5 clip was taken from Warrior Within; in this game the steady scenes measured peak at 6.0x white (BT.2020
   max channel, see Measurements), so 7.5x leaves headroom. The stealth-kill dagger flash goes far beyond it (up to 134x) and is
   clamped at Peak.
5. **PsychoV**: not used. The game has no tone curve to match (hard clip at 8-bit targets), so PsychoV's anchors have nothing to
   match. RenoDRT and Neutwo were rejected in the sister mods Warrior Within and Sands of Time on the same glow chain
   (they compressed the fire well below Peak); not re-measured here.
6. Settings: Tone Mapper, Peak / Game / UI Brightness, Color Grading (Exposure, Highlights, Shadows, Contrast, Saturation,
   Highlight Saturation, Blowout, Flare), Gamma Correction, Display Output options.

## Measurements

- Fire frame, back buffer region (480x270), `devkit_watch_region`, 164 writers measured, CPU cross-check matched: no scene
  writer took the region above 1.0. The fire particle draws (vs 0x4A1DBD26) peak at 0.985. Pass 1 raises the maximum to
  exactly 1.000 (the game's offset, then saturate). Pass 2 scales by the Game/UI ratio and adds no range. Full frame in that
  scene: 20 pixels above 1.0, maximum channel 1.17, none above 2.
- Scene readback (temporary diagnostic build, not in the repository): the saved HDR scene (T just before pass 1) was copied to
  system memory and scanned on the CPU in every 20th frame: 172 samples, 68 with a maximum above 1.0, 30 above 1.25, 17
  above 1.5, 16 above 2, 3 above 4, largest 8.66; most pixels above 1.0 in one frame: 56,791 (0.7%). The first samples
  (menus and loading) never exceeded 0.65.
- Per-frame data (second diagnostic build, 255 samples, one in 20 frames, not labelled by scene): in 244 steady samples 73 had pixels above
  white (about 6,000 pixels, 0.07% of the frame, per frame); their BT.2020 max channel (linear, 1.0 = game white) had median 1.25x,
  90th percentile 1.8x, 99th 5.6x, maximum 6.0x; no steady frame had a pixel above 7.5x. In 11 samples that match three stealth
  kills by time (the dagger flashes bright white; the attribution is inferred from time stamps and the author's remark) the maximum reached 134x (8.4x encoded), with up to 68,000 pixels above white and thousands
  above 64x. Those are clamped at Peak.
- Darkened highlights: on pixels with real range that the game darkened afterwards (luminance below 85% of the SDR bridge),
  the rebuild leaves them on average 1.2x to 1.26x (up to 2.1x) brighter than a multiplicative model would (darkening ratio x
  HDR luminance). This covers about 5,000 pixels per frame at most (0.07%). An experimental setting that scaled the range with
  the darkening was tried in game (sunlit scene with shadows, bright effects) and made no visible difference, so it was not
  kept. Which darkening dominates (shadow receivers ZERO/SRCCOLOR, the negative offset in some scenes with c0 0.4408, or the
  glow B composite SRCALPHA/INVSRCALPHA) was not determined.
- Offset constant: the game's `c0` for the offset blit was read successfully in every frame and varies between 0.4408 and
  0.5250, i.e. the offset is between -0.118 and +0.05 in encoded units (negative darkens).
- Blend states, from a draw-time log of Direct3D 9 render states (about 1.8 million draws into the 3840 px targets): opaque
  69%, SRCALPHA/INVSRCALPHA 23%, additive 6% (SRCALPHA/ONE 62k, ONE/ONE 41k, DESTALPHA/ONE 3k), multiplicative ZERO/SRCCOLOR
  2% (the shadow receivers, 0xEAA1F8EA). Glow A combine 0x8B23F50B is ONE/ONE, glow B composite 0x3D277874 is
  SRCALPHA/INVSRCALPHA. The additive draws are mainly particle vertex shaders 0x4A1DBD26, 0xBC49B90B, 0xC25ABE42, 0x670FCC7E
  and 0x17B5C7B0. Every one of these draws targets float16 (format 113) with the mod.

## Verification performed

- Build: `renodx-princeofpersiat2t` (x86 RelWithDebInfo; new or removed shader files need CMake Configure).
- Runtime (author, in game): the add-on loads next to the Devkit, HDR10 proxy runs, HUD and menus show, the RenoDX menu has all
  settings, Game Brightness changes the scene but not the HUD, glow/blur and character shadows look like the original.
- Main menu: normal picture; Game Brightness changes the 3D backdrop but not the menu text. Devkit snapshot of the frame: copy,
  offset blit (pass 1), glow A, colour lookup, glow B composite, the next backbuffer to T copy (pass 2), menu UI, final copy.
  The distortion draw did not run in that frame.
- Pause menu: normal; Game Brightness changes the scene behind it but not the pause UI.
- Videos and loading screens follow UI Brightness only (no offset blit, so no scene pass), no flicker, no jump.
- Death fade to black and back: smooth, no leftover highlights or flash.
- Alt-Tab: the game freezes while away and resumes unchanged, as in the original. The log shows no device Reset or swapchain
  destruction from it, so the Reset path was not exercised.

## Known limitations / not tested

- Which effects produce the range above white (sand powers, specific particles, lava) was not attributed per scene; the readback
  samples were not labelled. The large fire plume that was measured in detail has none.
- Not tested: the main-menu distortion draw (it clamps range where it draws), the device Reset path (resolution or window mode
  change), other resolutions, `BlurFX=0` or `ShadowsEnabled=0` in `Hardware.ini` (without the glow B composite pass 2 never
  runs), fades that are not done through c0, the stealth-kill flash (clamped at Peak) as seen on screen, sand powers (not
  unlocked by the author).
- The game's own Gamma / Brightness / Contrast options were not checked.
- The "mouse guard" setting works around two ImGui contexts reading one mouse with the Display Proxy (inherited from the
  sister mods; it ignores the mouse when the settings window is narrower than 600 px).

## Possible later work (not implemented)

An optional, default-off "Fire enhancement" for effects that stay at or below the game's white (for example the large fire plume
measured in detail). That is invented luminance, not recovered HDR, and would be labelled as such in the add-on. It would need its own design and a
mask so the character's white clothes, sky and UI are not lifted.
