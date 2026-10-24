# Planetary PixInsight Module

Planetary: planetary and lunar video for PixInsight, with two processes.

**SERViewer** opens SER video files
(as written by planetary capture programs such as FireCapture and SharpCap) and uncompressed 8-bit greyscale or raw Bayer AVI files (such as ASIAIR captures, including OpenDML files over 1 GB): it shows any frame in a
preview with a frame slider and an info block, and opens the selected frame as an ordinary 32-bit PixInsight
image, debayered (bilinear) or as the raw CFA mosaic. For an AVI you pick the Bayer pattern (or let the ASIAIR
sidecar decide); for an AVI with no sidecar, Save pattern... remembers the pattern you chose in a sidecar it writes next
to the video, and Save as SER... writes the AVI out as a SER file.

**PlanetaryStack** stacks such a video into one image (lucky imaging), with
nothing to set: it measures every frame, chooses the sharpest frames from the capture's own statistics, aligns them
globally and region by region, and stacks them with outlier rejection; Stack! shows a progress bar and Cancel, the stack
is previewed with zoom, and Execute opens it as a linear 32-bit image with FITS keywords. A capture with nothing usable
gives no image but a diagnosis (no target, target leaves the frame, clipped, out of focus, poor seeing, frames that do
not line up) with the numbers behind it. The engine is `src/stack/` (PCL-free, no OpenMP).

Both read only the video file you choose (and an AVI's `.txt` sidecar). SERViewer writes only the SER file you choose;
either writes, if you click Save pattern..., the `.txt` sidecar next to the AVI. Neither uses the network or runs external
programs.

**Status:** released: SERViewer, 0.2.2.0 (it reads a Flip=Vert(unimplemented) AVI with its sidecar's Bayer pattern as is; 0.2.0.0-0.2.1.0 rotated it
180 degrees, RGGB to BGGR; see the SERViewer doc); 0.2.1.0 added Save pattern... (a generated sidecar for an AVI that has none); 0.2.0.0 added AVI input and
Save as SER (0.1.0.0 was released 2026-10-21). PlanetaryStack is not released: the source version stays 0.2.2.0 until a
release is approved, and `release.sh` refuses to reuse the published 0.2.2.0 package (it lacks the PlanetaryStack docs), so
that release needs a version bump. The module id `Planetary` and process id `SERViewer` are
permanent: existing installs update by these ids, so never rename them; `PlanetaryStack` becomes permanent at its first release.

**User documentation** (installed with the module as `doc/tools/<process>/<process>.html`):
[`doc/SERViewer/SERViewer.html`](doc/SERViewer/SERViewer.html) and
[`doc/PlanetaryStack/PlanetaryStack.html`](doc/PlanetaryStack/PlanetaryStack.html). Third-party notices (the same file
in each doc folder): [`doc/SERViewer/THIRD_PARTY_NOTICES.txt`](doc/SERViewer/THIRD_PARTY_NOTICES.txt). The update
repository's description of the package is [`package-description.xml`](package-description.xml); `release.sh`
writes it into `updates.xri` on every release.

This product is based on software from the PixInsight project, developed by Pleiades Astrophoto and its contributors (https://pixinsight.com/).

## Layout

| Path | What |
|---|---|
| `src/ser/` | SER and AVI readers, SER writer, debayer library (`planetary_ser`). PCL-free, unit-tested on its own. |
| `src/stack/` | The PlanetaryStack engine (`planetary_stack`): measure, select, align, regions, stack. PCL-free, no OpenMP, unit-tested on its own. |
| `src/module/` | The PixInsight module: SERViewer and PlanetaryStack processes, instances, interfaces, parameters; `PlanetaryVersion.h`. |
| `test/unit/` | Catch2 unit tests (SER and AVI readers, sidecar, Save as SER, debayer, FITS text, preview, the stack engine, stack keywords). |
| `test/` | Headless probes, GUI smokes and real-data harnesses (PixInsight only through `tools/pi-headless.sh`); test-only CLIs (`stack-probe`, `stack-region-probe`, `focus-survey`, ...). |
| `test/bench/` | The PlanetaryStack benchmark against AutoStakkert / ASIStudio results (host-only; see its README). |
| `doc/SERViewer/`, `doc/PlanetaryStack/` | Shipped user documentation and third-party notices. |

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
bash test/run-probe-stack.sh              # headless PlanetaryStack.executeGlobal() over synthetic captures vs stack-probe, slot 96
bash test/gui-smoke.sh <out-dir>          # SERViewer: real GUI on a private Xvfb, XTest-driven, slot 97
bash test/gui-smoke-stack.sh <out-dir>    # PlanetaryStack: the same, Stack! / progress / Cancel / zoom / Execute, slot 97
bash test/run-real-data.sh                # 2026-09-07 Saturn capture (NAS) -> SER fixtures -> probe + GUI smoke
bash test/run-avi-real.sh                 # every NAS AVI: frames vs ffmpeg, open time, Bayer phase, >1 GB round trip (host-only)
bash test/run-stack-bench.sh              # PlanetaryStack on the NAS benchmark set vs AS / ASI (host-only; test/bench/README.md)
bash test/run-focus-survey.sh             # the focus survey behind the focus limits (host-only, NAS)
```

The probe, GUI-smoke and Saturn real-data scripts sign the module and need `/tmp/.pi_codesign_pass`; without it they print BLOCKED and exit 2.
`PLANETARY_SO` / `PLANETARY_MAKE_FIXTURES` (and `PLANETARY_STACK_PROBE` for the two stack scripts) point them at another
build, e.g. the container build in `build-portable/`. The run-stack-bench and focus-survey scripts read the NAS only and
never launch PixInsight.

## Roadmap

Stacker (this branch: quality ranking, alignment, lucky selection) → live sliders on the stacked image → derotation.
