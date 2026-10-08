# RenoDX: Wolfenstein: The New Order

HDR mod for Wolfenstein: The New Order (Steam 201810, `WolfNewOrder_x64.exe`, id Tech 5, **OpenGL**, x64).

Status: experimental. Tested on one machine (RTX 5070 Ti, driver 617.14, 4K OLED, HDR10), ReShade 6.8.0 with add-on support.

## Install

1. Install ReShade 6.8 with add-on support for OpenGL (`opengl32.dll` in the game folder).
2. In `ReShade.ini`, under `[INSTALL]`, set `HookDirectX=1` (game closed). The HDR output goes through a D3D11 Display Proxy, and
   ReShade loaded as `opengl32.dll` does not hook D3D11/DXGI without it.
3. Copy `renodx-wolfensteintno.addon64` into the game folder.
4. Enable HDR in Windows. Any in-game display mode works (Windowed, Fullscreen windowed, Fullscreen).

Do not load the RenoDX Devkit together with this mod (both run a Display Proxy).

## Settings

| Setting | Default | Notes |
|---|---|---|
| Tone Mapper | PsychoV | Vanilla (game image clipped like its 8-bit buffers), None (untonemapped, clamped at Peak), PsychoV (PsychoV-17) |
| Peak Brightness | 1000 | display peak in nits |
| Game Brightness | 203 | scene paper white |
| UI Brightness | 203 | HUD and menus |
| Gamma Correction | 2.2 | how the game's gamma-encoded image is decoded (Off = sRGB, 2.2, BT.1886) |
| Exposure, Highlights, Shadows, Contrast, Saturation, Hue Correction | 1 / 50 / 50 / 50 / 50 / 100 | PsychoV only |
| Display Output group | | swap-chain decoding, gamma, custom color space, output clamp (advanced) |
| Use Display Proxy | On | Advanced; required: OpenGL has no HDR swap chain of its own |

## How it works

### Pipeline (gameplay)
- Materials write gamma-encoded scene colour into the 2x MSAA target 0xFA1 (+ MRTs 0xFA3 VT feedback, 0xFA5 normals). Each post stage
  resolves into the single-sample scene colour 0xFBF (mip chain). Reflections (SSR) go to 0xFA2.
- Luminance / bright-pass / glare / haze chain at 1/8 resolution (0x4A36BD2A, 0x58CB1B38, 0xF7C9D539, 0x08853A44, 0xB7D2D3B5,
  0xE4870070).
- **0xA85A9FE0** (GLSL, replaced): main post-process. DOF/blur via mips, radial blur, sharpen, glare add, desaturate, 1D curve
  `cbconversionlut`, overlay, film grain, `saturate` -> 16^3 colour LUT `dynamiccc`. Output into 0xFA1.
- 0xD7D85DFF copies the result to the back buffer, then the HUD is drawn.

### Signals
- The scene is gamma-encoded (materials end with `pow(x, 1/2.2)`) and, once the targets are float, carries real HDR range: measured
  steady highlights reach at least 20x game white (fire cores, light streaks, torch flames).
- The game's curve, grain and LUT expect values <= 1. The replacement keeps them on an SDR range with a **max-channel bridge**:
  identical to vanilla for every pixel <= 1; above white the colour is divided by its max channel before the curve/grain/LUT and
  multiplied back after (hue kept). The result is the `untonemapped_graded` signal.

### Tone mapping
- **PsychoV-17**, GLSL port from `src/games/doom-tda/include/psychov_17.glsl` (same as `indygreatcircle`), one change: `fma(...)`
  rewritten as `a * b + c` (GLSL 1.50). Peak relative to game white, BT.2020 gamut mode for HDR10. Slider mapping as in the HLSL
  PsychoV-17 mods (darktide, rotsp, wutheringwaves).
- PsychoV evaluation: suitable. The scene carries real range (>= 20x game white); against the earlier highlight-only roll-off,
  mid-tones (p50-p95) stayed within ~5% and the brightest 1% rolled off smoothly instead of clipping; highlight hierarchy kept;
  stays below Peak.
  PsychoV-30 was not used: no GLSL port exists, and its differences are in saturated extreme highlights.
- Game/UI split: the scene is scaled by Game/UI in the composite (`RenderIntermediatePass` convention); the proxy scales the whole
  frame by UI Brightness and clamps at Peak (`renodx::draw::SwapChainPass`).
- Settings reach the GLSL as a UBO (ReShade's OpenGL push-constant path, binding 0). The shader declares only the fields it uses,
  with explicit `layout(offset = N)` (`GL_ARB_enhanced_layouts`); `addon.cpp` `static_assert`s the same offsets and the std140 size.
- Film grain is divided by the bridge factor, so highlights carry no amplified grain (vanilla clipped them after the grain).
- Bloom: the game's bright-pass saturates before the glare, so glare strength matches vanilla (kept by decision).

### Resource upgrades (RGBA8 render targets -> RGBA16F, in place)
- In place (`use_resource_view_cloning = false`): clone + redirect left the 3D scene black on OpenGL.
- Exact sizes, as fractions of the **window client area**: full, 1/2, 1/8. The engine creates its full-screen targets before the swap
  chain exists (so "Output size" misses them) and sizes them from the window, which can have any shape in windowed mode.
  `OnCreateResourceFollowWindowSize` updates the three rules before RenoDX's own `create_resource` handler runs.
- Quarter resolution stays 8-bit on purpose: it holds the screen-distortion offsets (0xBE1EA7F8, range -0.5..1.5) and vanilla relies on
  the 8-bit target clamping them.
- Side effect fixed: the box-projected damage decal 0x97DA5C21 outputs negative alpha in its corners on purpose (harmless in UNORM);
  its replacement clamps like UNORM would. Its vertex shader 0x0686B625 is replaced only to pin attribute locations (see below).
- Known over-upgrade (accepted): 0xFA3 (VT feedback) and 0xFA5 (normals) are created exactly like the scene target 0xFA1 and get
  upgraded too. Their values stay in 0..1, so this only costs memory.

### OpenGL specifics
- Shader replacements are plain GLSL (`0x<HASH>.frag.glsl` / `.vert.glsl`). Original uniforms are kept active and in order (GL
  locations are per program). The game's `glBindAttribLocation` / `glBindFragDataLocation` are not carried into the replacement
  program, so multi-input/output shaders pin them with `layout(location = N)`.
- **Window modes:** the proxy presents to the same window as the GL context. The game's own `SwapBuffers` still presented the
  (stale) GL back buffer, and in the game's topmost fullscreen windowed mode that won over the HDR output (black screen). The mod
  detours the system `opengl32!wglSwapBuffers` (ReShade calls it after its present event, where the proxy presents) and skips it
  while the proxy is active.
- **ReShade overlays:** with the proxy there are two ReShade runtimes (OpenGL, `ReShade.ini`; D3D11 proxy, `ReShade2.ini`). The
  OpenGL overlay is invisible but took the same clicks at a different layout and autosaved the shared preset. The mod keeps it
  closed (`reshade_open_overlay`). Only the first runtime per window can block game input (the OpenGL one), so while the visible
  overlay is open the mod calls `block_input_next_frame()` on it every frame.

### Changes outside this folder (same branch)
- `src/utils/device_proxy.hpp` (`LOCAL (wolfensteintno)`): on OpenGL, the proxy's D3D11 shared texture is imported with
  `GL_EXT_memory_object_win32` (`GL_HANDLE_TYPE_D3D11_IMAGE_EXT`, dedicated, size 0, `glTexStorageMem2DEXT`). ReShade's GL
  `create_resource` only imports OPAQUE_WIN32 handles sized via Vulkan, which terminated the process on NVIDIA.
- `src/addons/devkit/addon.cpp`: swap chain size taken from the real back buffer when a stale create desc from the engine's 32x32
  helper window is pending.

## Known issues
- Vanilla issues, present without the mod (and without id5Tweaker): distant lamp lights flicker; small floor clutter (planks, tins)
  flickers at a distance.
- PsychoV-17's Blowout has no visible effect with a fixed adaptation state, so it is not exposed.
- Haze flare passes add into the scene after the composite; they are clamped at Peak by the proxy, not tone mapped.
- Tested only with id5Tweaker (`r_multisamples 2`, 60 fps cap); without forced MSAA is untested.

## Verification
1. Build target `wolfensteintno` (Clang x64 preset) -> `renodx-wolfensteintno.addon64`. After adding/removing shader files: CMake Configure.
2. `ReShade.log`: `GL present skipped while the proxy presents`, `wolfensteintno full upgrade follows window: WxH` (also half,
   eighth), a proxy `CreateSwapChainForHwnd` with `R10G10B10A2_UNORM`, no shader compile errors.
3. In game: HDR in Windowed, Fullscreen windowed and Fullscreen, including switching between them in game; Tone Mapper Vanilla /
   None / PsychoV differ; Game Brightness changes only the scene, UI Brightness the HUD.
4. ReShade menu: clicks change only what is clicked; the game ignores input while the menu is open; effect choices persist.
5. Distortion (glass, heat haze) looks like vanilla; no rectangular blobs on damage decals.
