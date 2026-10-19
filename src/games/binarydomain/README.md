# Binary Domain (RenoDX)

Steam release (app 203750), `BinaryDomain.exe`, Direct3D 9, 32-bit. Build target `binarydomain` (x86 preset, produces
`renodx-binarydomain.addon32`). Tested with ReShade 6.8 (32-bit, add-on support) as `d3d9.dll`, the game windowed at
3840x2160. The log shows plain `IDirect3D9::CreateDevice` with `BehaviorFlags = 0x56`: a **pure device** (no `Get*` calls
from an add-on) and not D3D9Ex; the RenoDX Display Proxy upgrades the device itself, so the Devkit's Force DX9Ex must stay
off while this mod is loaded.

## What the mod does

HDR10 output through the Display Proxy with Game Brightness / UI Brightness / Peak Brightness, user color grading, and
highlights that keep the range the game really renders. Nothing is invented: the original game tone maps a float16 scene
(`sqrt(1 - exp(-x * exposure))`, per channel, no LUT) into 8-bit targets, so the range above white exists in the scene
buffer and is only lost at that curve.

## How the game renders (Devkit snapshots of a bright outdoor scene)

| Step | What | Shaders |
|---|---|---|
| 1 | G-buffer, light accumulation into an RGBM-like 8-bit buffer, decoded into the float16 scene | many; resolve 0x30FA9336 |
| 2 | Sky and forward geometry into the float16 scene (R16G16B16A16F, 3840x2160) | 0x247F825A, 0x8B002827, ... |
| 3 | Luminance downsample chain to an R32F 1x1 (CPU auto exposure, constant c66.x, ~14 outdoors) and an 8-bit bloom (384x240) | 0x80325B32, 0xE2464658, ... |
| 4 | Translucent layers into 8-bit targets (full and half resolution), gamma-2 encoded, unclamped output | 0x3706C51F, 0x545C828F, 0xA1DED5C0 |
| 5 | **Tone map and composite**, drawn twice (full resolution layer, then half resolution layer) with a constant write between | 0xD46EFAB6, 0x76589376 |
| 6 | Copy, FXAA-like pass, copy | 0xBDFBB64D |
| 7 | Haze / fog sprites blended on the tone mapped 8-bit image (about two thirds of the outdoor brightness) | 0x887653C9 |
| 8 | Copy of the finished scene into the HUD buffer, HUD sprites, final pass to the back buffer (`pow(x * c192.x + c192.y, c192.z)`, identity at defaults) | 0x19BC57FB, 0x707C2DA8, 0x1F45D316, 0x785AEA45 |

The 8-bit era image is gamma-2 encoded and the display decodes it with its own curve (about 2.2): on screen the light is
`encoded ^ 2.2`, not `encoded ^ 2`. The mod keeps the game's encoded values as they are and decodes with 2.2.

## Replaced shaders

- **0xD46EFAB6** (tone map): vanilla display light `(1 - e^-x)^1.1` up to a knee at `x = 1.7` (0.801, 0.904 encoded), then
  linear continuation with the slope at the knee (0.1969). Below the knee it is exactly vanilla. Output stays in the game's
  2.2 encoding so the haze sprites and the AA pass blend as they always did (the particle layer alpha is clamped to 0..1 as
  the 8-bit target did). Vanilla mode keeps the original clip of this
  shader's own output (the finish copy clamps the whole scene at white again, see below). The
  exposure is read in the shader (c66.x), the replaced shader sees the game's own constants.
- **0x19BC57FB** (plain texture copy): replaced only for the back-buffer sized copy into the HUD buffer (decided in
  `on_replace` from the render target size; the luminance downsample chain uses the same shader and keeps the original).
  Applies color grading, the Game / UI brightness split (`RenderIntermediatePass`) and the Roll-off to the finished scene,
  haze sprites included. The split has to happen here: the haze sprites are drawn after the tone map and ignore Game
  Brightness, so a split inside the tone map left the UI slider moving most of the picture.
- **0x707C2DA8** (HUD sprite): `saturate` of the output, which the original 8-bit target did implicitly.
- **0x785AEA45** (final pass): the original math plus a cap on what the HUD can add: `min(after, max(scene_before_hud, 1))`
  per channel. The HUD pulse effect stacks about twelve additive glow sprites; the original 8-bit buffer capped the sum at
  white, the float16 buffer did not (the pulse reached the Peak Brightness). The scene as it was just before the first HUD
  sprite is kept in a native D3D9 texture, bound to ps_3_0 sampler 14 (no game shader uses it) at the final pass and
  unbound at present. This is an end-of-chain approximation of "clamp after every blend": identical for additive stacks.
- **0x36401A42** (shadow filter, an addon-only improvement, not a vanilla fix): the game's soft shadow pass blends 7 reads of the
  shadow data with depth-aware weights (out.x = sum(w * x) / sum(w), out.yzw = center tap). Its tap spacing leaves the
  shadow edges blocky (observed inside the bus, also in the base game, and worse with the filter switched off). The
  replacement is the same math with the distance of the 6 outer taps from the center scaled by **Shadow Filter Width**
  (1 = original, default 3, up to 4); widths 2 and 3 were judged much better than the original in game and about equal to
  each other.

## Resource upgrades

Every back-buffer sized `b8g8r8a8_unorm` render target becomes `r16g16b16a16_float` (tone map output, the copies, the AA
pass, the haze sprites and the HUD then keep their range). The half resolution layer and the bloom stay 8-bit. A Devkit
clone test showed a value of 2.0 reaching the HUD buffer unclamped through all of these passes.

## Deviations from repo conventions (decisions)

- No `ToneMapPass`: the game's own curve is analytic and per channel, so the default Roll-off tone mapper extends it
  (`extended-vanilla`) instead. **PsychoV17** is an optional Tone Mapper (not the default): `psychotm_test17` on the exposed
  scene, anchored to the vanilla curve at output midgray (exposed scene 0.236185 -> 0.18) with the analytic slope
  (cone response 0.97521), peak = Peak / Game Brightness, bleaching off (the function default is 1; it desaturates bright
  colors, which is a look and not neutral), BT.2020 gamut compression, followed by the same
  Roll-off (haze, bloom and HUD are added after the tone map). It matches vanilla at midgray and below but has no closing
  shoulder, so it is brighter than vanilla from the upper mid-tones up (+16% at an exposed scene value of 1, +39% at
  1.7). Cost: about 557 ps_3_0 instruction slots in each of the two tone map draws.
- **Native D3D9 for the pre-HUD copy** (`CreateTexture`, `StretchRect`, `SetTexture` on `get_native()`), not
  `create_resource` + `CustomShader::views`: D3D9 descriptor injection is untested here and the Devkit needs no view of this
  texture. The texture is released on `destroy_swapchain` / `destroy_device` (D3D9 only) before `Reset`.
- **`CustomShader::on_drawn` is not used.** It cancels the draw and replays it through the ReShade command list, which draws
  nothing for this game's full-screen quads: a black screen, and HUD copies piling up. The copy is done at the first
  0x707C2DA8 draw after the finish copy instead.
- Shader constants c150-c157 carry `shader_injection`. D3D9 constants are device-global, so the block must not overlap a
  register the game sets and reads later. Disassembling all 491 pixel shaders dumped with the Devkit shows the game reads
  c0-c13, c32-c41, c46-c49, c54-c55, c57-c66, c68-c71, c76-c108 and c192-c223 (c57, its punch fade parameter, is read by 174
  shaders, and 6 read c100-c107), so c109-c191 is the free range. Vertex shaders have their own constant registers.
- Preset Off and Vanilla still run the float16 upgrade, the HUD cap and the Game / UI split (at 203/203 it is the identity).
  The original clamped to white after every blend; Vanilla clamps the finished scene once, in the finish pass, after the haze
  sprites. It is not bit-identical to the original: the output is HDR10 with white at Game Brightness, while the original on
  an HDR desktop uses the Windows SDR brightness and the sRGB curve; 16-bit float also removes 8-bit rounding.

## Settings

Tone Mapper (Vanilla, None, Roll-off, PsychoV), Peak / Game / UI Brightness, color grading (Exposure, Highlights, Shadows, Contrast,
Saturation, Highlight Saturation, Blowout, Flare), Shadow Filter Width, Display Output (Intermediate Encoding, Swapchain Decoding, Gamma
Correction, Clamp Color Space, Force Borderless, Prevent Fullscreen, Encoding). Roll-off compresses the max channel in the
swap chain color space: identity up to 0.6 x Peak, Peak at 16 x Game Brightness (first estimate, not measured); it also runs after PsychoV.

## Verification

Build (from a Visual Studio x86 developer environment): `cmake --preset clang-x86` then
`cmake --build --preset clang-x86-relwithdebinfo --target binarydomain` (new or removed shader files need the configure step
again; use `clang-x86-release` for a Release build).
Copy `build32/RelWithDebInfo/renodx-binarydomain.addon32` next to
`BinaryDomain.exe` (never the Devkit addon by mistake).

Runtime checks done (author, in game): HDR10 on; Game and UI Brightness act separately; pause menu follows UI nits; the
startup videos are fine; the HUD pulse effect stays at UI brightness; muzzle flashes reach Peak Brightness (they are part of
the scene, not the HUD, so they follow Game Brightness); lamps reach Peak and show the warm bulb colour (Blowout whitens
them); Alt-Tab returns cleanly; fullscreen works (the mod runs it borderless). Devkit measurements: no NaN / Inf / negative
pixels in the final output; final output max luminance below the HUD buffer's after the cap.

## Known limitations / not tested

- The full resolution translucent layer is a float16 target now and its particle output is not clamped, so effects drawn there
  (muzzle flashes were observed reaching Peak Brightness) keep their range; the half resolution layer and the bloom stay 8-bit
  and clipped. Which effects use which layer was not catalogued.
- The blocky shadows inside the bus are in the base game too (compared with the add-on removed), not caused by the mod.
- The blinking ammo cache is a scene effect (not HUD, it stays with the HUD shaders off). It reaches about 650 nits, below Peak.
- The white clip of the Roll-off is an estimate; peak white of the brightest scenes was not measured over varied content.
- Device Reset / resolution change with the pre-HUD texture bound, other aspect ratios, fades to white, cutscenes without
  HUD sprites and a second HUD shader (0x1F45D316 draws before the first 0x707C2DA8) were not exercised.
- The game's own brightness / gamma option (c192) was only checked at its default.
