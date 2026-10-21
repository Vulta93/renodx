# RenoDX Devkit MCP

This guide covers the current RenoDX devkit and MCP workflow for inspecting a running game, dumping shaders, iterating on replacements, and validating HDR resource behavior.

See also:

- `docs/CONTRIBUTING.md`

## 1. What the pieces are

The live inspection stack has three parts:

1. the game-side `devkit` addon
2. the `renodx-mcp-bridge` process
3. an MCP client such as Codex or another JSON-RPC MCP host

The game addon exposes frame, shader, and resource inspection over a named pipe. The bridge exposes that pipe as an MCP tool server.

## 2. What to build

For live inspection and MCP-driven modding, build:

```powershell
cmake --preset clang-x64
cmake --build --preset clang-x64-debug --target devkit mcp_bridge
```

The debug artifacts are:

- `build/Debug/renodx-mcp-bridge.exe`
- `build/Debug/renodx-devkit.addon64`

The bridge advertises the DevKit tool surface up front. If there is exactly one live backend pipe, it auto-connects on first use. Otherwise use the bridge tools first:

- `renodx_list_connections`
- `renodx_connect`

## 3. Tool groups

The exact MCP schema can evolve, but the current workflow is built around these groups.

### Connection and frame state

- `renodx_list_connections`
- `renodx_connect`
- `devkit_status`
- `devkit_select_device`
- `devkit_queue_snapshot`
- `devkit_snapshot_summary`

### Frame inspection

- `devkit_list_shaders`
- `devkit_list_draws`
- `devkit_get_draw` (`includeConstantValues: true` adds the constant values the draw saw, see section 6)
- `devkit_get_shader`
- `devkit_list_replacements` (which registered shader replacements drew in the snapshot, see section 6)

`devkit_queue_snapshot`, `devkit_analyze_resource` and `devkit_watch_region` take `delaySeconds` (0-30): the tool waits that
long before it queues the request. Use it when the game is paused while you talk to the agent: start the call, unpause,
and the capture lands on gameplay instead of the pause menu. The wait happens before queueing, so it does not count
against the tools' own 15 s wait for the next present. It runs on the MCP connection's thread: other Devkit tool calls wait
until it ends, and the arguments other than `delaySeconds` are only checked after it.

### Resource inspection

- `devkit_analyze_resource`
- `devkit_watch_region`
- `devkit_dump_resource_with_hash`
- `devkit_set_resource_clone`

### Texture replacement rules (experimental)

- `devkit_set_texture_replace_enabled`
- `devkit_set_texture_replace_rules`
- `devkit_get_texture_replace_state`
- `devkit_list_texture_replace_observations`
- `devkit_clear_texture_replace_observations`
- `devkit_dump_texture_replace_observation`

### Shader dump and live iteration

- `devkit_dump_shader`
- `devkit_set_tools_path`
- `devkit_set_live_shader_path`
- `devkit_load_live_shaders`
- `devkit_unload_live_shaders`

Default devkit folder layout under the ReShade base path:

- `renodx-dev/live`
- `renodx-dev/dump`
- `renodx-dev/boot`

`boot/` is scanned and decoded at devkit startup so matching `initial_data` uploads can be replaced immediately.
If boot assets are found, devkit texture replacement is enabled automatically.
Texture replacement lookup checks `live` first, then the decoded `boot` cache.

## 4. Recommended investigation loop

For a new game, work backward from the end of the frame.

1. Connect and choose the active device.
2. Queue a snapshot and wait for the next present.
3. Inspect late draws first.
4. Identify the likely scene composite, UI, and final blit stages.
5. Inspect the candidate draw with `devkit_get_draw`.
6. Read back the relevant resources with `devkit_analyze_resource`.
7. Only after the target pass is clear, dump and edit shaders.

This usually answers the important question first:

- is HDR already gone before the final blit
- or is the final blit still carrying recoverable scene data

Do not start from the first bright shader you find. Start from the latest pass that still has scene HDR potential.

## 5. Choosing the right device

Many games expose multiple D3D devices. Only one may be the active renderer.

Check `devkit_status` and `devkit_select_device` before analyzing anything:

- active swapchains
- pipeline counts
- tracked shaders
- captured draws

If one device shows `0` draws or `0` tracked shaders, it is usually not the one you want.

## 6. Reading frame structure

Use `devkit_list_draws` to find likely late-frame candidates, then `devkit_get_draw` for the exact pass.

The useful fields are:

- pixel and vertex shader hashes
- render target view details and format
- SRV inputs and format
- blend state
- swapchain and clone flags

For HDR work, the most important draw categories are:

- the last scene composite or tonemap pass
- later SDR UI and text passes
- the actual final copy or swapchain pass

If the final pass is only a one-texture blit from an already SDR target, it is not the place to recover HDR.

### Constant values (`devkit_get_draw` with `includeConstantValues`)

Exposure, white point, LUT strength and grade parameters usually live in constants. With `includeConstantValues: true`,
`devkit_get_draw` adds, as 32-bit words read both as float (`floatValues`) and as uint (`uintValues`):

- Per constant buffer bound to the pixel stage (draws) or compute stage (dispatches): the bytes copied when the snapshot
  recorded the draw, because games rewrite the same buffer between draws. At most 4096 bytes from the bound offset
  (`valuesTruncated` when that cap was reached; a whole-buffer bind of exactly 4096 bytes also reports it). `valuesSource`:
  - `cpu_cache`: the bytes the CPU last wrote through map/unmap or `update_buffer_region` (`utils::constants`). This is the
    D3D10/11 path, and OpenGL uniform buffers written the same way. D3D9 has no constant buffers (see the shader constants
    below).
  - `untracked`: the Devkit setting **Trace Constant Buffers** (Settings > Trace, ReShade.ini `TraceConstantBuffers`) was
    off when the game started (it only registers its hooks at startup, so enable it and restart), or the buffer was not created
    as a constant buffer, or it was mapped at that moment.
  - `not_read_by_shader`: the active shader's reflected bindings do not include this buffer. When the shader's bindings are
    not reflected, every bound buffer is captured.
  - `unsupported_api`: D3D12 and Vulkan. Their games write constant buffers through persistent maps or GPU copies, which the
    CPU-side cache never sees, so it would only report zeros. Use the root/push constants below, or read the buffer's
    layout from the decompiled shader.
- `pushConstants` for the active stage (pixel for draws, compute for dispatches), at most 1024 words each (longer ranges
  are cut without a flag), `firstValue` being the word index of `floatValues[0]`. They do not need Trace Constant Buffers.
  - D3D12 root constants and Vulkan push constants, only for the root signature / pipeline layout bound for the draw's bind
    point (the one its descriptor tables are bound with). When no descriptor table was bound since the last root signature
    change, that layout is unknown and the values of every layout pushed during the snapshot are listed (check
    `layoutHandle`).
  - D3D9 shader constants: `layoutParam` 5/6/7 are the pixel shader's float/int/bool constants. Float and int register N start
    at word 4 * N; bool register N is word N.
  - OpenGL `glUniform*`: word 4 * location. One array per context, not per program: a location means something different in
    every program, so a word can come from another program. Arrays and matrices fill consecutive words from 4 * location,
    overlapping the next locations. `glProgramUniform*` is not hooked by ReShade, so those writes are missing.

Limits: values are recorded only from calls the game makes during snapshot frames (like the other bindings the snapshot
records). A constant the game set before the snapshot frame and did not set again is missing, or shows a value from an
earlier snapshot on D3D9 and the D3D11/OpenGL immediate context, whose state is kept between frames. Vertex shader
constants are not included. Reflection names for the words are not resolved: read them from the decompiled shader's
cbuffer layout.

Constant buffer binds in snapshot draws (`constantBuffers`, with or without values) are the pixel or compute stage's, like
SRVs and UAVs: before this change a vertex shader bind on D3D11 could overwrite the pixel shader's slot.

### Replacement status (`devkit_list_replacements`)

Lists every shader hash a loaded add-on registered a replacement for on the device: `compileTime` (registered for
replacement when the game creates a pipeline) and `runtime` (registered for replacement when a pipeline is bound). RenoDX
mods usually register both; the Devkit's live shaders are runtime only. For the current snapshot it counts, per hash,
`snapshotDraws` (draws whose pipeline uses the original hash; snapshot draws always record original hashes) and
`snapshotDrawsReplaced` (of those, draws whose pipeline had the replacement in effect when the draw was captured: created
with it, or with a bind-time replacement pipeline built for that stage). Loading or unloading live shaders afterwards does
not change these counts; take a new snapshot. `snapshotPending` is true while a snapshot is queued or capturing. Reading it:

- `snapshotDraws: 0`: the shader did not draw in the snapshot frame (wrong scene, or the hash is not the one the game uses).
- `snapshotDraws` above `snapshotDrawsReplaced`: the original shader still drew for some draws, for example a pipeline created
  before the mod loaded or a replacement that failed to compile.

It answers "is my shader live and drawing here?" without reading `ReShade.log`. Queue a snapshot first.

## 7. Shader inspection and dumping

`devkit_get_shader` returns metadata plus optional disassembly or decompilation text over MCP. It does not send raw shader bytecode by default.

If the game ships its own `dxcompiler.dll` or other conflicting shader tools, point `devkit_set_tools_path` at the repo `bin` directory first so devkit loads the repo toolchain instead of the game-local copy.

Use `devkit_dump_shader` when you want the original compiled shader on disk.

Important rule:

- dump original `.cso` files outside `src/games/**`

Do not place dumped `.cso` files in your live shader folder. If both a dumped binary and an editable HLSL replacement exist for the same hash, the binary can shadow the editable source.

Recommended layout:

- originals: `tmp/{game}/original/`
- editable live replacements: `src/games/{game}/`

For dumped DXBC shaders, the repo-standard external decompiler is:

- `bin/cmd_Decompiler.exe`

If it is missing, bootstrap the repo tools from the repo root before using the decompiler:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup-dev-env.ps1 -Install
```

That installs `cmd_Decompiler.exe` into `.\bin`. For the full tool bootstrap and the remaining optional reverse-engineering binaries, see `docs/CONTRIBUTING.md`.

Devkit MCP can already disassemble tracked DirectX shaders. Decompilation is best-effort: DXBC SM4/5 shaders use `cmd_Decompiler.exe` from the configured tools path, while DXIL SM6 shaders use the internal DXC decompiler path. The external tools in `.\bin` are still useful when you want standalone files on disk or alternate reverse-engineering workflows.

Example:

```powershell
bin\cmd_Decompiler.exe --decompile tmp\road96\original\0x1561F9BA.cso
```

## 8. Live shader iteration

The safe loop is:

1. dump original shader to `tmp`
2. decompile it when available, or fall back to disassembly / external tools
3. copy the editable HLSL into `src/games/{game}`
4. point `devkit_set_tools_path` at your repo `bin` folder if the game has conflicting DXC DLLs
5. point `devkit_set_live_shader_path` at that folder
6. call `devkit_load_live_shaders`
7. verify with `devkit_get_shader` that the source is `File`

Keep the live shader folder clean:

- only editable HLSL replacements
- no dumped `.cso`
- no unrelated experiment artifacts

When editing a dumped shader:

- preserve the original register bindings
- keep the vanilla logic visible in comments if you are testing an HDR change
- do not silently delete the original behavior when you still need an SDR reference

## 9. Resource cloning and HDR validation

Resource cloning is often the fastest way to prove whether a scene path can carry HDR.

Important distinction:

- the original view may still be `r8g8b8a8_unorm_srgb`
- the clone can be `r16g16b16a16_float`

That means:

- the original view can look clipped
- the clone can still hold values above `1.0`

Use `devkit_set_resource_clone` to enable cloning, then analyze the same resource with:

- the original view
- `preferClone: true`

This is the authoritative way to answer whether the upgraded path is actually preserving HDR data.

## 10. Dump formats

`devkit_analyze_resource` can write both preview and HDR-preserving outputs.

- `.png`
  SDR preview only
- `.exr`
  HDR-preserving `RGBA16F` dump

Do not use the PNG alone to decide whether HDR survived. Use:

- the numeric analysis
- the EXR dump
- or the clone readback stats

The PNG is for quick visual inspection, not for HDR truth.

### Regions and nits

`devkit_analyze_resource` also accepts:

- `regionX`, `regionY`, `regionWidth`, `regionHeight` (pixels, all four together): only that box is copied and analyzed, so
  every statistic, preview and dump describes the region. Use it to isolate one light source or UI element.
- `signal`: `raw` (default), `pq` or `scrgb`. With `pq` (HDR10 ST 2084 targets, BT.2020) or `scrgb` (linear, 1.0 = 80 nits,
  BT.709) the result adds `analysis.nits`: luminance and max-channel stats in nits, luminance percentiles (p50/p90/p99/p99.9)
  and the share of pixels above 203/400/1000/4000 nits. Only use it on a resource that actually holds that encoding, such as
  the swap chain or an HDR10/scRGB output target.

### Measure point

`devkit_analyze_resource` takes `measurePoint`, and every result reports which one it used:

- `present` (default): read inside the Devkit's own `present` handler, before ReShade's effects and overlay. Add-ons whose
  `present` handlers run after the Devkit's (ReShade calls them in load order; add-ons in the game folder load in directory
  order, which is why the Devkit is installed as `zz-renodx-devkit.addon64`) are not included.
- `frame_end`: read in `reshade_present`, which ReShade raises after every add-on's `present` handler and after its own
  effects and overlay, the same point as the watch's end-of-frame measurement. Includes the ReShade menu, FPS/clock and any
  active effect such as Lilium: disable them for clean numbers. A back buffer is read at the frame end of its own swap chain
  (as the watch does). Needs a swap chain with a ReShade effect runtime on the game's device (otherwise the call times out
  after 15 s; so does a D3D9 present that ReShade skips). Not verified with a RenoDX DX9 proxy mod, where the effect
  runtime runs on the proxy device: expect the timeout there and use `present`. Refused on Vulkan for now: the analysis flushes ReShade's immediate command list and waits
  for the queue, and inside `reshade_present` that would submit ReShade's effect commands without the present's wait
  semaphores.

### Multisampled resources

On D3D9, D3D10/11 and OpenGL, `devkit_analyze_resource` resolves a multisampled texture (ReShade's `resolve_texture_region`,
which averages the samples) into a temporary single-sample texture of the source's own format (an sRGB target stays sRGB; a typeless one is resolved
with the typed format of the analyzed view, else its default typed format) and analyzes that; the result reports
`resolvedFromSampleCount`. The whole texture is resolved on every API (D3D10/11 cannot resolve a box, and an OpenGL
multisample blit must keep identical bounds), then the region is copied out of it. Refused, besides D3D12/Vulkan and
depth-stencil: formats D3D10/11 cannot resolve (integer formats), and a multisampled OpenGL default framebuffer (ReShade's
GL resolve flips its rows, which a multisample blit does not allow; analyze the texture the game renders into before
presenting). OpenGL does not report resolve support, so an integer multisampled texture there keeps one sample instead
of averaging. `devkit_dump_resource_with_hash` shares this path. The numbers describe the resolved image, which is what the game's own resolve produces
for a plain box resolve. Refused: multisampled depth-stencil resources, and every multisampled resource on D3D12 and
Vulkan, where the resolve needs the texture in a resolve-source state the tool does not know. `devkit_watch_region` still
refuses multisampled textures: watch the texture the game resolves into instead.

### Watching a region over time (`devkit_watch_region`, D3D9Ex, D3D11, D3D12, Vulkan, OpenGL)

`devkit_watch_region` answers "which draw changes this region of this buffer, and how?" without a snapshot or screenshots.
The state of a texture after command N equals its state before the next command that writes it, so the devkit measures the
region before every command that writes the texture and once more at the end of every frame. Writers are draws with the
texture bound as a render target, dispatches with it bound as a compute UAV, and copies, render target and UAV clears,
resolves and updates that target it. The reduction is a small compute shader (`watch_region_reduce.cs_5_x.hlsl`) that writes
one row of an R32_UINT stats texture per measurement; the stats texture is read back two frames later.

The end of a frame is ReShade's `reshade_present` event. ReShade raises `present` for every add-on first and only then runs its
effect runtime, which ends with `reshade_present`, so the end-of-frame measurement includes everything other add-ons wrote in
their present handlers (for example a mod's swap chain proxy pass), whatever the add-on load order. A swap chain without a
ReShade effect runtime never reaches `reshade_present`, so a watch on it times out.

A watch belongs to one effect runtime (one swap chain): a back buffer is watched by the runtime that presents it, any
other texture by the first runtime to reach the end of a frame. Only that runtime's frame ends measure, so a game with
two swap chains presenting on different threads never runs a watch's frame-end work twice at once. If that runtime is
reset or destroyed (a resize, a fullscreen switch or the window closing; ReShade raises `destroy_effect_runtime`), the
watch fails ("The swap chain the watch measured on was reset or destroyed"), so a resize always ends a running watch,
whatever texture it watches. A failed or canceled watch is finished by the next frame end of any runtime presenting on
the same command queue (those frame ends hold that queue's lock, like the starting runtime's), or, on D3D12, when that
queue is destroyed, or when the device is destroyed. When the tool call times out (the swap chain stopped presenting),
the watch is canceled at once: the hooks stop measuring, and the next frame end finishes it.

On the back buffer the end-of-frame measurement (`endOfWatch`, the final step of a trace and `verify` at that point) also
includes everything ReShade draws there before `reshade_present`: in `runtime::on_present` (`external/reshade/source/runtime.cpp`)
the enabled effects run first (`render_effects`), then `draw_gui` draws the overlay (`runtime_gui.cpp`: the ReShade menu, the
splash/loading banner, the FPS/clock/frame-time/preset-name statistics, add-on settings windows while the menu is open, and
whatever add-ons draw in the `reshade_overlay` event, which fires every frame even with the menu closed, for example the HDR
Analysis Logger's region outline), and only then is `reshade_present` raised. With a resolved or format-converted back buffer
the result is copied back before the event, so it is included as well. For clean final numbers, disable visible effects, close
the ReShade menu, turn off the FPS/clock statistics and any add-on on-screen drawing before watching the back buffer.
Measurements on any other texture are not affected.

Arguments: `resourceViewHandle` and `preferClone` (as for `devkit_analyze_resource`; the backing resource or its clone is
watched), the optional region (`regionX`/`regionY`/`regionWidth`/`regionHeight`, default: whole texture), `signal`
(`raw`/`pq`/`scrgb`), `frames` (default 1, max 3600), `shaderHash` (measure only writers using that shader),
`includeUnchanged` (also list writers that did not change the region, `frames: 1` only), `verify` (read the region back to
the CPU at one measurement point at the end of the watch and compare it with the GPU statistics of that same point:
`verify.status` is `match`, `mismatch` with the differing fields, or `skipped` when no such point was measured; on D3D11 and on a D3D12/Vulkan back buffer the point is the end of the last frame) and
`scratchBudgetMB` (D3D9/D3D12/Vulkan, default 512, see below). The call blocks until done.

- `frames: 1` returns `trace`: every writer that changed the region, in command order, with `commandIndex` (ordinal among the
  frame's draws, dispatches and copies, in submission order on D3D12/Vulkan; offset from `devkit_list_draws` indexes, so match
  by shader hash and order), shader hashes and region stats before and after (`maxLuminance`, `minLuminance`, `meanLuminance`,
  `maxChannel`, pixel counts above 1.0, negative, non-finite, black, and above 203/400/1000/4000 nits with a signal). Render
  pass writers (D3D12 render passes, Vulkan render pass instances) also list their `draws`.
- Every call returns `writers`, aggregated per writer (kind + pixel/vertex/compute hash; render passes by the set of their pixel
  shaders): executions, frames seen, how often the region changed, min/mean/max of the region peak and mean before and after,
  how often the peak was lowered or raised, how often values went from above 1.0 to at most 1.0, and `afterPeakMode` (the most
  repeated after-peak, a ceiling candidate).
- Writers whose state before or after could not be measured are never dropped: each one is in `trace` with `measured: false`,
  `unmeasuredSide` and `unmeasuredReason`, counted in `writers[].unmeasuredExecutions`/`unmeasuredReasons`, in
  `unmeasured.reasons`, and its frame is listed in `unmeasured.frames`. Reasons: `slot_limit` (more than 1023 measurements in a
  frame; on D3D9 more than the atlas holds, see below), `scratch_budget`, `scratch_pool_empty`, `state_unknown`, `inside_render_pass`, `other_queue`, `resubmitted`,
  `untracked_command_list`, `bundle`, `no_later_measurement`, `descriptor_table` (a D3D12/Vulkan dispatch that can reach
  the texture only through a bound descriptor table: bindless engines keep most textures in one table, so this does not
  prove a write and gives no known state; no barrier or copy is issued for it), `cross_queue_not_ready` (D3D12: see
  cross-queue points below).
- `endOfWatch` holds the region stats measured at the end of the last frame (D3D9/D3D11: always; D3D12/Vulkan: only for the
  presented back buffer). It includes every write made during present, including passes other add-ons draw there.
- Statistics are luminance (BT.709 weights on raw values, or nits for `pq`/`scrgb`); the GPU path computes no percentiles.

**D3D11** measures in place on the immediate context: the region is copied into a scratch texture and reduced right away. The
compute slots the pass uses (CS shader, b0, t0, u0) are saved and restored with native D3D11 calls. Deferred contexts are skipped.

**D3D9** has no compute shaders or UAVs, and the readback fence needs **D3D9Ex** (`utils::gpu_fence`'s D3DKMT backend; the
Devkit calls `gpu_fence::Use`, which finds a D3D9Ex device's GPU handles when the device starts). A RenoDX mod that runs the
display proxy already creates the device as D3D9Ex, and so does any wrapper that creates it with `CreateDeviceEx` (for
example dxwrapper's `D3d9to9Ex = 1`, used for Prince of Persia: The Two Thrones); the Devkit alone does not, so a watch on a
plain D3D9 device is refused with "DX9 watches need D3D9Ex: run the game with a RenoDX proxy mod or a wrapper that creates
the device with CreateDeviceEx [...]". Force DX9Ex
only works for games that start windowed or borderless: ReShade upgrades the device with `CreateDeviceEx` and no fullscreen
display mode (`external/reshade/source/d3d9/d3d9.cpp`), which D3D9Ex rejects for exclusive fullscreen (`D3DERR_INVALIDCALL`,
the game closes at boot; seen in Prince of Persia: The Sands of Time). A proxy mod also turns exclusive fullscreen into
borderless, so it does not hit this. At a
measurement point the region is only copied (native `StretchRect`, point filter, same format) into its slot of one atlas
texture: a grid of region-sized slots, as many as `scratchBudgetMB` and the device's maximum texture size allow, at most
1023 per frame plus the end-of-frame one (`atlasSlots` in the output; writers past the last slot are `slot_limit`). No game
state is touched during the frame. A source `StretchRect` cannot copy from fails the watch with that message. Only 2D textures
and surfaces are watched, and a region has at most 4096x4096 pixels (the reduction counts pixels in float, exact up to 2^24).
At the end of the frame (inside the scene ReShade opens for its present work, so the passes issue no `BeginScene`/`EndScene`
of their own) a pixel shader (`watch_region_reduce_dx9.ps_3_0.hlsl`) reduces every used slot: each output pixel reduces an
8x8 block, the intermediate levels hold the 16 statistics in four float render targets (MRT), and the last pass writes one
row of the stats target (a whole 4K region takes 4 passes). The game's state is captured once in a `D3DSBT_ALL` state block
and applied after the passes; render targets and the depth-stencil surface, which state blocks do not record, are saved and
restored separately. The passes set a full known state first (no depth/stencil, blending, alpha test, scissor, clip planes,
fog or sRGB write; colour writes on for all four targets; point sampling with clamp and no sRGB read). The stats target is
copied to the readback ring (`GetRenderTargetData`), the device is flushed and the watch's fence is signalled; a later frame
end reads the readback once the fence passed. The luminance mean is a float sum (the compute path uses 64-bit fixed point);
its error stays far below `verify`'s tolerance. Non-finite pixels are detected with range comparisons (SM3 has no bit casts);
`verify` checks the count. A device `Reset` (resize, fullscreen switch) fails the running watch, as on every API. gpu_fence
binds the device's GPU handles only when the device is created; whether a D3D9Ex `Reset` replaces them is not known, so a
watch started after a `Reset` that times out on its fence points there.

**OpenGL** (ReShade needs GL 4.3, so compute and image atomics are always there) records in execution order on one context
like D3D11 and has no cheap state-free in-place reduction, so it works like D3D9: a measurement point only copies the region
into its cell of a grid atlas, and one compute dispatch at the end of the frame reduces every cell
(`watch_region_reduce_gl.comp.glsl`, GLSL text compiled by the driver; the hand port of the HLSL reduction writes the same
`r32ui` stats words, so decoding, comparison and `verify` are shared). Details that matter:

- Only the context that presents the swap chain is measured; events on other contexts (loader threads, helper windows) are
  ignored before the watcher lock is taken, device-level updates included.
- Writers are found by asking GL, not from ReShade's events (it raises none for `glDrawBuffers` or DSA attachment changes):
  at every draw the draw framebuffer and its enabled draw buffers (for framebuffer 0 only `GL_DRAW_BUFFER0` is read, and only
  `GL_BACK*`/`GL_LEFT`/`GL_RIGHT`/`GL_FRONT_AND_BACK` count), at every dispatch the image units written. That is about 10-25
  GL queries per draw while a watch runs (none otherwise); with a threaded driver they can cost real frame time, and draws
  outside `shaderHash` with no event open are skipped before the queries. Copies, blits, clears, resolves, `glTexSubImage*`
  (also from a pixel unpack buffer, reported as `update`) and `glCopyTexSubImage*` arrive as ReShade events; those to another
  mip level or array layer than the watched one are ignored. Not seen: `glBlitFramebufferEXT`, `glBlitFramebufferANGLE`
  and `glCopyImageSubDataNV` (ReShade hooks only the core `glBlitFramebuffer`, `glBlitNamedFramebuffer` and
  `glCopyImageSubData`, and some drivers return a different entry point for the extension names; a texture a game refills
  that way shows no writer although its content changes every frame, and the end-of-frame numbers are still right.
  Measured on Wolfenstein: The New Order: in a level it resolves its 2-sample scene into the single-sample scene texture
  with `glBlitFramebufferEXT`, called from the game executable, at an entry point that differs from the core one), `glCopyTexImage1D/2D`
  (ReShade's hooks raise no event for them either; that game does not import them), `glGenerateMipmap` (levels above 0), image stores from
  fragment shaders, draws into `glTextureView` views of the watched texture, and a cube map face other than face 0 (the
  attachment query returns no face). A `glClear` of framebuffer 0 counts whatever its draw buffer is.
- The region rectangle is passed unchanged to ReShade's `copy_texture_region`, exactly like `devkit_analyze_resource`, so both
  tools read the same pixels. ReShade's copy from the default framebuffer flips rows, so on the back buffer the rectangle may
  count rows from the bottom (OpenGL's origin); check with a known pixel before trusting a corner region.
- The atlas is the non-sRGB twin of an sRGB source (`glCopyImageSubData` copies the bits and the shader applies the sRGB
  decode, like D3D11's sRGB view); the default framebuffer is measured as stored. Supported: RGBA8 (also sRGB), RGB10A2,
  R11G11B10F, RGBA16, RGBA16F, RGBA32F colour textures, renderbuffers and the default framebuffer (a back buffer without
  alpha bits, reported as an x8 format, is watched as RGBA8). Refused: multisampled,
  depth/stencil and three-channel formats (`GL_RGB8`, `GL_RGB16F`, `GL_RGB32F` have no image format), and draws into
  `glTextureView` views of the watched texture are not seen.
- The frame-end pass changes four things in the game's GL state and restores them: the current program, `GL_ALPHA_TEST`
  (contexts that are not core profile, which covers ReShade's own test: `GL_ARB_compatibility` listed, or created with `wglCreateContext`; its `bind_pipeline` disables it), and the two highest image
  units (read and written by the pass; a unit that was empty is not rebound, because ReShade's hook of `glBindImageTexture`
  would raise a GL error for texture 0 on 4.5 contexts, and our textures unbind themselves when they are deleted at the
  end of the watch). The stats clear is an upload of the used rows (ReShade's OpenGL UAV clear is a stub; its texture binding is restored by ReShade). The
  default framebuffer's read buffer, which ReShade's blit sets to `GL_BACK`, is put back when the game's differed. Pending GL
  errors are drained once at the start of the watch, before the first copy and before the first reduction, so that errors
  after them are the watch's own; the game's `glGetError` does not see those. A mid-frame default-framebuffer copy goes through
  `glBlitFramebuffer`, which an active conditional render can skip (stale slot).
- The readback fence is a GL sync object per frame, created on the presenting context and followed by ReShade's own flush (the
  exported `glFlush` would raise `execute_command_list` to every add-on), polled there (no
  worker thread or second context; `gpu_fence::OpenGLFence` joins a thread that must be retired outside ReShade's WGL locks).
  `map_texture_region` on OpenGL is a synchronous `glGetTexImage`, so it is only called once the sync object passed.
- The watch's GL objects are deleted only while the session's own context is current; otherwise (some games delete the context
  before ReShade destroys the runtime, or a helper context is current) they are leaked and left to the context teardown.
- A change of the presenting context during the watch fails it. A resize or window-mode switch that makes the game recreate its
  GL device (Wolfenstein does on a window change) fails a running watch with "The swap chain the watch measured on was reset or
  destroyed"; a new watch on the recreated back buffer works.
- With the Devkit's Device Proxy active the game renders into RenoDX's RGBA16F clone, not into the real back buffer: watch the
  back buffer with `preferClone: true`. Without it the real back buffer reports no writers (its end-of-frame numbers are still
  right). While the proxy's New Window has focus the game pauses, so its scene passes do not run.

**D3D12 and Vulkan** never change the game's pipeline, root signature, descriptor heap, descriptor set or push constant
bindings. At a measurement point inside a game command list, the region is only copied into a pooled scratch texture: the
watched subresource is transitioned from its exactly known state to copy source and back (a native per-subresource barrier
for textures with more than one subresource, since ReShade's `barrier()` transitions all of them). The lists' records are
committed at `execute_command_list`, so measurements follow submission order, not recording order; all reductions run at the
end of the frame on ReShade's immediate command list of the presenting queue, after barriers that order them behind the
game's copies. The stats readback is guarded by a fence (D3D12: signalled after flushing that immediate command list;
Vulkan: signalled at `finish_present`). Scratch textures copied into
by a command list stay reserved until that list is reset or destroyed (it may be executed again), also after the watch ends.
On D3D12 that is not enough: D3D12 lets a game reset a command list while the GPU still runs it (Vulkan does not), so
every execution of a list that copies into a scratch texture is recorded as a submission on its queue, and the texture is
reused or destroyed only after the GPU passed a lifetime fence signalled on that queue (a per-queue fence, created only at
frame ends on the presenting thread; on D3D12 `utils::gpu_fence` signals it with the native
`ID3D12CommandQueue::Signal`, while ReShade's `command_queue::signal` would also flush that queue's immediate command list
without the queue's lock). A list reset without
any execution frees its textures at once. A submission is covered by the fence value queued at the first frame end after
its `execute_command_list` event: at present, before the real `Present`, the watch signals each queue that executed
region copies since the last frame end, once (a signal is a queue command, so it lands after the lists already submitted
to that queue). No timing is involved. **Stated assumption:** ReShade raises `execute_command_list` under the queue's
lock, releases the lock and only then calls the real `ExecuteCommandLists`, with no event after the real submission; a
submission whose event came before a frame end is assumed to be submitted by that frame end. The only gap is another
thread caught between ReShade releasing the lock and its real `ExecuteCommandLists` at that exact moment. When the game
destroys a queue (ReShade raises `destroy_command_queue` before releasing the native queue), its submissions get one more
signal and are waited for there; a queue destroyed before it got a fence, or whose wait fails, keeps its textures until
the device is destroyed (logged).
`scratchBudgetMB` caps their total size: a full-resolution 4K FP16 region needs 66 MB per measurement, so use a region or
`shaderHash` on busy buffers; measurements beyond the budget are reported as `scratch_budget`. The budget becomes a
texture count, budget / (region width x height x bytes per pixel), capped at 4096 and reported as `scratchEntriesMax`
(512 MB for a 512x512 `r10g10b10a2` region is 512 textures of 1 MB). The hooks never create
scratch textures: creating an image inside the game's own command recording calls the driver and the game's allocation
callbacks from a context the game does not expect (it deadlocked DOOM 2016's allocator). A pool of up to 16 is created when
the watch starts. Each frame end (on the presenting thread) counts the textures that frame asked for, found or not, and
grows the pool so the next frame starts with that many free, within the budget; a texture still held (D3D12: until the
GPU passed the lifetime fence queued at the frame end after its execution) does not count as free, so on D3D12 the pool
settles near that many frames' worth.
Closing frames grow it only by what the hooks were short of. The warm-up frame sizes the pool for the first measured
frame, so a 1-frame watch is usually fully sized but not always: lists already recording when the watch started are
untracked and reserve nothing in the warm-up frame. A hook that finds no free texture reports that writer as
`scratch_pool_empty`. `scratchEntriesNeeded` is the largest pool a frame end wanted; above `scratchEntriesMax` the summary
says the budget was too small (use a region or `shaderHash`, or raise `scratchBudgetMB`). Each frame end that creates
textures logs how many (`created N of M wanted scratch texture(s)`), since creating many at once can cause a hitch. The
pool belongs to one watch: it is destroyed when the watch ends (textures still owned by a game command list or not yet
passed by the GPU are destroyed once they are), and the next watch starts again from 16.

The watch's lock only covers bookkeeping. On D3D12/Vulkan no GPU work (barriers, copies, reductions, fence signals, maps),
no object creation or destruction and no wait runs under it, because a driver call can call back into the game (for
example its allocation callbacks), and the game may then wait for one of its own threads that is waiting for that lock (a
deadlock found in DOOM 2016). Hooks reserve a measurement under the lock and record it after unlocking; the frame end
plans under the lock, does the GPU work unlocked, and merges the results under the lock. A finished watch is detached
first; its scratch textures still owned by a command list (or, on D3D12, still written by the GPU) stay alive until that
is over. D3D11 still measures under the lock on the immediate context: its hooks only run on that one thread (deferred
contexts are skipped before the lock). Any wait for the lock longer than 2 s is logged in `ReShade.log` with the owner's
thread and step (`devkit::watch_region(lock wait over 2 s: ...)`).

Lookups are allowed under the lock. The ones in use take only leaf locks, always after the watcher lock: RenoDX's
`pipeline_infos` and `pipeline_layout_data` maps (shared), the descriptor `DeviceData::mutex` (shared, dispatch table
scan; order: pipeline layout map, then descriptor mutex, then ReShade's view map), and ReShade's D3D12
`device_impl::_resource_mutex` (shared, view lookups). Checked in the source: the exclusive sections of all of them only
edit maps, raise no ReShade event and never call back into the Devkit, and the Devkit never takes the watcher lock inside
them. **Stated assumption (from the Vulkan spec, not checked in source):** on Vulkan the view lookups call the driver's
`vkGetPrivateData`, which has no allocator parameter and cannot call back into the game or the Devkit. A new lookup under
the watcher lock is allowed only if it takes no lock, or only a leaf lock that never raises a ReShade event or calls back
into the Devkit. Check this before adding one. The progress of the watch's fences is also read under the lock: each
fence's completion counter has its own mutex, which only the fence's completion callback takes besides the watch.

Frame ends of different runtimes can still run at once, and each one fences the D3D12 queues that executed region
copies (see below). A queue whose fence a frame end is still signalling is skipped by the others, so one thread at a time
signals a queue's fence and its values are signalled in order; its submissions get the next frame end's value.

**Fences (`utils::gpu_fence`).** The D3D12 lifetime fences (one per game queue) and the D3D12/Vulkan stats readback
fence are `gpu_fence::Fence`s, and every signal carries a completion callback. gpu_fence numbers the values of a fence 1,
2, 3, ... (a failed signal uses none) and delivers the callbacks in order on a worker thread, so a callback only counts:
the n-th callback means value n completed. Frame ends read that count instead of querying the GPU, and every CPU wait for
a fence is a condition-variable wait on it. Three rules keep the count right, checked in the code: every signal carries
a callback; a fence is signalled on one queue only (gpu_fence orders every signal after the previous one with a GPU wait, so a
second queue would be made to wait on the first); at most one thread signals a fence at a time (asserted). gpu_fence picks the value when it signals, so a frame end
marks the submissions a signal will cover under the lock, signals without it, and attaches the value under the lock
again; a failed signal leaves them for the next frame end. While a signal's value has not completed, gpu_fence's worker
checks the fence every millisecond (one worker per fence: the stats fence's ends with the watch, a queue's sleeps when
nothing is pending). A Vulkan device without timeline semaphores has no fence: the readback then waits for the queue to
go idle.

Every CPU wait for the GPU in the watch has a timeout (2 s: lifetime fences at the end of a watch, at queue and device
destruction, the stats readback fence, and the waits of a destroyed queue and of device destruction for a frame end's
signal in flight), and none holds the lock. The one exception is ReShade's `command_queue::wait_idle` (end of a watch), which has no timeout parameter; a
GPU hang ends it through the driver's device removal. **Device removal:** `gpu_fence` passes the backend's completion
value through, including `UINT64_MAX` after a device removal (TDR), and no longer ends the process (upstream `6d563066`
changed this). Pending callbacks then run as if completed, so the watch's bounded waits return at once and the game
keeps running. Expected, not yet tested: the watch then fails with "The stats readback fence could not be signalled."
or "Failed to map the stats readback texture."

The state of the watched subresource is only taken where it is exactly known; everywhere else the writer is reported as
unmeasured and its "before" comes from the last measurement point in submission order, if no other writer came in between:

| Point | D3D12 | Vulkan |
|---|---|---|
| Draw with the texture as render target (outside a render pass) | RENDER_TARGET | (draws are always inside a render pass) |
| Render pass begin with the texture as attachment | RENDER_TARGET | not measured (`state_unknown`): the event is also raised for NextSubpass and inherited secondary command buffers, and the initial layout is not exposed |
| Draws inside a render pass | members of the render pass writer | members of the render pass writer |
| Render target clear | RENDER_TARGET | COLOR_ATTACHMENT_OPTIMAL for single-subresource images (ReShade's own transition around the event); `inside_render_pass` for vkCmdClearAttachments |
| Dispatch with the texture as a UAV bound to a slot (root/push descriptor) / UAV clear | UNORDERED_ACCESS | GENERAL |
| Dispatch that reaches the texture only through a bound descriptor table | not measured (`descriptor_table`) | not measured (`descriptor_table`) |
| Copy or resolve destination | not measured (implicit promotion from COMMON) | not measured (GENERAL or TRANSFER_DST) |
| The game's barrier on the texture | not used (no subresource, enhanced barriers report access) | the new layout, for single-subresource images and layouts ReShade maps one-to-one (not `general`) |
| End of frame | PRESENT, only for the presented back buffer | PRESENT_SRC, only for the presented back buffer |

**Swap chains (D3D12/Vulkan).** Each image of a swap chain is a separate texture, and a game may render each frame into any
of them, in any order (DOOM 2016 on Vulkan does not cycle them round-robin). A watch on one of them therefore follows the
swap chain: every image of the presenting swap chain counts as the watched texture, so every frame is measured
(`swapChainImages` in the output, and a line in the summary). Before and after are always taken from the same image; a writer
whose neighbouring measurement is on another image reports that side as `no_later_measurement` (or with its own reason)
instead of comparing two images.

For any other texture, the last writer of a frame is closed by the next exact point, usually in the next frame; the watch runs
up to 8 extra frames for that (longer for cross-queue points, below) and reports `no_later_measurement` otherwise. The first frame of a D3D12/Vulkan watch is a
warm-up: command lists that were already recording when the watch started have an unknown render pass state and are reported
as `untracked_command_list` until they are reset.

**Cross-queue points (D3D12).** A game may render on a direct queue other than the presenting one (Gears of War: E-Day,
Unreal Engine 5 with Streamline). Those lists' copies run on the game's queue, the reductions on the presenting queue, so
such a point is reduced only once the GPU finished the copy: each frame end checks on the CPU whether the game queue's
lifetime fence passed the point's submission (at the frame end after the one that queued its fence value, at the
earliest, see above). The presenting queue is never made to wait on the GPU, so a fence that never arrives (queue
destroyed, device lost) cannot freeze present. A point still not ready when the watch ends is reported as
`cross_queue_not_ready`; while such points are pending, closing goes on past 8 frames until they are ready or 1 s has passed. The scratch texture stays
in COPY_DEST from its creation through the copy on the game's queue (only the watched image is transitioned there), and the
reduction on the presenting queue transitions it COPY_DEST -> COPY_SOURCE and back. It is reused or destroyed only after
both the game queue's fence and the presenting queue's fence (covering the reduction) passed. These writers are marked
`crossQueue: true` in `trace`, counted in `writers[].crossQueueExecutions` and in the summary. **Stated assumption:**
measurements from different queues are ordered by CPU submission order (`execute_command_list` order). That is correct
only if the game synchronizes its queues; a game that doesn't would already be racing.

Refused or limited cases on D3D12/Vulkan, all stated in the output:

- Command lists executed on a queue other than the presenting one: on Vulkan, and D3D12 compute and copy (non-direct)
  lists: `other_queue`.
- A command list executed again without being reset (or a Vulkan secondary command buffer inserted again): `resubmitted`;
  when this happens twice in the same frame, the first execution's measurements are invalidated too.
- D3D12 bundles cannot contain copies or barriers: a bundle executed while the texture is a render target is a `bundle`
  writer. Vulkan secondary command buffers are inserted into the primary at `execute_secondary_command_list`; their render pass
  draws are merged into the primary's render pass.
- D3D12 textures with `D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS` and Vulkan images without `TRANSFER_SRC` usage are
  refused. Device-level texture updates are ignored.
- Many Vulkan games create their render targets without `TRANSFER_SRC` (in DOOM 2016 only the swap chain and RenoDX's clones
  have it). The opt-in Devkit setting **Watch Region: Copyable Vulkan Images** (Settings > Other, ReShade.ini key
  `WatchRegionCopySourceUsage` in `[renodx-dev]`, off by default) adds `copy_source` usage to every image the game creates
  from then on, in the Devkit's `create_resource` handler. It only affects images created after it is enabled: turn it on and
  restart the game. It follows the rules of the Vulkan clone targets in `utils/resource_upgrade.hpp` (`OnCreateResource`):
  shared and sparse images and formats without copy support are left alone. Interaction with RenoDX mods: ReShade passes the
  same `desc` to every add-on's `create_resource` handler, and resource upgrades only OR in their own usage bits (in-place
  upgrades) or start from the created image's desc (clones), so the flag is kept whatever the add-on order. One exception:
  when the mod's handler runs before the Devkit's, the fallback desc a resource upgrade records for the original format does
  not have the flag, so fallback copies the mod creates later cannot be watched. The flag can disable some driver
  compression of those images; compare frame times with the setting on and off.
- The measurement copies and barriers cost GPU time and VRAM; use a region or `shaderHash` for long watches.

Limits on every API: 2D, non-MSAA textures in 8-bit, 10-bit, R11G11B10, 16-bit or 32-bit float formats; up to 1023 measured
writers per frame (D3D9: the atlas slots; `truncatedFrames` reports frames that hit it or the scratch budget).

- Only commands the game issues are hooked. Passes that add-ons draw through the ReShade API (for example a mod's swap chain
  proxy pass at present) are not reported as writers; their effect lands in the end-of-frame measurement. When a mod
  redirects the game's rendering into clones, watch the clone (`preferClone: true`).
- Draws that write the texture through a pixel-shader UAV are not detected.
- If the watched texture is destroyed during the watch, or the swap chain the watch measures on is resized, switched to
  or from fullscreen or closed, the watch stops and returns an error.
- When another add-on performs a write itself inside the same ReShade event as a game writer (for example a mod redirecting a
  clear onto its clone), its handler may run before or after the Devkit's, so that write lands on either side of the
  measurement.
- `devkit_analyze_resource` reads by default inside the Devkit's own `present` handler, so on a back buffer it can miss
  what add-ons loaded after the Devkit draw at present. `measurePoint: "frame_end"` reads in `reshade_present` instead (see
  "Measure point" in section 10); it is refused on Vulkan for now.
- Cross-queue points are reduced at the second frame end after their execution at the earliest (later when the GPU
  runs behind), so they hold scratch textures longer (on busy targets the pool can reach the budget: use a region or
  `shaderHash`), and a 1-frame watch on such a game closes a few frames later.
- On targets with hundreds of writers per frame (for example an Unreal Engine 5 G-buffer), a 1-frame watch can come up
  empty: Unreal records the next frame while the pool grows. Use `shaderHash` to watch only the writers you care about
  (keeps the per-command trace), or `frames` >= 2 (aggregated writers only, no trace); both only reduce the loss. In
  Gears of War: E-Day's G-buffer (cross-queue), `shaderHash` on one draw shader measured 5 of its 72 draws in a 1-frame
  watch and 145 of 288 over 4 frames, the rest `scratch_pool_empty`. Growing the pool by about 1400 textures at one
  frame end took about 450 ms there (a one-time hitch, logged).
- A command list the game executes again on its own queue without resetting it, after one of its cross-queue points was
  reduced, can overwrite that point's copy while the presenting queue still reads it (wrong statistics for that point; the
  overlapping use is undefined in D3D12, not seen in testing). Engines that reset their lists every frame (Unreal Engine) do not do this.
- On Vulkan, a watch on a RenoDX clone (`preferClone: true`) sees no writers. When the mod redirects the final pass into its
  clone, a watch on the original swap-chain image shows that pass as a writer with "0 changed" (the mod writes the image
  outside the hooked commands).
- On D3D11, `framesMeasured` counts frames with at least one measurement: a texture no hooked command writes is measured
  only at the end of the last frame, so it reports 1.
- With DLSS Frame Generation loaded (Streamline), the game renders into Streamline's own back buffers
  (`nv.sl.dlss_g.tex2d.fake-swapchain-buffer`), not the swap-chain images; watch those, not the swap chain.
- Follow-up: the hooks still do their lookups (render targets, view descriptions) under the watcher lock; doing them before
  taking it would remove the lookup rule above.
- Known issue, not caused by the watch: the game process can hang on exit with the Devkit loaded and has to be ended from
  Task Manager; this predates the MCP work. Seen in Gears of War: E-Day (D3D12) and on every exit of Sleeping Dogs:
  Definitive Edition (D3D11); two DOOM 2016 (Vulkan) exits after watches did not hang. Cause not yet investigated.

## 11. Current practical caveats

- Some clone and preview workflows may still behave better when the devkit overlay has already touched the resource in the current session. Treat that as a devkit limitation, not as a mod requirement.
- If a game stalls during live shader reload, rebuild the latest devkit. Live shader load and unload are designed to be queued and processed on present, not on the MCP thread.
- Shader decompilation is not guaranteed for every tracked DirectX shader. If `devkit_get_shader` reports the decompilation section as unavailable, use disassembly or a dumped shader with external tools instead.
- If an MCP client loses its transport, that does not necessarily mean the game-side devkit backend is gone. Reconnect the bridge and check `devkit_status`.

## 12. What to turn into a real mod

Devkit MCP is for narrowing the problem and proving a path. The shipping mod should then move the proven behavior into `src/games/{gamename}`:

- `addon.cpp` for resource upgrades and shader registration
- CRC-addressed shader replacements
- manual verification notes

Do not ship a mod that still depends on manual devkit clone toggles or the overlay being open.

The usual progression is:

1. prove the target pass with MCP
2. prove HDR survival with clone readback
3. convert the successful path into addon logic
4. validate the real non-devkit path

## 13. Client configuration examples

These examples assume you built the local debug bridge at:

- `C:\Mods\renodx\build\Debug\renodx-mcp-bridge.exe`

### Codex

Add this to `.codex/config.toml`:

```toml
[mcp_servers.renodx]
command = "C:\\Mods\\renodx\\build\\Debug\\renodx-mcp-bridge.exe"
args = []
```

Typical first prompt:

```text
Use the RenoDX MCP bridge. List the available DevKit connections, connect if needed, queue a snapshot on the active device, then show me the last 10 draws and the first 10 tracked shaders.
```

### Claude Desktop

Add this to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "renodx": {
      "type": "stdio",
      "command": "C:\\Mods\\renodx\\build\\Debug\\renodx-mcp-bridge.exe",
      "args": []
    }
  }
}
```

### VS Code / Copilot Chat

Add this to `.vscode/mcp.json` in your workspace:

```json
{
  "servers": {
    "renodx": {
      "command": "C:\\Mods\\renodx\\build\\Debug\\renodx-mcp-bridge.exe",
      "args": []
    }
  }
}
```

Notes:

- VS Code can also keep the same MCP server config in user `settings.json` instead of a workspace `.vscode/mcp.json`.
- GitHub-hosted Copilot coding agent is not a good fit for this bridge. `renodx-mcp-bridge.exe` is a local Windows process that expects a running game and DevKit addon on the same machine.
