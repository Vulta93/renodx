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

A Devkit pipe is named after its game's process id (`renodx-devkit-mcp-<pid>`), so a restarted game comes back under a new
name. Before each tool call the bridge drops a selected Devkit pipe that is no longer listed, even one chosen with
`renodx_connect`, and uses the only listed pipe instead; with none or several listed, the call fails with that reason. A
pipe chosen with `renodx_connect` that is still listed stays selected while other games start. Pipe names without the
`renodx-devkit-mcp` prefix (custom `--pipe` names) are never listed and never dropped.

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
- `devkit_frame_overview` (passes, written textures with their readers, and tonemap/LUT/UI/output candidates, see section 6)

`devkit_queue_snapshot`, `devkit_analyze_resource` and `devkit_watch_region` take `delaySeconds` (0-30): the tool waits that
long before it queues the request. Use it when the game is paused while you talk to the agent: start the call, unpause,
and the capture lands on gameplay instead of the pause menu. The wait happens before queueing, so it does not count
against the tools' own 15 s wait for the next present. It runs on the MCP connection's thread: other Devkit tool calls wait
until it ends, and the arguments other than `delaySeconds` and `waitForKey` are only checked after it.

The same three tools take `waitForKey` (`"F1"` to `"F12"`): the call waits, at most 60 s, until the person presses that key
in the game, then applies `delaySeconds` if given, then captures. The person decides the moment, for example after
walking to a spot, without the agent guessing a delay. The key is read in `reshade_present` through ReShade's input
(`effect_runtime::is_key_pressed`), so the game window needs focus (ReShade very likely still sees the key with its overlay
open, while the game does not; not checked in a game). If the key is not pressed in time, the call fails and nothing is captured. The game still receives the
key, so pick one it does not use. Like the delay, the wait blocks other Devkit tool calls and a Devkit unload until it
ends.

`devkit_queue_snapshot` with `waitForCompletion: true` returns only when the captured frame and its end-of-frame processing
are finished (at most 120 s), with that device's snapshot summary, so no `devkit_snapshot_summary` polling is needed. It waits
for the device's completed-snapshot id to change, so a capture that was already running when the call came also ends it.
It fails when the device is destroyed or replaced meanwhile (its index then names another device), and at the limit: the
request stays queued when the game presented no frame (paused in the background, minimized), a running capture keeps
capturing, and with neither pending (a request dropped without a capture) it says to queue a new one. Like the delay, it
blocks other Devkit tool calls (worst case 60 s key + 30 s delay + 120 s).

### Resource inspection

- `devkit_analyze_resource`
- `devkit_watch_region`
- `devkit_dump_resource_with_hash`
- `devkit_analyze_lut`
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
- `devkit_set_shader_source`
- `devkit_set_draw_bypass`
- `devkit_write_shader_stub`

`devkit_set_shader_source {shaderHash, source}` is the overlay's per-shader Original/Add-on/File choice over MCP:
`original` draws the game's shader, `addon` the game mod's replacement (refused when no add-on replaces that hash or the
Devkit has not synced the add-on's replacements on that device yet), `file` the compiled live shader file. It runs on the
present thread and takes effect at the shader's next bind. Use it to find which replaced shader causes an artifact, then
switch back to `addon`. Only that hash changes: the mod's resource upgrades, clones and swap-chain proxy stay, so
`original` everywhere is not a vanilla view (the mod's Off preset is). With the Devkit loaded every mod replacement is a
bind-time one (the Devkit sets `use_replace_async`), which is why the switch can undo a mod's shader at all. On D3D12 and
Vulkan the present queue is waited idle before the switch, because it destroys the replacement pipeline at once. That
does not cover other queues (async compute, a separate present queue) or command lists already recorded for the next
frame, so a device loss is still possible there; the overlay's combo has the same risk.

`devkit_set_draw_bypass {shaderHash, bypass}` sets the flag behind the overlay's per-shader Draw button: while it is true,
draws, indexed draws, dispatches and indirect draws/dispatches with that shader bound in any stage are skipped (mesh and
ray dispatches are not), in every pass the shader draws in (check `sharedShaders` in `devkit_frame_overview`). It is set
on the present thread, so the game must be presenting (the call times out after 15 s otherwise), and it acts on command
lists recorded afterwards: D3D12 bundles and command lists recorded once and executed again keep their draws. It sets the
value rather than toggling it, so repeated calls and the overlay button stay consistent; the result lists every bypassed
shader. Use it to remove a draw and see what disappears (bloom, fog, a UI layer). In `devkit_watch_region` a bypassed draw
still appears as a writer, with no change to the region.

`devkit_write_shader_stub {shaderHash, outputDirectory?}` writes a shader's decompilation (the text `devkit_get_shader`
shows) to `0x<HASH>.<stage>_<major>_<minor>.hlsl`, the name the live-shader loader parses, so a replacement starts from
the shader's own signature and constant buffer layout instead of a hand-written one. It covers DXBC shader model 4 and 5
(D3D10/11, D3D12 SM5; SM 5.1 stubs are untested and may lose register spaces), reading the shader model from the
bytecode, and refuses when the folder already holds a file for that hash. The default folder is the dump folder, outside
the default live folder (refused if the live path was set to a folder containing it). The edit loop: write the stub,
check it against the original (copy it into the live folder, `devkit_load_live_shaders`, then `devkit_analyze_resource`
on a small region of the target the shader writes, with `abShaderHash`, `abSourceA: original`, `abSourceB: file`, after
an A-against-A run for the noise floor: overlapping ranges mean no measurable difference in the region's statistics, not
a pixel-exact match), then replace a constant such as `cb0[3].x` with a number, reload and compare again. That answers
"what does this constant do" without a constant override tool.

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

`serverInfo.modulePath` and `serverInfo.moduleLastWriteUtc` name the Devkit file the game loaded and its last write time
(copying keeps the build's time), so they tell which build is installed. With the repository's CMake presets `version`
stays `unknown` and `buildTimestampUtc` is absent, because they do not inject the `build_info` values.

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
  are cut and flagged with `valuesTruncated`), `firstValue` being the word index of `floatValues[0]`. They do not need
  Trace Constant Buffers.
  - D3D12 root constants and Vulkan push constants, only for the root signature / pipeline layout bound for the draw's bind
    point (the one its descriptor tables are bound with). When no descriptor table was bound since the last root signature
    change, that layout is unknown and the values of every layout pushed during the snapshot are listed (check
    `layoutHandle`).
  - D3D9 shader constants: `layoutParam` 5/6/7 are the pixel shader's float/int/bool constants, 2 the vertex shader's
    float constants. Float and int register N start at word 4 * N; bool register N is word N. The float constants
    (`layoutParam` 5 and 2, `valuesSource: "device"`) are read from the device at the draw (`GetPixelShaderConstantF`,
    `GetVertexShaderConstantF`) rather than collected from pushes: from c0 up to the end of the shader's constant table,
    or without one the shader model's float registers (ps_1_x 8, ps_2_x 32, ps_3_0 224, vs_1_1 96, vs_2_x/vs_3_0 256),
    or further when the game pushed higher registers during the snapshot, at most 256 registers. Every word is then
    known (no `unwrittenRanges`), whenever and by whom it was set: constants set in an earlier frame, by a state block,
    or by an add-on that injects its own constants (RenoDX shader injection) all show. A ps_1_x shader clamps its
    constants to [-1, 1] when it reads them; the device holds, and the Devkit reports, the unclamped values. The int and
    bool registers (`layoutParam` 6/7) stay collected from pushes. On a pure device (`D3DCREATE_PUREDEVICE`), whose
    `Get*` calls are not reliable, the float constants are collected from pushes too, pixel shader only. `layoutHandle` is
    ReShade's one D3D9 layout, or 0 when the game pushed no constant during the snapshot.
  - OpenGL `glUniform*`: word 4 * location. Values are kept per program (the one current at each call) and per context,
    and a draw lists only its own program's (`glProgram`), since a location means something different in every program;
    a uniform set on another context of the share group is not seen. Arrays and matrices fill consecutive words from
    4 * location, overlapping the next locations. Values left from a deleted program whose name the driver reused show
    up under the new program. `glProgramUniform*` is not hooked by ReShade, so those writes are missing.

Limits: values other than the D3D9 float constants are recorded only from calls the game makes during snapshot frames
(like the other bindings the snapshot records). A constant the game set before the snapshot frame and did not set again
is missing, or shows a value from an earlier snapshot on D3D9 (int and bool registers) and the D3D11/OpenGL immediate
context, whose state is kept between frames. Vertex shader constants are not included, except the D3D9 float ones and on
OpenGL, where uniforms belong to the whole linked program.

`valueNames` labels the words with the active pixel (or compute) shader's own variable names, read from the reflection
in its disassembly (on OpenGL, the linked program's active uniforms, all stages): each entry is `name`, `index` (into
`floatValues`/`uintValues`) and `count` (words).
- D3D10/11 constant buffers and D3D12 root constants (DXBC `//` or DXIL `;` disassembly): the "Buffer Definitions"
  block (cbuffer members and their byte offsets, struct members as `struct.member`) joined to the register and space
  through "Resource Bindings"; DXIL's wrapper struct named after the cbuffer adds no prefix (a DXBC struct member with
  the cbuffer's name keeps it). Offsets are relative to the bound offset. A D3D12 root constant parameter is
  matched through the root signature's register and space, and its word is the 32-bit value offset; D3D12 root and
  table CBVs have no values, so no names. Members of nested structs (and every DXIL member) have no size in the
  disassembly; their size is derived from the type (column_major packing unless the line says `row_major`; array
  elements start on 16-byte boundaries; `double` and 64-bit integers take 8 bytes, 16-bit types are counted as
  4). Arrays of structs are labelled at element 0 only. `$Globals` is included; structured buffer layouts are not.
  Shipped D3D12 shaders often strip their reflection (`-Qstrip_reflect`), and then the list is empty.
- D3D9 shader constants (`layoutParam` 5/6/7, and 2 for the vertex shader's float constants, named from the vertex
  shader's own table): the constant table's "Registers" rows (`c`, `i`, `b`).
- Vulkan push constants: the SPIR-V push constant block's `OpMemberName` and `OpMemberDecorate ... Offset` (word =
  Offset / 4, as `vkCmdPushConstants` addresses it; 16-bit members share their 32-bit word). Scalars, vectors, matrices
  (`MatrixStride` per column, per row when `RowMajor`) and arrays (`ArrayStride` per element) are sized from their type;
  the next member's offset caps that size, since stride padding can overlap it, and bounds other members; the last one
  runs to the end of the captured words. Modules run through spirv-remap keep the offsets but lose the names, so the
  list is empty; it is also empty when the active pixel or compute shader declares no push constant block (a push that
  only the vertex shader reads).
- OpenGL uniforms: the draw's program (`glProgram`) is read from the context, and only the glUniform* values set
  while that program was current are listed; its active uniforms (`glGetActiveUniform`, `glGetUniformLocation`) name
  them at word 4 * location, counting components times array elements, as one whole-array call packs them. A game
  that sets an array per element puts element i at word 4 * (location + i): inside the name's span for vec4 and
  matrix arrays, past it (over the next locations' words) for float, vec2 and vec3 arrays. Unwritten words and
  uniforms without a value are listed in `unwrittenRanges` and `namesWithoutValues` (below). Names are read once per
  program per snapshot; double uniforms are not seen. Uniform block members and `glProgramUniform*` values are never
  seen. Draws through program pipelines (`glUseProgramStages`) have no current program and list only values set
  without one; a program whose last link failed gets no names.
- An empty list means the game stripped the reflection, or no named variable starts inside the captured words.

Each `pushConstants` entry also carries (all omitted when empty or false):
- `valuesTruncated`: the range was longer than the 1024-word cap; words past it are not listed and may have been set.
- `unwrittenRanges`: `index`/`count` ranges of words inside the captured span that no recorded call wrote (calls are
  recorded in snapshot frames; on D3D9 and immediate contexts earlier snapshots count too). They read 0, so a 0 there
  is not a value the game set.
- `namesWithoutValues`: named variables of the same register set with no word inside the captured span: `name`,
  `word` (absolute, in `firstValue`'s numbering, so OpenGL location L is word 4 * L) and `count` when the reflection
  sizes it. No call recorded for this entry set them (the game set them earlier, never, or through another entry), so
  the value is unknown. A variable that only partly overlaps the span is neither here nor in `valueNames` when it
  starts before `firstValue`; variables past a truncated range are left out.

`unwrittenRanges` and `namesWithoutValues` are per entry: a register set with no push recorded has no entry, so it
lists no names at all (for example an OpenGL program whose sampler units were all set at load), and when several
entries share a register set (Vulkan ranges with different stage flags, layouts listed because the bound one is
unknown) each reports the others' words and variables as missing.

Constant buffer binds in snapshot draws (`constantBuffers`, with or without values) are the pixel or compute stage's, like
SRVs and UAVs: before this change a vertex shader bind on D3D11 could overwrite the pixel shader's slot. Vulkan dynamic
uniform and storage buffers (`VK_DESCRIPTOR_TYPE_*_BUFFER_DYNAMIC`) are never listed: ReShade (API version 18) forwards no
descriptors for them (`vkUpdateDescriptorSets` hook, `vulkan_hooks_device.cpp`, `default: update.count = 0`), so the
Devkit never sees those bindings. A Vulkan draw whose shader reads constant buffers but shows none (DOOM 2016's output
draw) most likely uses them. Listing them would need a change in ReShade.

### D3D9 fixed-function stages (`fixedFunction` in `devkit_get_draw`)

A D3D9 draw the game issues without a pixel shader is shaded by the fixed-function texture stages. The snapshot reads them
from the device at each such draw (`GetTextureStageState`, `GetRenderState`), the same way it reads D3D9 blending, and
`devkit_get_draw` lists them as `fixedFunction`:

- `stages`: stage 0 onwards, up to the first whose color op is `D3DTOP_DISABLE` (at most eight). Each has `colorOp` and
  `alphaOp` (D3DTOP names in snake case: `modulate`, `modulate2x`, `select_arg1`, `blend_texture_alpha`, ...),
  `colorArg1`/`colorArg2` and `alphaArg1`/`alphaArg2` as `{source, complement?, alphaReplicate?}` (source `diffuse`,
  `current`, `texture`, `texture_factor`, `specular`, `temp` or `constant`; `complement` is D3DTA_COMPLEMENT, 1 - x;
  `alphaReplicate` is D3DTA_ALPHAREPLICATE), `colorArg0`/`alphaArg0` only for `multiply_add` and `lerp`, which use three
  operands, and `resultArg` only when the stage writes somewhere else than `current` (usually `temp`).
- `textureFactor` (D3DRS_TEXTUREFACTOR as RGBA 0-1) with `textureFactorHex` (the raw ARGB value), and `specularEnable`.
- `devicePixelShaderBound`: the device had a pixel shader bound although the game bound none. An add-on drew the draw with
  its own shader (a stand-in), which it binds without a ReShade event; the stages are still the game's, which is what the
  stand-in has to reproduce.

Why it matters for HDR: the stages compute in the hardware's fixed-point range, so `modulate2x`, `modulate4x` and `add`
stages clamp at white (1.0) before blending, and `add_signed`/`subtract` clamp at 0. A stand-in pixel shader that
reproduces the stages in float has to clamp the same way where the game relies on it, or decide on purpose not to.

The stages are read only on draws without a game pixel shader, only while a snapshot records, and not on a pure device
(created with `D3DCREATE_PUREDEVICE`), whose `Get*` state calls are not reliable.

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

It answers "is my shader live and drawing here?" without reading `ReShade.log`. Queue a snapshot first. The list is paged
(`limit`, default 100, max 5000, and `offset`, in hash order): a generic mod such as `renodx-ue-extended` registers hundreds
of replacements (418 in Gears of War: E-Day). `drawnOnly: true` keeps only the hashes the snapshot drew; `totalRegistered`,
`totalDrawn` and `totalFiltered` give the counts before paging.

### Shader coverage (`devkit_shader_coverage`)

A snapshot shows one frame; coverage answers "which of my replacements did this play session never exercise?".
`action: start` takes every shader the device has a replacement for (a game mod's, `source` `addon`; a compiled live file,
`file`; or both) and counts their draws from then on; `status` reports without stopping, `stop` ends the recording and
reports. Play through what the mod must handle (menus, loading screens, videos, fades, cutscenes, gameplay) between start
and stop. The report lists `neverDrawn` first, then `drawn` with `draws`, `framesSeen` and `firstFrame`/`lastFrame`
(presents since start), at most 64 per group; `watchedShaders` and `neverDrawnCount` are complete. A never-drawn
replacement is either a scene not visited yet or a hash the game no longer uses.

Counts cover draws, indexed draws, dispatches and indirect calls with the shader bound in any stage. On D3D12/Vulkan a
command list recorded once and executed again is counted when it is recorded. While recording, every draw looks up its
bound shaders under a shared lock and takes the exclusive lock only when a watched shader draws; nothing is counted
otherwise. `start` is refused while a recording runs; `stop` first. Replacements added after start (a live file loaded
later) are not watched until the next start. The recording lives with the device: a game that recreates its device (a
D3D9 reset path, a mode change) loses it, and the new device needs a new start. Not on a Display Proxy device.

### Frame overview (`devkit_frame_overview`)

One call that summarizes the current snapshot, so the tonemapper, LUT, UI and output passes can be found without walking
thousands of draws:

- `passes`: consecutive commands of the same kind that write the same targets with the same blend state form one pass
  (render targets for draws, UAVs for dispatches); each copy is its own pass, with its destination as target and its
  source as input. Each pass has its draw range (`firstDraw`/`lastDraw`, the indexes `devkit_get_draw` takes), `targets`,
  `blendEnabled` (draws; `null` when no blend state was recorded for the bound pipeline), `inputCount`, up to 8 shader hashes and, for
  candidates, its `inputs` and `hints`. `orderUnknown` marks D3D12/Vulkan commands without a known submission order, which
  are sorted after every ordered command (other APIs record commands in issue order and never set it).
- Textures are described by the resource the game created: `upgradedFrom` gives the game's format when a RenoDX mod
  upgraded the resource in place, `cloneFormat`/`cloneActive` the clone a mod renders into instead.
- `writtenTextures`: every texture a pass writes, latest writer first, with up to its last 8 writer passes and the first 8
  draws that read it (`readerCommandCount` counts every reading command, copies included). Follow a texture from its
  writer to its readers to trace the chain.
- `candidates`: pass indexes per hint; each hint in a pass carries the rule that matched. The order follows the skills
  (`swapchain-resource-analysis`, `handle-sdr-tonemap-lut`): scene HDR, tonemap, LUT, UI, output.
  - `tonemap`: a draw (not blended) or dispatch whose first target is large and that reads a float texture of the same
    size (RGBA16F, R11G11B10F, RGBA32F, RGB32F, RG16F, RGB9E5, as rendered, so a mod's float clone counts) and writes an
    SDR target (RGBA8/BGRA8/BGRX8, sRGB or not, RGB10A2/BGR10A2), either as rendered or as the game created it before a
    mod's upgrade or clone. A pass that fills a float swap chain from a same-size float texture is a candidate too: a mod
    that upgrades the swap chain when the game creates it (DOOM 2016's mod) leaves no SDR format to compare with, and an
    HDR-native game presents a float swap chain as well (there, several post passes into the back buffer can all
    match, and the last one anchors the UI search). Passes that write the swap chain stay candidates (vanilla games often
    tonemap straight into the back buffer, and an RGB10A2 swap chain can also be an HDR10 output), but their rule says so.
    The rule also notes when the float input is float only through a mod, i.e. the game created it in a non-float format
    and the mod cloned or upgraded it in place. A swap-chain writer with such an input is the game's own final pass (AA,
    upscale, composite) reading an intermediate the mod upgraded, often the pass a mod's output work targets. A mod's
    swap-chain proxy pass itself is never in a snapshot: passes an add-on draws through the ReShade API raise no add-on
    events.
  - `lutTextures`, `lutReaderPasses`, `lutBuilderPasses`: textures with the geometry `src/shaders/lut.hlsl` samples (a 3D
    cube of 16 to 65, or a 2D strip N*N x N with the usual sizes N = 16/32/33/48/64), the passes that read them and the
    passes that write them. Only post-processing passes count as readers: a large target that is not square (width and
    height more than 5% apart, unless the screen itself is square), because scene, shadow and probe draws into square
    targets also sample LUT-shaped noise and lighting volumes. Pillarboxed and letterboxed post chains still count. Without
    a swap chain the screen size is a guess (the largest target written), so only the size is checked there. Non-square
    shadow atlases (e.g. 4096x2048) that sample such a volume still match. Noise volumes and ramp strips of the same shape
    still match as LUT textures.
  - `uiPasses`: blended draws onto a large swap-chain target or one the game created as SDR, in known order, after the last
    tonemap candidate that is not a final pass from a mod-upgraded intermediate (UI can be drawn onto that intermediate before
    the final pass; a vanilla game's tonemap into the back buffer stays the anchor). When every candidate is such a final
    pass (e.g. the mod tonemaps in its swap-chain proxy pass, which an add-on draws through the ReShade API and a snapshot
    never records), the scan starts after the last one, so UI drawn onto the intermediate before it is not found. A UI
    candidate whose every input was written earlier in the frame (copies count) is noted as a possible composite: of
    post-processing (bloom, glow, blur), or of UI drawn into an earlier target. UI drawn from its own art (atlases, fonts
    loaded before the frame) gets no note. In Prince of Persia T2T (D3D9) the HUD pass (122 fixed-function draws, BC3 art,
    source-alpha blending) was found next to two bloom/glow composites that got this note.
  - `videoPasses`: passes that read two or more one- or two-channel 8-bit 2D textures (R8/R8G8), one of them half the
    width and height of another: the YUV 4:2:0 planes of decoded video (Bink and similar decoders). ReShade reports no YUV
    formats, so only the plane sizes are checked, and noise or mask textures of that shape match too. Video decoded into
    one NV12 texture (hardware or Media Foundation decoding, read through R8 and R8G8 views of the same resource) is not
    found, since both views have the resource's size. Check video brightness and the video's path to the swap chain from
    there. Take the snapshot while a video plays.
  - `outputPass`: the last pass in known order that writes a swap-chain back buffer, by draw, dispatch or copy.
  - `sharedShaders`: shaders (the pixel or compute stage hash each command records) of a tonemap, UI or output candidate
    that also draw in a pass of another role (`ui`, `tonemap`, `output`, `other`; a pass that is both tonemap and output
    counts as tonemap). A replacement of such a hash changes every one of those passes, as when one shader draws both UI
    and scene, so its callback has to tell them apart (for example by render target or by the bound texture). Each lists
    up to 8 passes (the first pass of each role, then the others in frame order), with `firstDrawOfShader` (that
    shader's first draw in the pass, for `devkit_get_draw`); at most 16 are listed, the one with the latest first
    candidate pass first, and `sharedShaderCount` counts them all.

Comparing two frames (`saveBaseline`, `compareWith`): `saveBaseline` (absolute path ending in `.json`; the file is
overwritten) writes every pass's
structure: its targets and inputs with the format the game created and the format rendered, their sizes and swap-chain
flag, its shader hashes, command count and hint types, and returns the path as `baselineSaved`. The file survives a
game restart, which most comparisons need (upgrades and mod changes act at creation, and resource handles change with
a restart). `compareWith` takes such a file and adds `comparison`:
- Passes are aligned in frame order (longest common subsequence) by kind, the targets as the game created them (format,
  size, swap chain; a swap chain by size alone, since a mod upgrades it when the game creates it) and blend, so a pass
  whose target a mod upgraded or cloned still aligns.
- For each aligned pass, `changes`: `targetGameFormat` (a swap chain created in another format, as after a mod's
  swap-chain upgrade), `targetRenderedFormat` (a target now upgraded or cloned, or no longer; left out when it only
  repeats `targetGameFormat`, a swap chain upgraded in place without a clone),
  `inputRenderedFormats` (added and removed input formats), `shaderHashes` (only for passes of at most 4 shaders: scene
  passes draw a different set of objects each frame), `commandCount` (when it changed by half or more, at least 4) and
  `hints`.
- `onlyInBaseline` and `onlyInCurrent`: passes with no counterpart.
- `changedPasses`, `onlyInBaseline` and `onlyInCurrent` hold at most 32 entries each, the latest in the frame (tonemap,
  UI and output come last); earlier entries are left out, and the counts are complete.
Use it before and after a resource upgrade or a mod change, on the same spot. First compare two snapshots of an
unchanged still scene, to see what varies on its own (passes that come and go with effects or particles).

Limits: hints are candidates from formats, sizes, blend state and command order, never proof; the skills require proving
the signal (`devkit_get_draw`, the shader, `devkit_watch_region`) before calling a pass the tonemap. Snapshots record no
viewports, vertex counts or depth targets: a "large" target covers at least a quarter of the swap chain's back buffer
(without a swap chain, of the largest target written), so passes at a lower render resolution still count, and depth-only
passes into different depth targets merge into one pass. On D3D9 the blend state is read from the device's render states
(`GetRenderState`) at each recorded draw, since D3D9 blending is not part of a pipeline and state blocks and `Reset` change
it without a ReShade event; a failed read or the obsolete `D3DBLEND_BOTHSRCALPHA`/`BOTHINVSRCALPHA` factors leave it
`null`. On OpenGL it is read from the current context per draw buffer (`glIsEnabledi(GL_BLEND)`, `glGetIntegeri_v`
blend factors and equations, `glGetBooleani_v(GL_COLOR_WRITEMASK)`, blend color, logic op) at each recorded draw, since
GL blending is context state; each draw buffer's state is stored at the color attachment it writes (from
`GL_DRAW_BUFFERi`), and attachments no draw buffer writes report blending off with an empty write mask. Without a current
context or the GL entry points, or with a value ReShade cannot express, it stays `null`. UI drawn
with alpha test and no blending (D3D9, legacy GL) is not a UI candidate. Otherwise blend state is only recorded from
pipelines that carry one, so `blendEnabled` is `null`, and those draws are never UI candidates, for a D3D11
draw that never bound a blend state, and for any pipeline whose blend state the Devkit did not record: D3D10/11 blend
states and D3D12/Vulkan pipelines created before the Devkit loaded. D3D12 pipeline states with a root signature embedded
in the shader (created with a null layout) do get their blend state recorded. A draw never reports the previous
pipeline's blend state instead. With a mod loaded, every pass that renders into
the mod's float clones or upgrades can match the tonemap rule; the first candidate after the scene passes is the likely
one. Readers come from the shader resource views recorded per command (pixel stage for draws, compute for dispatches) and
copy sources: reads through D3D12/Vulkan descriptor tables the snapshot did not walk, and bindless reads, are missing.
OpenGL: units bound to texture name 0 are not inputs. A texture the resource tracker has no entry for (bound with
`glBindTextures`/`glBindTextureUnit`/`glBindMultiTextureEXT` before it had storage, or given storage without a ReShade
event such as `glCopyTexImage2D`) is described at the draw from ReShade's GL device on the drawing thread's context, with
upgrade and clone info only when the tracker knows the texture; names that are no texture any more (`glIsTexture`),
buffer textures, `GL_TEXTURE`-targeted binds and textures without storage stay out. ReShade reports no bind for
`glBindTexture(target, 0)` nor for `glBindTexture` of a texture without storage, so such a unit keeps listing the texture
bound before.
Copies are described from the resource tracker's current entry for each handle, after the snapshot: a texture destroyed
since then is left out, and a handle reused since then (for example after a resize) describes the new resource. `maxPasses` (default 40) and `maxResources` (default 32) cap the output; the budget goes to tonemap candidates,
the output pass, UI, LUT builders and LUT readers first, then to the latest passes. Tested in Sleeping Dogs DE (D3D11,
2026-10-22) with the first version of the tool; the later fixes to `orderUnknown`, the LUT readers, the UI start and the
final-pass note are not tested yet, and nothing has run on D3D12, Vulkan or OpenGL.

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

That installs `cmd_Decompiler.exe` and the `d3dcompiler_46.dll` it imports into `.\bin` (Windows does not ship that
DLL version; without it the decompiler exits with `0xC0000135`, "DLL not found", and every DXBC decompilation fails).
A `bin` set up before the script copied the DLL holds only the `.exe`, and the script cannot read that `.exe`'s version
to repair it: delete `bin\cmd_Decompiler.exe`, then run the script with `-Install`. Point `devkit_set_tools_path` at
`bin` once per game; a decompilation that failed before is tried again on the next `devkit_get_shader` or
`devkit_write_shader_stub` call. For the full tool bootstrap and the remaining optional reverse-engineering binaries, see `docs/CONTRIBUTING.md`.

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
6. call `devkit_load_live_shaders`; a file that failed to compile lists the compiler's message as `compilationError`
   (`failedCount` counts them). That shader is then left with no replacement at all, so the game's original shader runs
   even where a game mod replaces it; `devkit_unload_live_shaders` brings the mod's version back. With the Devkit's Live Reload setting on, the Devkit loads changed files itself at each present and this call
   finds nothing new; compile errors are then only in `ReShade.log`.
   `devkit_unload_live_shaders` resets only the shaders whose source is a live file, and a shader a game mod also replaces
   goes back to the mod's version. The overlay's "Unload Shaders" menu item still removes every runtime replacement,
   including the mod's: with a mod loaded, all its replacements are runtime ones while the Devkit runs.
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

### Formats

`devkit_analyze_resource` decodes RGBA8/BGRA8 (UNORM and sRGB, with or without alpha), R10G10B10A2/B10G10R10A2,
R11G11B10 float, R9G9B9E5, RGBA16 UNORM and float, RGBA32 float, and the one- and two-channel R8, R16 UNORM, R16 float,
R32 float, R8G8, R16G16 UNORM/float and R32G32 float (for example a 1x1 auto-exposure texture). One- and two-channel
values land in `channels.r` (and `channels.g`) with B = 0, so the luminance fields mean nothing there and a `signal`
is refused (with `frames`, `rMean` and `gMean` carry them per frame). Depth views (D16/D24/D32) are not read; a
depth buffer seen through its R32_FLOAT or R16_UNORM shader view decodes as one channel, read whole (a region is refused:
D3D10/11 drop and D3D12 rejects a box copy of a depth-stencil resource); `devkit_watch_region` refuses depth-stencil resources.
Both check the tracked description and the device's own; `analysisTarget` reports `resolvedResourceUsage`,
`nativeResourceUsage` and `depthStencil` when an analysis succeeds. A texture without depth-stencil binding (for example a
typeless copy the game makes of its depth) is not refused by this guard, but still decodes only through a supported view
format such as R32_FLOAT or R16_UNORM. R9G9B9E5 is refused on OpenGL
(ReShade's GL readback of it is not valid). `devkit_watch_region` reads the one- and two-channel formats and R9G9B9E5
too, but not on D3D9 (nor R9G9B9E5 on OpenGL), and BGRX8/BGR10A2 only on D3D9 (see its limits below).

### Signal hints

Every `devkit_analyze_resource` result has `analysis.pixelsAnyChannelExactly1` and `analysis.signalHints`, a list of
facts each labelled with the rule that produced it: `unormStorage` (a UNORM format stores 0 to 1; whether that clipped
light depends on the encoding, PQ in 10 bits covers 0 to 10000 nits), `aboveOne` (a float format holding values above 1),
`atOne` (a float format with at least 1% of pixels exactly at 1.0: consistent with a saturate or clamp, or white
content), `negative` and `nonFinite`. For one- and two-channel data (motion vectors, normals, masks) only
`unormStorage` and `nonFinite` are given. They are candidates
for classifying a signal as untonemapped, neutral SDR or graded SDR, which still has to be proven with the snapshot, the
shader and `devkit_watch_region`; the values alone cannot tell linear from gamma-encoded data, so no such guess is made.

### Banding (`banding: true`)

`devkit_analyze_resource` with `banding: true` (one frame, no A/B) adds `analysis.banding`: per channel, the steps
between neighbouring pixels in the values as stored (UNORM formats in codes, an sRGB format re-encoded first; float
formats as the step relative to the value and as the absolute step, log-binned). A step is smooth when it is at most 4
codes, or for a float at most 2% of the value or 4/255. `smoothShare` is the share of smooth or equal neighbour pairs,
`equalShare` the share of equal pairs, `medianStepCodes` (UNORM) or `medianStepRelative`, `medianStepAbsolute` and
`medianStepIn8BitCodes` (float, the absolute step x 255; log-binned, so within about 20%) the typical smooth non-zero
step. A band is a horizontal run of
at least two equal values bounded by smooth non-zero steps on both sides: `bands`, `meanBandLength`, `longestBand`,
`bandsAtLeast8px`. Use a region on a smooth gradient (sky, fog); with too few smooth steps the block says so. Long bands
bounded by 1-code steps are visible banding; dither reads as many short bands and is fine. To tell whether a mod's float
clone still carries 8-bit steps from upstream, compare the clone (`preferClone: true`) with the RGBA8 original
(`preferClone: false`) on the same region: `medianStepIn8BitCodes` of the clone approximates `medianStepCodes` of the
original only when both hold the same encoding (a linear float clone of an sRGB-encoded target does not). Rows are streamed (only
the previous row is kept), so memory does not grow, but a float frame costs several logarithms per pixel on the game's
thread: use a region.

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
  after 15 s; so does a D3D9 present that ReShade skips). With a RenoDX DX9 proxy mod the effect runtime runs on the
  Display Proxy's device, so `frame_end` on the D3D9 device is expected to time out: measure the final output on the proxy
  device instead (see "Display Proxy device" below). On Vulkan the analysis cannot flush
  ReShade's immediate command list and wait inside `reshade_present` (that would submit ReShade's effect commands without
  the present's wait semaphores). There a back buffer of the presenting swap chain is copied into a Devkit-owned texture on
  that list, which ReShade submits with the semaphores at present, and the analysis reads the copy in `finish_present`
  (the same split the watch uses for its stats signal). Any other texture asked for with `frame_end` on Vulkan is read
  after the present as it is, which is the same as `present` for it.

### Several frames (`frames`)

`frames: N` (1-32) measures N consecutive frames: the request stays queued and is analyzed once per present on the
game's own thread, so no frame is skipped between samples (each sample carries its frame number). The game stalls while
each frame is analyzed; a 4K swap chain with `signal` can take seconds in all, and other Devkit tool calls wait. The
result keeps `analysis` for the last frame and adds `frames`: `samples` with headline numbers per frame (luminance
mean/max, nan and negative ratios; with a signal nits mean/max/p50/p90/p99/p99.9, shares above 203/400/1000 nits,
saturation mean/p90) and `summary` with n, mean, stddev, min and max of each. Use it wherever single frames vary: in
Sleeping Dogs the image alternated between a bright and a dim state from one frame to the next (means 10.6 vs 6.2 nits),
so two single captures could differ by 70 % with nothing changed. `frames` above 1 needs `measurePoint: present` and
writes no image; an `outputPath` ending in `.json` is allowed (see below).

`abShaderHash` turns it into an A/B of one shader: `frames` samples per side, on consecutive frames in the order A, B,
B, A, ... (which balances a scene that flips between two states each frame). The game's thread switches the shader's
source between `abSourceA` (default `original`) and `abSourceB` (default `addon`), each `original`, `addon` or `file` as
in `devkit_set_shader_source`, starting after one warm-up frame, and puts the shader's previous source back at the end,
also when a frame or a switch fails. The result adds `ab` with each side's summary, `difference` (B minus A of each
mean, and `rangesOverlap` when the two sides' min-max ranges overlap: a difference within frame noise) and
`restoredSource`; `frames.samples` carry their `side`. Run A against A first (`abSourceA` = `abSourceB`) to see the
noise floor of the spot. Limits: `measurePoint: present`, no image `outputPath`, the game's device (not a Display Proxy), and
D3D9/D3D10/D3D11/OpenGL only, since on D3D12/Vulkan a switch destroys a pipeline that frames still in flight may use.
Only that shader changes: the mod's resource upgrades and swap-chain proxy stay, so `original` is not a vanilla view.
In an A/B, `frames.summary` is left out (it would mix both sides); `ab.a` and `ab.b` carry the summaries.

More A/B rules and caveats:
- Refused while the Devkit's Live Reload setting is on: it loads changed files at every present and would switch the
  shader in the middle of the run. If it is turned on during a run, the next switch fails and the shader is put back
  (the restore itself is never refused).
- The previous source is put back on completion, failure and cancel. The recorded source can describe nothing active
  (`file` after a live file failed to compile); then the shader falls back to `addon`, then `original`, and
  `ab.restoredSource` and `ab.restoreNote` say so. After the overlay's "Unload Shaders", which removes the replacements
  without resetting the recorded source, the restore brings the recorded replacement back.
- The call gives up after 180 s (a game that stops presenting, for example minimized) and cancels the rest; the shader
  gets its previous source back at the next present.
- A frame measured right after a switch uses the new source for draws on the immediate context. Command lists recorded
  ahead (D3D11 deferred contexts) can still hold the old one, and effects that carry earlier frames (TAA history, eye
  adaptation, bloom feedback) mix the other side into each sample: an A-against-A run shows how much noise remains,
  but not that lag.
- Samples follow the device's presents. With several swap chains on one device they interleave windows, and `frame`
  counts the presents the request saw, not the game's frame numbers.

### Saving a result to compare runs (`outputPath` ending in `.json`)

An absolute `outputPath` ending in `.json` writes the finished result (the `structuredContent` the call returns, with
`frames` and `ab` when present) instead of an image, and the result gains `jsonPath` (or `jsonError` when the file could
not be written; the measurement itself is still returned). It is the one output allowed with `frames` above 1 and with
`abShaderHash`. Use it for comparisons that span a game restart or a rebuild, where the two results are never in one
conversation turn:

- Before and after a rebuild: same spot, same region and signal, `frames: 8` or more, one file per build.
- Mod on against mod off (a vanilla reference in the same scene): measure with the mod, close the game, rename the mod's
  `.addon64`/`.addon32` so ReShade does not load it (the Devkit alone still loads), launch to the same spot, measure
  again, close the game and rename it back. Unlike an A/B (`original` keeps the mod's upgrades and swap-chain proxy),
  this compares against the game as shipped. Add `devkit_frame_overview` `saveBaseline`/`compareWith` across the two
  launches to see which passes the mod changed. Resource handles change with every launch: take a new snapshot and look
  the target up again. Measure a target that has the same format in both runs, with the same signal. The swap chain does
  not qualify: with an HDR mod it is HDR10 (or scRGB), without it usually 8-bit sRGB, and `signal` has no SDR decode, so
  `pq` on the vanilla back buffer gives wrong nits and `raw` values of the two encodings do not compare.

Compare the two files metric by metric, after checking that format, region and signal match: a difference whose min-max
ranges overlap is within frame-to-frame noise; a single-frame result has no noise floor, so its differences cannot be
judged.

### LUTs (`devkit_analyze_lut`)

`devkit_analyze_lut {resourceViewHandle, layout?, flipY?, outputPath?, preferClone?}` reads a texture as a colour lookup table at the
Devkit's next present and summarizes it, so a LUT audit needs no dump script. Layouts: `3d` (an N x N x N 3D texture),
`strip_x` (N*N x N: N slices side by side, as Unreal's 256x16 and Unity's 1024x32) and `strip_y` (N x N*N); `auto`
picks one from the shape (any N from 2 to 256; `devkit_frame_overview` lists the usual sizes as LUT textures). Input point (r, g, b) / (N - 1)
is in the LUT's own coordinates: many games address their LUT with log, PQ or gamma-encoded colour, so the numbers
describe the table, not display light.

- `corners`: the outputs at black, red, green, blue, yellow, magenta, cyan and white. Check them first: a primary at the
  wrong corner means the layout or `flipY` is wrong. `flipY` reads the image bottom-up (green increases upward; for
  `strip_y` the slice order reverses too).
- `neutralAxis` and `neutralAxisMonotonic`: the output along (i, i, i), the LUT's tone curve in its own coordinates.
- `identityDeviation` (mean and max per channel, p99 of each point's largest channel deviation, the worst point) and
  `isIdentity` (every channel within 1.5/255, and no NaN or infinite value). For an sRGB format the comparison uses the
  stored, encoded values.
- `outputRange`, `valuesAboveOne` and `nonFiniteValues` (both count channel values), `allZero`. All zero usually means the read came before the LUT is
  built in the frame or after its texture was reused; read it again from a pass that samples it.
- `outputPath` (absolute `.csv`) writes every lattice point with its indices, input and output, for plots
  (`analysis-graphing` skill), for N up to 65; it is written on the game's thread, so larger LUTs get `csvError` instead.

The view must select the top level of a single texture. 3D LUTs are read on D3D10/11/12 and OpenGL; they are refused on
D3D9 (ReShade does not copy volume textures there) and on Vulkan (ReShade reads back through LINEAR-tiled images, which
Vulkan guarantees only for 2D). Strips are read on every API. As in `devkit_analyze_resource`, the copy issues no state
transition on D3D12/Vulkan, so a LUT in a shader-read state is copied as it is (the validation layers would flag it).

### Tone curve of a pass (`devkit_measure_tone_curve`)

`devkit_measure_tone_curve {drawIndex}` measures the effective transfer of one pass of the current snapshot, for example
the tonemap candidate of `devkit_frame_overview`: what output the pass makes from what input, so a new mod can match the
game's own curve (mid grey, shoulder, peak).
- The draw's first render target is the output; the input is `inputSlot`, or else the first shader resource of the
  target's size (a float one if there is one). Each is resolved to a mod's active clone when it has one (the texture the
  GPU really renders: Sleeping Dogs' tonemap targets are game r8g8b8a8 with r16g16b16a16_float clones), else to the
  texture itself, so the call also works without a mod; both are read at the same present,
  so both come from one frame. They must be the same size; `regionX/Y/Width/Height` applies to both.
- Pixels are paired one to one. Input luminance (`inputSignal`: `raw` = BT.709 luminance of the decoded values,
  `scrgb`/`pq` = nits) is split into `bins` log-spaced bins (default 24) between the input's 0.5th and 99.5th
  percentile; bins with fewer than 16 pixels are left out. Each point is the bin's median input and median output
  (`outputSignal`: `raw`, or nits for `pq`/`scrgb`).
- Also reported: `midGray` (output at mid grey and the log-log slope there; mid grey is a linear 0.18 for a `raw`
  input and 14.4 nits for `scrgb`; a `pq` input is absolute display light with no scene mid grey, so none),
  `peakOutputP99_9`, `highestMedianOutput` and `shoulderInput` (where the median output first reaches 90% of its highest
  value, reported only when that value is positive; `null` when only the top bin reaches it: the curve rises into the
  last bin, and a clip or knee inside that bin is not resolved; more `bins` or a smaller region can).
  Pixels with any non-finite channel, alpha included, are left out.
- Validity: both textures are read at the end of the frame, after the whole frame. When the snapshot shows a later
  draw, dispatch or copy writing the input or the output again (a ping-pong chain, UI drawn onto the output), the call
  refuses and lists them (`laterInputWriters`, `laterOutputWriters`); `allowLaterWriters` overrides it when a region
  keeps away from what they touch. Clears, resolves and uploads after the pass are not seen. The call also refuses while
  a snapshot is queued or capturing. The snapshot and the read are different frames: take them on the same, still scene.
  The view handles come from the snapshot; a handle the game recycled since (a D3D12 descriptor, a Vulkan view) would
  read another texture, as with `devkit_analyze_resource`. Depth buffers are never picked as the input.
- It is the pass's overall mapping, not a formula: grading, LUTs and local effects (bloom, vignette, local contrast)
  also move pixels, so points scatter where those act. Compare with `devkit_set_shader_source` original vs addon to see
  what a replacement changed.

### Display Proxy device

A RenoDX Display Proxy (a DX9 or OpenGL proxy mod such as `princeofpersiaww`, or the Devkit's own Device Proxy) copies the
game's frame into a D3D11 device and swap chain it creates (`utils/device_proxy.hpp`), draws its swap-chain pass there and
presents HDR10 on it. The game's device only holds the float intermediates; the encoded output exists only on that D3D11
device. The Devkit lists it after the game's device, measure-only:

- `devkit_status` shows it with `api: d3d11`, `isProxy: true` and `backBufferViews` (one entry per back buffer:
  `resourceViewHandle`, `resourceHandle`, `width`, `height`, `format`). The proxy draws through the ReShade API, which raises
  no `init_resource_view`, so the Devkit creates these render-target views itself when the swap chain starts (and releases
  them before every resize, which DXGI requires) and registers them with `utils::resource` so the measuring tools accept them.
- Measure the final output with `devkit_analyze_resource`, `deviceIndex` of the proxy, one of its `backBufferViews` as
  `resourceViewHandle`, `signal: pq` and `measurePoint: frame_end`. `present` reads inside the Devkit's handler for the
  proxy's present, which can run before the mod's handler draws the swap-chain pass (handlers run in load order). ReShade's
  overlay and effects (Lilium) run on the proxy's own effect runtime (its own ReShade ini, usually `ReShade2.ini` in the
  ReShade base folder), so `frame_end` includes them: close the menu and disable them first. Without that runtime
  (`[INSTALL] MaxEffectRuntimes` set to 1) `frame_end` on the proxy times out after 15 s. ReShade's D3D11 swap chain exposes
  one back buffer, so there is one view per proxy swap chain; after a resize or a proxy restart it has a new handle, so
  read `devkit_status` again (a request on a released view fails as not tracked).
- `devkit_watch_region` works on those views, but the proxy pass is drawn through the ReShade API and is never a writer, so
  only `endOfWatch` (and `verify` at that point) is meaningful. `withSnapshot` links nothing on this device.
  `devkit_dump_resource_with_hash` also reads them.
- `devkit_select_device`, `devkit_queue_snapshot`, shader inspection (`devkit_get_shader`, `devkit_dump_shader`), live
  shaders, resource clones and texture replacement return an error for its index. The snapshot listings
  (`devkit_snapshot_summary`, `devkit_list_draws`, `devkit_get_draw`, `devkit_list_shaders`, `devkit_frame_overview`,
  `devkit_list_replacements`) return empty results for it. The overlay does not list it. The device handlers keep only what
  the watch needs on it (command list and lifetime hooks); no shaders, pipelines or draws are tracked.
- The game's device is listed first and stays the selected device, so tools without `deviceIndex` keep working on it (the
  proxy is created at the game's first present and torn down with the game's device). Other non-D3D9 devices of a D3D9
  game (video, overlays) stay unlisted, as before.

### Colour statistics (saturation and hue)

With `signal: pq` or `scrgb`, `analysis.color` describes the chromaticity of the display light in CIE 1976 u'v' (XYZ from
the signal's primaries, D65 white). Pixels below `minNits` (1 nit), whose chromaticity is mostly noise, are left out
(`pixelsCounted`):

- `saturation` (min/max/mean) and `saturationPercentiles` (p50/p90/p99/p99_9) of s_uv = 13 x the distance from the D65
  white point. Unlike chroma in ICtCp or CIELAB the metric does not change with brightness, so an HDR preset that only
  brightens a colour leaves it unchanged (perceived colourfulness still rises with luminance, the Hunt effect, so equal
  s_uv does not mean equal-looking saturation between SDR and HDR). For orientation (computed): white 0, light pink 0.07, skin about 0.55, BT.709 cyan 0.79,
  yellow 1.10, green 1.55, magenta 2.28, red 3.36, blue 4.05; BT.2020 red 4.71.
- `pixelsChromatic`: pixels with s_uv >= `neutralSaturationBelow` (0.05).
- `hueHistogram`: 12 bins of 30 degrees of the hue angle around D65 (`atan2(v' - v'n, u' - u'n)`), each with its share
  of the chromatic pixels. For orientation (computed): BT.709 red about 12 degrees, yellow 86, green 128, cyan 192, blue
  266, magenta 308.

Use it to compare two captures of the same spot with the mod's own toggle or preset, if it has one (for example a vanilla
tone mapper setting against the HDR one), ideally on the same small region: a lower saturation mean or p90 means the mod
desaturates, a shifted histogram means hue shifts. Compare `pixelsCounted` first: the 1-nit floor selects pixels by
brightness, so when a preset moves many pixels across it the two captures measure different pixels and the comparison is
not valid. For one shader, `devkit_set_shader_source` (or `abShaderHash` with `frames`) switches between the game's
original and the mod's replacement; the mod's resource upgrades and swap-chain proxy stay, so neither side is a vanilla
view. `abShaderHash` runs on D3D9, D3D10/11 and OpenGL only; on D3D12 and Vulkan a switch can still lose the device (see
`devkit_set_shader_source` in section 3).

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

To follow one pixel through the frame, watch a 1x1 region (`regionWidth` and `regionHeight` 1) with `frames: 1` and
`withSnapshot: true`: the trace then lists every writer of that pixel in command order with its value before and after
(the region statistics of a single pixel) and, through `snapshotDraw`, the draw's shaders and inputs. Add
`includeUnchanged: true` to also see the writers that left it unchanged.

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
- `withSnapshot: true` (`frames: 1` only) also captures a snapshot of the watched frame and links each traced writer to it:
  draws, dispatches, copies and the member draws of a render pass get `snapshotDraw` (the index for `devkit_get_draw`) and
  their `targets` and `inputs` as `devkit_frame_overview` describes them (format, size, upgrade, clone). Clears, resolves
  and updates are not snapshot commands and get none. `snapshotLinkedCommands` counts the linked commands (writers and render-pass member draws);
  `snapshotLinkError` says why none could be linked (another snapshot was queued or running, replaced it, or had not
  finished); the text summary says the same. A key includes the command list's recording number, so a D3D12/Vulkan list
  reset and re-recorded within the frame does not link to the wrong command.
  The snapshot starts at the frame end before the watch's first counted frame (the watch's start on D3D9/D3D11/OpenGL,
  the end of the warm-up frame on D3D12/Vulkan) and ends at the next present, before the watch completes. Each command is
  linked by the key the snapshot gives it while recording (command list, its recording, command number in it), read by the watch hook,
  which runs before the snapshot's: D3D12/Vulkan commands recorded before the watched frame and executed in it are not in
  the snapshot and stay unlinked. The snapshot replaces any earlier one, and a snapshot frame is slow, so the call may
  take up to two minutes longer.
- Every call returns `writers`, aggregated per writer (kind + pixel/vertex/compute hash; render passes by the set of their pixel
  shaders): executions, frames seen, how often the region changed, min/mean/max of the region peak and mean before and after,
  how often the peak was lowered or raised, how often values went from above 1.0 to at most 1.0, and `afterPeakMode` (the most
  repeated after-peak, a ceiling candidate).
- Alpha: every measurement has `maxAlpha` and `pixelsAlphaAboveOne` (alpha as stored, of the finite pixels; formats
  without alpha read 1, so their numbers say nothing), and `writers[]` add `raisedAlphaAboveOneExecutions` (no pixel's
  alpha was above 1.0 before the writer and some is after) and `afterMaxAlpha`. This finds the draw that pushes alpha out
  of range after an 8-bit target was upgraded to float (Dead Cells: the glow layer's alpha summed to 3.5-7.2). `verify`
  checks both alpha numbers against the CPU. Same layout on every API (the reduce shaders' slots 14 and 15; D3D9 group 3).
  A writer that changes only alpha counts as changing the region (`changed`, `changedExecutions`).
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
- The atlas is the non-sRGB twin of an sRGB source (`glCopyImageSubData` copies the bits), so an sRGB texture is measured
  in its stored, encoded values, as on every API (`devkit_analyze_resource` decodes it instead); the default framebuffer is
  measured as stored. Supported: RGBA8 (also sRGB), RGB10A2, R11G11B10F, RGBA16, RGBA16F, RGBA32F, R8, R16, R16F, R32F,
  RG8, RG16, RG16F, RG32F colour textures, renderbuffers and the default framebuffer (a back buffer without alpha bits,
  reported as an x8 format, is watched as RGBA8). Refused: multisampled,
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
  have it). `devkit_analyze_resource` and `devkit_dump_resource_with_hash` refuse such images with the same hint instead of
  issuing an invalid copy. The Devkit setting **Watch Region: Copyable Vulkan Images** (Settings > Other, ReShade.ini key
  `WatchRegionCopySourceUsage` in `[renodx-dev]`, on by default) adds `copy_source` usage to every image the game creates
  from then on, in the Devkit's `create_resource` handler. A change only affects images created afterwards: restart the
  game after toggling it. A `WatchRegionCopySourceUsage=0` left in a game's ReShade.ini by an earlier toggle keeps it
  off there. It follows the rules of the Vulkan clone targets in `utils/resource_upgrade.hpp` (`OnCreateResource`):
  shared and sparse images and formats without copy support are left alone. Interaction with RenoDX mods: ReShade passes the
  same `desc` to every add-on's `create_resource` handler, and resource upgrades only OR in their own usage bits (in-place
  upgrades) or start from the created image's desc (clones), so the flag is kept whatever the add-on order. One exception:
  when the mod's handler runs before the Devkit's, the fallback desc a resource upgrade records for the original format does
  not have the flag, so fallback copies the mod creates later cannot be watched. Side effects, because ReShade rebuilds
  the `VkImageCreateInfo` of every image an add-on changes: colour and depth attachments also get `TRANSFER_DST` (ReShade
  adds it for clears), and a game's `TRANSIENT_ATTACHMENT` bit, which the add-on API cannot see, is kept next to the
  transfer bits, which Vulkan does not allow (RenoDX in-place upgrades already do the same to the images they touch).
  External-memory (interop) images arrive as `shared` and are left alone. The flag can disable some driver compression
  (mainly a concern on AMD; not measured). Measured: DOOM 2016, NVIDIA RTX 5070 Ti (driver 621.1), 3840x2160, one spot,
  GPU 3.5 ms average in the game's performance overlay (0.1 ms resolution) with the setting on and off (2026-10-22).
  If a Vulkan game crashes, fails to create images, or runs slower only with the Devkit, try the setting off first.
- The measurement copies and barriers cost GPU time and VRAM; use a region or `shaderHash` for long watches.

Limits on every API: 2D, non-MSAA textures in RGBA 8-bit, 10-bit, R11G11B10, 16-bit or 32-bit float formats, and except on
D3D9 the one- and two-channel R8, R16, R16F, R32F, R8G8, R16G16, R16G16F and R32G32F and R9G9B9E5 (not on OpenGL). One-
and two-channel values are measured in R (and G): the peak is the largest channel (`peakMeasure: maxChannel`), the means
and minimums stay BT.709 luminance of (R, G, 0) and mean nothing there, and a `signal` is refused. Up to 1023
measured writers per frame (D3D9: the atlas slots; `truncatedFrames` reports frames that hit it or the scratch budget).

- Only commands the game issues are hooked. Passes that add-ons draw through the ReShade API (for example a mod's swap chain
  proxy pass at present) are not reported as writers; their effect lands in the end-of-frame measurement. When a mod
  redirects the game's rendering into clones, watch the clone (`preferClone: true`).
- D3D11 draws that write the texture through a pixel-shader UAV (`OMSetRenderTargetsAndUnorderedAccessViews`, slots 0-7)
  are reported as draw writers. D3D11.1 slots 8-63, and pixel-shader UAV writes on D3D12/Vulkan, are not detected. Not yet
  tested in a game.
- If the watched texture is destroyed during the watch, or the swap chain the watch measures on is resized, switched to
  or from fullscreen or closed, the watch stops and returns an error.
- When another add-on performs a write itself inside the same ReShade event as a game writer (for example a mod redirecting a
  clear onto its clone), its handler may run before or after the Devkit's, so that write lands on either side of the
  measurement.
- `devkit_analyze_resource` reads by default inside the Devkit's own `present` handler, so on a back buffer it can miss
  what add-ons loaded after the Devkit draw at present. `measurePoint: "frame_end"` reads in `reshade_present` instead (see
  "Measure point" in section 10).
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
- Exit hang (fixed, not yet tested in a game). ReShade unloads add-ons with `FreeLibrary` when the last device is destroyed
  (`reshade::unload_addons`) while the MCP threads still run. The Devkit stopped its MCP server in `DllMain`, where joining
  those threads deadlocks on the loader lock, and the pipe server waited for connection threads that process termination had
  already killed. Now:
  - the server stops in `AddonUninit`, which ReShade calls before `FreeLibrary`, outside the loader lock;
  - `utils::ipc::Server::Stop` joins its connection threads instead of waiting for them to unregister, so no connection
    thread runs Devkit code once it returns;
  - on process termination nothing is stopped (the session object is never destroyed), since the other threads are gone;
  - analysis, dump and live-shader requests still waiting for a present fail when their device is destroyed.
  A tool call sleeping in `delaySeconds` still delays the unload until its delay ends (up to 30 s). Seen before in Gears of
  War: E-Day (D3D12), Sleeping Dogs DE (D3D11) and Wolfenstein: The New Order (OpenGL + D3D11 proxy).

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
