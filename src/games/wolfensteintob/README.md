# RenoDX: Wolfenstein: The Old Blood

HDR mod for Wolfenstein: The Old Blood (Steam 350080, `WolfOldBlood_x64.exe`, id Tech 5, **OpenGL**, x64). Ported from
the Wolfenstein: The New Order mod (`wolfensteintno`); same engine, different shader hashes.

Status: experimental. Tested on one machine (RTX 5070 Ti, 4K OLED, HDR10), ReShade 6.8.0 with add-on support, with
id5Tweaker.

## Install

1. Install ReShade 6.8 with add-on support for OpenGL (`opengl32.dll` in the game folder).
2. In `ReShade.ini`, under `[INSTALL]`, set `HookDirectX=1` (game closed). The HDR output goes through a D3D11 Display
   Proxy, and ReShade loaded as `opengl32.dll` does not hook D3D11/DXGI without it.
3. Copy `renodx-wolfensteintob.addon64` into the game folder.
4. Enable HDR in Windows.

With the RenoDX Devkit loaded at the same time, name it `zz-renodx-devkit.addon64`. If the screen stays blank or the
game closes within seconds, the Devkit took over the Display Proxy at startup (`ReShade.log`: the
`device_proxy::OnPresent(reconfiguring ...)` line carries a `[RenoDX DevKit]` tag). That happened in 5 of 5 launches on
2026-10-21; a Devkit build that always loses the handler election (a Devkit-side change, not part of this mod) fixed it.

## Settings

Tone Mapper, Gamma Correction and the grading sliders are shown in the Intermediate / Advanced settings modes.

| Setting | Default | Notes |
|---|---|---|
| Tone Mapper | PsychoV | Vanilla (game image clipped like its 8-bit buffers), None (untonemapped, clamped at Peak), PsychoV (PsychoV-17) |
| Peak Brightness | 1000 | display peak in nits |
| Game Brightness | 203 | scene paper white |
| UI Brightness | 203 | HUD and menus |
| Gamma Correction | 2.2 | how the game's gamma-encoded image is decoded |
| Hue Correction, Exposure, Highlights, Shadows, Contrast, Saturation | 100 / 1 / 50 / 50 / 50 / 50 | PsychoV only |
| Display Output group | | swap-chain decoding, gamma, custom color space, output clamp (advanced) |
| Use Display Proxy | On | Advanced; required: OpenGL has no HDR swap chain of its own |

## How it works

### Pipeline (gameplay)
- Materials write gamma-encoded scene colour into the 2x MSAA target 0xFA1 (+ MRTs 0xFA3, 0xFA5). Resolves go into the
  single-sample scene colour 0xFC0 (0xFBF is the depth texture here, unlike New Order).
- Luminance / bright-pass / glare at 1/8 resolution (0x4A36BD2A, 0x1F5C414B, 0x58CB1B38, 0xF7C9D539), haze after the
  composite (0x08853A44, 0xB7D2D3B5, 0xE4870070).
- **0x9DBD325F** (GLSL, replaced): main post-process. DOF/blur via mips, radial blur, glare add, desaturate, 1D curve
  `cbconversionlut`, overlay, film grain (gated by `_fa_[21].y`), `saturate` -> 16^3 colour LUT `dynamiccc`. Output into
  0xFA1. Unlike New Order's 0xA85A9FE0 it has no sharpen block, and the grain uniforms are `_fa_[21..23]`.
- 0xD7D85DFF copies the result to the back buffer, then the HUD is drawn.

### Signals
- The scene is gamma-encoded and, once the targets are float, carries real HDR range. Measured with Tone Mapper None and
  Peak 10000: a fireplace reaches about 30x game white (about 6000 nits), a lamp about 12x, the muzzle flash about 8x.
- The game's curve, grain and LUT expect values <= 1. The replacement keeps them on an SDR range with a **max-channel
  bridge**: identical to vanilla for every pixel <= 1; above white the colour is divided by its max channel before the
  curve/grain/LUT and multiplied back after (hue kept). Film grain is divided by the same factor.

### Tone mapping
- **PsychoV-17**, GLSL port from `src/games/doom-tda/include/psychov_17.glsl`, peak relative to game white, BT.2020 gamut
  mode for HDR10. Game/UI split as `RenderIntermediatePass`: the scene is scaled by Game/UI in the composite, the proxy
  scales the frame by UI Brightness and clamps at Peak (`renodx::draw::SwapChainPass`).
- **Local change to the port (project decision):** the published PsychoV-17 calls the plain Naka-Rushton curve, whose
  `p` is an *asymptotic* peak (`src/shaders/tonemap/naka_rushton.hlsl`), so a 12x lamp stopped at about 945 nits with
  Peak 1360. The shader uses the White Clip form from the same file instead, with the clip fixed at **30x game white**
  (the measured scene top, scaled by Exposure), never below 3x Peak (closer to Peak the curve turns into a near-linear
  hard clip). Brighter input is capped by its max LMS channel (chromaticity kept). Highlights / Contrast reshape the
  input before the clip and are not compensated. So the scene top reaches Peak only while Peak <= 10x Game Brightness
  (about 2030 nits at Game 203); above that the 3x floor sets the clip and the curve approaches the published one.
  Measured at Peak 1360: the fireplace lands on Peak, its flicker range stays visible below it.
- In BT.2020 gamut mode PsychoV can return colours outside BT.709 as slightly negative BT.709 values (0.3-1% of pixels,
  about -0.08 nits at most); the proxy converts to BT.2020 before clamping, so they are valid colours, not errors.

### Resource upgrades (RGBA8 render targets -> RGBA16F, in place)
- In place (`use_resource_view_cloning = false`), exact sizes as fractions of the window client area: full, 1/2, 1/8.
  The engine creates its full-screen targets before the swap chain exists, so `OnCreateResourceFollowWindowSize` keeps
  the rules on the window (class `Wolfenstein The Old Blood`).
- Quarter resolution stays 8-bit on purpose: it holds screen-distortion offsets (0xBE1EA7F8) that vanilla clamps.
- Side effect fixed: the box-projected damage decal **0x7B8FA202** writes negative alpha in its corners (edge fade
  `1 - |localPos|^8`), harmless in UNORM, a black blob with green fringes on float targets. Its replacement clamps the
  output like UNORM. Its vertex shader **0x25010C88** is replaced only to pin attribute locations. Old Blood's decal has an
  extra fog input (`vofi_TexCoord5`), so New Order's 0x97DA5C21 / 0x0686B625 replacements do not apply.

### OpenGL specifics
- Shader replacements are GLSL (`0x<HASH>.frag.glsl` / `.vert.glsl`); original uniforms kept in order; attribute and
  output locations pinned with `layout(location = N)` because the replacement program does not get the game's
  `glBindAttribLocation` / `glBindFragDataLocation`.
- The mod detours `opengl32!wglSwapBuffers` and skips it while the proxy presents; the hidden OpenGL ReShade overlay is
  kept closed and game input is blocked while the proxy overlay is open (same as New Order).
- Shared code outside this folder (same branch, separate commit, not upstream): `src/utils/device_proxy.hpp` imports
  the proxy's D3D11 shared texture with `GL_EXT_memory_object_win32` (from the New Order branch).

### Bloom and particles (checked, kept as is)
- Bloom: the bright-pass 0x58CB1B38 clamps its threshold term (`saturate(total - threshold - bias)`) but adds `glare * 4`
  and `frame` unclamped; vanilla's 8-bit target clamped the sum at 1. With the float 480x270 chain the glow can go
  slightly above 1 (measured 1.21 in the bloom target, lamp glow about 1.26 / 340 nits in the final image). Kept by
  decision: it looks right, the lamp itself still carries the HDR range.
- Smoke: 0x30270368, 0x77B690CD (alpha x4) and 0x4BED1C4E write unclamped alpha into the float scene target. They are
  drawn in the fireplace smoke (snapshots), which looks correct; the blend state is not readable on OpenGL, so this is
  a visual check, not a measurement. First suspects if smoke ever looks too bright.

## Known issues
- Alpha of the scene target reads 2.0 after the haze passes (UNORM clamped it to 1); no visible effect found.
- The 1x1 average-luminance target and a 160x120 helper stay 8-bit; their effect on bloom in bright scenes is untested.
- Untested: without id5Tweaker.

Tested Bink cutscenes (2026-10-21): one draw (0x13072711) converts the 8-bit YUV planes straight into the back buffer,
values 0..1, so video white lands at UI Brightness (SDR, no inverse tone map); picture and colours correct.

Tested windowed (2026-10-21): 3818x2104 (maximised window) and 2560x1440. The upgrade rules follow the window
(`full/half/eighth upgrade follows window` in the log), scene 0xFA1 / 0xFC0 and the bloom target are RGBA16F at the
window size, the quarter-size map stays 8-bit; picture, HUD and highlights correct.

## Verification
1. Build target `wolfensteintob` (Clang x64 preset) -> `renodx-wolfensteintob.addon64`. After adding/removing shader
   files: CMake Configure.
2. `ReShade.log`: `Registered runtime replacement` for 0x9dbd325f, 0x7b8fa202, 0x25010c88; `GL present skipped while
   the proxy presents`; `utils::device_proxy::OnPresent(reconfiguring proxy runtime ...)` without a `[RenoDX DevKit]`
   tag (the mod owns the proxy); no shader compile errors.
3. In game: HDR picture; Tone Mapper Vanilla / None / PsychoV differ; Game Brightness changes only the scene, UI
   Brightness the HUD; no black blobs where bullets hit walls; with PsychoV at default Game Brightness and Peak <= 2000 nits, a fireplace
   reaches Peak.
