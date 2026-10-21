# Planetary PixInsight Module

Planetary: a SER video viewer for PixInsight, the first step toward lucky-imaging stacking. Its one process, **SERViewer**, opens SER video files
(as written by planetary capture programs such as FireCapture and SharpCap): it shows any frame in a
preview with a frame slider and an info block, and opens the selected frame as an ordinary 32-bit PixInsight
image, debayered (bilinear) or as the raw CFA mosaic. It reads only the SER file you choose, uses no network
and runs no external programs.

**Status:** released as 0.1.0.0 (2026-10-21). The module id `Planetary` and process id `SERViewer` are
permanent: existing installs update by these ids, so never rename them.

**User documentation** (installed with the module as `doc/tools/SERViewer/SERViewer.html`):
[`doc/SERViewer/SERViewer.html`](doc/SERViewer/SERViewer.html). Third-party notices:
[`doc/SERViewer/THIRD_PARTY_NOTICES.txt`](doc/SERViewer/THIRD_PARTY_NOTICES.txt).

This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/).

## Layout

| Path | What |
|---|---|
| `src/ser/` | SER reader + debayer library (`planetary_ser`). PCL-free, unit-tested on its own. |
| `src/module/` | The PixInsight module: SERViewer process, instance, interface, parameters; `PlanetaryVersion.h`. |
| `test/unit/` | Catch2 unit tests (header, frames, debayer, FITS text, preview). |
| `test/` | Headless probe, GUI smoke and real-data harnesses (all through `tools/pi-headless.sh`). |
| `doc/SERViewer/` | Shipped user documentation and third-party notices. |

## Build

From `modules/planetary/`:

```bash
cmake -B build -DPCLDIR=$HOME/PCL -DPLANETARY_BUILD_MODULE=ON && cmake --build build -j$(nproc)
```

Without `-DPLANETARY_BUILD_MODULE=ON` only the SER library and its tests build (no PCL needed).
Shipped binaries come only from the portable Rocky 9 container: `tools/build-env/build-modules.sh` (repo
root) builds all modules into `build-portable/` and runs this module's ctest there.

## Test

```bash
(cd build && ctest --output-on-failure)   # unit tests + fixture writer
bash test/run-probe.sh                    # headless SERViewer.executeGlobal() over synthetic fixtures, slot 96
bash test/gui-smoke.sh <out-dir>          # real GUI on a private Xvfb, XTest-driven, slot 97
bash test/run-real-data.sh                # 2026-09-07 Saturn capture (NAS) -> SER fixtures -> probe + GUI smoke
```

The three scripts sign the module and need `/tmp/.pi_codesign_pass`; without it they print BLOCKED and exit 2.
`PLANETARY_SO` / `PLANETARY_MAKE_FIXTURES` point them at another build, e.g. the container build in
`build-portable/`.

## Roadmap

Stacker (quality ranking, alignment, lucky selection) → derotation → post-processing with live sliders.
