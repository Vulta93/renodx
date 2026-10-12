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
- `devkit_get_draw`
- `devkit_get_shader`

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

### Watching a region over time (`devkit_watch_region`, D3D11, D3D12, Vulkan)

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
`scratchBudgetMB` (D3D12/Vulkan, default 512, see below). The call blocks until done.

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
  frame), `scratch_budget`, `scratch_pool_empty`, `state_unknown`, `inside_render_pass`, `other_queue`, `resubmitted`,
  `untracked_command_list`, `bundle`, `no_later_measurement`, `descriptor_table` (a D3D12/Vulkan dispatch that can reach
  the texture only through a bound descriptor table: bindless engines keep most textures in one table, so this does not
  prove a write and gives no known state; no barrier or copy is issued for it), `cross_queue_not_ready` (D3D12: see
  cross-queue points below).
- `endOfWatch` holds the region stats measured at the end of the last frame (D3D11: always; D3D12/Vulkan: only for the
  presented back buffer). It includes every write made during present, including passes other add-ons draw there.
- Statistics are luminance (BT.709 weights on raw values, or nits for `pq`/`scrgb`); the GPU path computes no percentiles.

**D3D11** measures in place on the immediate context: the region is copied into a scratch texture and reduced right away. The
compute slots the pass uses (CS shader, b0, t0, u0) are saved and restored with native D3D11 calls. Deferred contexts are skipped.

**D3D12 and Vulkan** never change the game's pipeline, root signature, descriptor heap, descriptor set or push constant
bindings. At a measurement point inside a game command list, the region is only copied into a pooled scratch texture: the
watched subresource is transitioned from its exactly known state to copy source and back (a native per-subresource barrier
for textures with more than one subresource, since ReShade's `barrier()` transitions all of them). The lists' records are
committed at `execute_command_list`, so measurements follow submission order, not recording order; all reductions run at the
end of the frame on ReShade's immediate command list of the presenting queue, after barriers that order them behind the
game's copies. The stats readback is guarded by a fence (Vulkan: signalled at `finish_present`). Scratch textures copied into
by a command list stay reserved until that list is reset or destroyed (it may be executed again), also after the watch ends.
On D3D12 that is not enough: D3D12 lets a game reset a command list while the GPU still runs it (Vulkan does not), so
every execution of a list that copies into a scratch texture is recorded as a submission on its queue, and the texture is
reused or destroyed only after the GPU passed a lifetime fence signalled on that queue (a per-queue fence, created only at
frame ends on the presenting thread, signalled with the native `ID3D12CommandQueue::Signal`: ReShade's
`command_queue::signal` also flushes that queue's immediate command list without the queue's lock). A list reset without
any execution frees its textures at once. **Stated assumption:** ReShade raises `execute_command_list` before the real
`ExecuteCommandLists` and outside any lock an add-on can take, with no event after the real submission, so a submission
gets its fence value only after two frame ends and at least 100 ms; the game's submit call is assumed to have returned by
then (100 ms covers the submit thread being paused by the OS). When the game destroys a queue (ReShade raises
`destroy_command_queue` before releasing the native queue), its submissions get one more signal and are waited for there;
a queue destroyed before it got a fence, or whose wait fails, keeps its textures until the device is destroyed (logged).
`scratchBudgetMB` caps their total size: a full-resolution 4K FP16 region needs 66 MB per measurement, so use a region or
`shaderHash` on busy buffers; measurements beyond the budget are reported as `scratch_budget`. The budget becomes a
texture count, budget / (region width x height x bytes per pixel), capped at 4096 and reported as `scratchEntriesMax`
(512 MB for a 512x512 `r10g10b10a2` region is 512 textures of 1 MB). The hooks never create
scratch textures: creating an image inside the game's own command recording calls the driver and the game's allocation
callbacks from a context the game does not expect (it deadlocked DOOM 2016's allocator). A pool of up to 16 is created when
the watch starts, and the frame end (on the presenting thread) grows it by however many the hooks were short of, within the
budget; a hook that finds no free texture reports that writer as `scratch_pool_empty`, so the first busy frames of a watch
can show some of those.

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
into the Devkit. Check this before adding one.

Every CPU wait for the GPU in the watch has a timeout (2 s: lifetime fences at the end of a watch, at queue and device
destruction, the stats readback fence, and a destroyed queue's wait for a frame end's signal in flight), and none holds
the lock. The one exception is ReShade's `command_queue::wait_idle` (end of a watch), which has no timeout parameter; a
GPU hang ends it through the driver's device removal.

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
lifetime fence passed the point's submission (two frame ends and 100 ms after its execute event at the earliest, see
above). The presenting queue is never made to wait on the GPU, so a fence that never arrives (queue destroyed, device
lost) cannot freeze present. A point still not ready when the watch ends is reported as `cross_queue_not_ready`; while
such points are pending, closing goes on past 8 frames until they are ready or 1 s has passed. The scratch texture stays
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
writers per frame (`truncatedFrames` reports frames that hit it or the scratch budget).

- Only commands the game issues are hooked. Passes that add-ons draw through the ReShade API (for example a mod's swap chain
  proxy pass at present) are not reported as writers; their effect lands in the end-of-frame measurement. When a mod
  redirects the game's rendering into clones, watch the clone (`preferClone: true`).
- Draws that write the texture through a pixel-shader UAV are not detected.
- If the watched texture is destroyed during the watch (for example on resize), the watch stops and returns an error.
- When another add-on performs a write itself inside the same ReShade event as a game writer (for example a mod redirecting a
  clear onto its clone), its handler may run before or after the Devkit's, so that write lands on either side of the
  measurement.
- Follow-up: `devkit_analyze_resource` still reads the texture inside the Devkit's own `present` handler, so on a back buffer
  it can miss what add-ons loaded after the Devkit draw at present (the ordering problem fixed here for the watch).
- Cross-queue points are reduced at least two frame ends and 100 ms after their execution, so they need more scratch
  textures (`scratch_pool_empty` on busy targets: use a region or `shaderHash`), and a 1-frame watch on such a game
  takes about a second to close.
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
- `verify` cannot decode `r16g16b16a16_unorm` or `r32g32b32a32_float` on the CPU yet and reports `skipped` for them, so
  watches on such targets are unverified (for example Gears of War: E-Day's `r16g16b16a16_unorm` G-buffer).
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
