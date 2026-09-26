# RenoDX for Gujian 3 (古剑奇谭三)

HDR mod for Gujian 3 (Steam 994280, 64-bit DX11, Aurogon fork of Havok Vision Engine 2014).

## What it does

- Replaces the final tonemap/composite pass `0x2B81D80E` (ps_4_0), which runs after lens flare and before SMAA and the UI.
  The game's exponential curve `(1 - e^-kx)(1 - b e^-kx)^2` is followed by a 0..1 clamp; the replacement keeps the same
  exposure, bloom/glow and gamma steps without the clamp and hands the result to RenoDX tone mapping
  (vanilla SDR image used as the colour-grade reference).
- Tone Mapper = Vanilla reproduces the original shader exactly (including dither).
- Upgrades R8G8B8A8_UNORM / R8G8B8A8_TYPELESS render targets to R16G16B16A16_FLOAT (hard-coded) so highlights survive
  through SMAA to the swap chain. Without it the image caps at Game Brightness (~203 nits).

## Build

`CMake: Set Build Target` → `renodx-gujian3`, `CMake: Build` → `build\Release\renodx-gujian3.addon64`.

## Install

ReShade with full add-on support (dxgi.dll) in `Gujian3\bin64`, copy `renodx-gujian3.addon64` next to it, enable Windows HDR.

## Manual verification

1. Add-ons tab lists "RenoDX for Gujian 3"; ReShade.log shows `Registered runtime replacement: 0x2b81d80e`.
2. Bright daytime scene: HDR Analysis max nits reaches the Peak Brightness setting (not stuck at ~203).
3. Tone Mapper Vanilla vs RenoDRT: similar overall look; UI/minimap stay at UI Brightness.

## Notes

- Devkit users: the game uploads textures with buffers smaller than their pitch implies; the Devkit needed
  `utils::resource::ClampToReadableSize` (local fix in `src/utils/resource*.hpp`) to stop crashing at startup.
