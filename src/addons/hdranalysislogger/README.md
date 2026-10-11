# HDR Analysis Logger

Standalone ReShade add-on that reads the statistics computed by Lilium's
HDR/SDR analysis effect (`lilium__hdr_and_sdr_analysis.fx`,
[EndlesslyFlowering/ReShade_HDR_shaders](https://github.com/EndlesslyFlowering/ReShade_HDR_shaders))
back to the CPU and writes them as JSON. It also measures a small square
or rectangular region of the image (screen centre or a drawn rectangle) so a single light
source can be isolated. It does not depend on a RenoDX game mod; the region
works even without the analysis effect.

## How it works

The effect keeps its per-frame statistics in a small R32F texture,
`TextureConsolidated`. After ReShade finishes rendering effects, the add-on
copies that texture to a CPU-readable resource, waits for the copy, and decodes
it. The texture width identifies the layout (colour space and compute vs.
pixel-shader path); offsets follow the `COORDS_*` defines in
`lilium__include/hdr_and_sdr_analysis/main.fxh`. An unknown width is reported
as a status message instead of guessing.

Values are the live per-frame results. Averages are clamped to `[min, max]`
the same way the effect does before showing them.

## Measurement region

Before any ReShade effect runs, the add-on copies a box of the back buffer and computes max/avg/min luminance, R/G/B and CLL for
it. Measuring before effects keeps Lilium's text, heatmap and active-area
blackout out of the numbers; it also means effects placed before Lilium in
the chain are not included.

The image is interpreted with the effect's defaults: 16-bit float as scRGB,
10-bit as HDR10 PQ, 8-bit as gamma 2.2. HDR values are nits with R/G/B in
BT.2020; SDR values are percent with R/G/B in BT.709.

Position is either a square at the screen centre (default 16 px; aim the
camera at the target in mouse-look games) or a drawn rectangle: open the
ReShade overlay, click **Draw region**, then either drag the existing cyan box
onto the target (press inside it, move, release; its size is kept) or draw a
new one: click one corner of the light source and then the opposite corner
(press, drag and release also works). The overlay can be closed first so it doesn't cover the target: while
drawing, game mouse/keyboard input is blocked and a cursor is shown until the
rectangle is done. Esc cancels. The rectangle is stored as fractions of
the screen, so it is screen-space: if the camera moves, the target can leave
it. A cyan outline shows the region; it is drawn after effects, so it is
never measured. Drawing it every frame keeps ReShade's GUI active while the
overlay is closed, which costs a little performance; turn the outline off if
that matters.

## Output

Written to `<ReShade base>/renodx-dev/hdr_analysis/`:

| File | Contents |
|---|---|
| `latest.json` | Most recent sample, overwritten every sample. |
| `session-<timestamp>.jsonl` | One sample per line while "Write continuous session log" is on. |
| `series-<timestamp>-<label>.json` | A recorded series: per-field min/mean/max/count summary plus all samples. |

Each sample contains the time, frame counter, game folder, graphics API,
back buffer format, the effect's colour space (`scrgb`, `hdr10`, `sdr`),
units (`nits` in HDR, `percent` in SDR), the active-area crop settings, and
`values`:

- `max_luminance`, `avg_luminance`, `min_luminance`
- `max_r`/`avg_r`/`min_r`, same for `g` and `b`, and `max_cll`/`avg_cll`/`min_cll`
  (compute path only). These are BT.2020 "optical" values in HDR and BT.709
  relative values in SDR, as on the overlay; max/min of each channel are not
  from the same pixel.
- `gamut_bt709_percent`, `gamut_dci_p3_percent`, `gamut_bt2020_percent`, and in
  scRGB also `gamut_ap0_percent`, `gamut_invalid_percent` (HDR only).

`values` and the related fields are omitted, and `analysis_status` explains
why, when the analysis effect is missing or disabled.

`region` holds the anchor (`screen_centre` or `drawn`), box position and
width/height in pixels, colour space,
units, and its own `values` (luminance, R/G/B, CLL; no gamut). In a series
summary, region fields are prefixed with `region_`.

## Controls

ReShade overlay → Add-ons → HDR Analysis Logger:

- Sample interval (default 500 ms). Each sample stalls the GPU briefly to wait
  for the copy.
- Write continuous session log (default on).
- Series label and duration (default 5 s), then **Record series** or the
  series hotkey (default **Shift+F9**). A series samples every 100 ms. A
  banner at the top of the screen shows the countdown while recording and
  "Logging complete" with the file name for 5 s afterwards (also with the
  overlay closed).
- **Change hotkey** rebinds the series hotkey: press the new combination
  (Ctrl/Shift/Alt modifiers are captured), or Esc to cancel. Saved in
  `ReShade.ini` under `[HDRAnalysisLogger]`.

- Measurement region: position (screen centre or drawn rectangle), square
  size, outline, **Draw region**.

ReShade effects must be enabled for anything to be sampled. Not captured:
waveform, CIE diagram, and percentiles (the effect does not compute them).
