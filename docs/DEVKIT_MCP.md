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

### Watching a region over time (`devkit_watch_region`, D3D11)

`devkit_watch_region` answers "which draw changes this region of this buffer, and how?" without a snapshot or screenshots.
The state of a texture after command N equals its state before the next command that writes it, so the devkit measures the
region on the GPU before every command that writes the texture and once more at present. Writers are draws with the texture
bound as a render target, dispatches with it bound as a compute UAV, and copies, render target and UAV clears, resolves and
`UpdateSubresource` calls that target it. Each measurement copies the region into a scratch texture and reduces it with a small compute shader
(`watch_region_reduce.cs_5_x.hlsl`) into a stats texture that is read back two frames later.

Arguments: `resourceViewHandle` and `preferClone` (as for `devkit_analyze_resource`; the backing resource or its clone is
watched), the optional region (`regionX`/`regionY`/`regionWidth`/`regionHeight`, default: whole texture), `signal`
(`raw`/`pq`/`scrgb`), `frames` (default 1, max 3600), `shaderHash` (measure only writers using that shader) and
`includeUnchanged` (also list writers that did not change the region, `frames: 1` only). The call blocks until done.

- `frames: 1` returns `trace`: every writer that changed the region, in command order, with `commandIndex` (ordinal among the
  frame's draws, dispatches and copies; offset from `devkit_list_draws` indexes, so match by shader hash and order), shader
  hashes and region stats before and after (`maxLuminance`, `minLuminance`, `meanLuminance`, `maxChannel`, pixel counts above 1.0,
  negative, non-finite, black, and above 203/400/1000/4000 nits with a signal).
- Every call returns `writers`, aggregated per writer (kind + pixel/vertex/compute hash): executions, frames seen, how often the
  region changed, min/mean/max of the region peak and mean before and after, how often the peak was lowered or raised, how often
  values went from above 1.0 to at most 1.0, and `afterPeakMode` (the most repeated after-peak, a ceiling candidate).
- Statistics are luminance (BT.709 weights on raw values, or nits for `pq`/`scrgb`); the GPU path computes no percentiles.

Limits: D3D11 immediate context only (deferred contexts are skipped); 2D, non-MSAA textures in 8-bit, 10-bit, R11G11B10, 16-bit
or 32-bit float formats; up to 1023 measured writers per frame (`truncatedFrames` reports frames that hit it). Each measured
writer costs a region copy and a small dispatch, so use a region or `shaderHash` for long watches of busy buffers.

- Only commands the game issues are hooked. Passes that add-ons draw through the ReShade API (for example a mod's swap chain
  proxy pass at present) are invisible; their effect shows up only if it lands before the present-time measurement. When a mod
  redirects the game's rendering into clones, watch the clone (`preferClone: true`).
- Draws that write the texture through a pixel-shader UAV are not detected.
- If the watched texture is destroyed during the watch (for example on resize), the watch stops and returns an error.

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
