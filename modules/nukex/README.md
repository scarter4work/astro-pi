# NukeX

Distribution-fitted image stacking and auto-stretch for PixInsight, with
filter-aware colour science: dual-narrowband OSC data is decomposed into
emission lines through the camera's measured quantum efficiency and composed
with a calibrated palette.

Rather than averaging or sigma-clipping, NukeX fits a distribution to every
pixel's samples across the stack and estimates the true signal from the fitted
model. Each pixel carries its own record — Welford accumulators, a histogram,
the fitted distribution and its robust statistics, per channel — and the
selection is made from that record.

## Install (PixInsight 1.8.9+, Linux x64)

Resources → Updates → Manage Repositories → Add:

    https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/

then Check for Updates and restart. The package installs `bin/NukeX-pxm.so`
under the PixInsight base directory; the camera (QE) database is built into
the module.

## Use

Process → NukeX. Add light frames (and optional flats), pick a stretch — Auto
is recommended — and Execute.

Outputs:

| window | contents |
|---|---|
| `NukeX_stacked` | the stack, linear |
| `NukeX_composed` | 3-channel sRGB, when derived emission-line slots exist |
| `NukeX_stretched` | the stretched result |
| `NukeX_noise` | per-pixel noise estimate |

NukeX reads `FILTER`, `BAYERPAT` and `INSTRUME` from the FITS headers to decide
how each frame is routed. An unrecognised dual-narrowband filter on Bayer data
stops the batch rather than guessing; an unrecognised mono filter warns and
stacks as luminance. To teach it a camera or filter it does not know, see
[docs/qe_overrides_format.md](docs/qe_overrides_format.md).

## Known limitations

- **One mono filter per batch.** Stack L, R, G and B separately and combine
  the results. A batch carrying more than one mono filter is refused with an
  explanation rather than producing empty channels.
- **Very long sessions lose frames at the ends.** Alignment is against a
  single reference frame, so frames far from it in time can fail once
  tracking drift accumulates. On a seven-hour set, everything within about
  90 minutes of the reference aligned and the outermost 21 of 72 frames did
  not.
- **A 24 megapixel colour stack wants more than 32 GB of RAM** to stay out of
  swap. It completes on less; it swaps while doing so.

## Build from source

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build

Requires PCL at `~/PCL` (or pass `-DPCLDIR=`), system Eigen 3, glog and Ceres,
and OpenCL headers. GPU work falls back to CPU when no device is available.

See [CHANGELOG.md](CHANGELOG.md) for release notes.

## Licence

MIT. Copyright (c) 2026 Scott Carter.
