# NukeX v5.1.0.0 — Color Science Overhaul: Human Testing Run-Book

Branch: `nukex-color-science-overhaul`. **Not released.** This is a dev build for you to
validate on real data before it's signed and distributed.

## What changed
Replaces v4's hardcoded OSC→RGB Bayer routing with a filter-taxonomy Phase-A router +
Phase-B Q-matrix decomposition + Lab/LCH ColorComposer with a calibrated emission-line
palette. Fixes the **M27-green dual-narrowband bug** (dual-band HaO3/S2O3 on an OSC sensor
was mis-colored green) and adds OSC-as-LRGB synthesis. Ships a 55-camera / 87-filter QE
database (`share/qe_database.json`).

## What's already verified (automated — no eyeball needed)
Independently re-run on a clean build at branch HEAD:
- Default `ctest`: **68/68**.
- `[integration]` Phase-A router **6/6**, Phase-B Q-solve **5/5** (incl. mixed mono+Bayer geometry).
- `test_gpu_agreement`: GPU == CPU **byte-identical** (`max_val_diff=0`), including heterogeneous
  per-channel-remap cases on the RTX 5070 Ti OpenCL backend.

During validation we found and fixed 4 real defects the old (never-run) integration tests had
masked: a silent-zero on <3-frame voxels (CPU+GPU), a wrong-Bayer OOB on mixed batches, an
architectural per-voxel-vs-per-channel frame-accounting flaw (cross-channel aliasing + OOB on
heterogeneous batches), and a GPU frame-constant buffer under-sizing. All fixed + regression-tested.

## What needs YOUR eye (the part automation can't judge)
Whether the **dual-narrowband OSC output looks right** — specifically **no green cast**.

## 1. Build + load into PixInsight (for testing — do NOT `make install`)
```bash
cd modules/nukex
cmake -B build -DPCLDIR=$HOME/PCL -DNUKEX_BUILD_MODULE=ON && cmake --build build -j$(nproc)
# module: build/src/module/NukeX-pxm.so  (version 5.1.0.0)
```
Load via PixInsight → **Process › Modules › Install Modules** → add the directory
`modules/nukex/build/src/module/` → restart PixInsight. (Or your usual local-module method.
Do NOT `sudo make install` — it conflicts with your production NukeX per project rules.)

**QE database location (important):** the module looks for `share/qe_database.json` next to the
installed module and logs a loud warning if it's missing. For testing, make `modules/nukex/share/`
findable — copy `modules/nukex/share/qe_database.json` alongside the loaded `.so`, or set the QE
override path in the NukeX interface's file picker to point at it.

## 2. Real-data validation

### A. THE headline case — dual-NB-OSC, M27-green fix (needs your eyeball)
Real dual-band data confirmed on the NAS (`BAYERPAT=RGGB`, ZWO ASI2400MC Pro OSC):
- **HaO3:** `/mnt/qnap/astro_data/9_11_2023/NGC281W/` (the `*_HaO3_*.fit` frames)
- **S2O3:** same dir (the `*_S2O3_*.fit` frames)

Run NukeX on the NGC281W HaO3 set. **Pass bar:** no green cast; calibrated red (Ha) + cyan (OIII)
distribution. This is the exact scenario old v4 turned green. (There's also raw dual-NB in
`2_4_2024/NGC2264_*` and `2_4_2023/M42_*` if you want more targets.)

### B. Mono-LRGB regression — same-geometry, must be clean
`/mnt/qnap/astro_data/NGC7635/L/Lights` (65 frames). Same-geometry output is guaranteed
byte-identical to pre-overhaul by the GPU/CPU parity gate — this is a "did I break the common
path" check. Should look like a normal luminance stack.

## 3. E2E golden fingerprints (establishes the regression floor on real pixels)
The e2e harness runs through PixInsight against the loaded module:
```bash
cd build
make e2e-regen     # first time: writes goldens for the 4 LIVE cases (3 stretch sweeps + mono_lrgb_ngc7635_v5)
make e2e           # thereafter: regression check (fails loud on any pixel-hash drift)
```
The manifest (`test/fixtures/e2e_manifest.json`) has 4 LIVE cases (all on the NGC7635/L corpus)
and 4 SKIP cases whose corpora aren't on this machine (NGC7635-OSC, M27-HaO3, S2O3, LRGBSHO) —
capture those later to extend coverage.

## 4. If it looks right → ship it
Then the remaining release steps (deferred until you approve the visuals):
- Version + CHANGELOG finalize, package `share/qe_database.json` into the NukeX tarball, sign,
  update `repository/updates.xri`, run `./release.sh` (project release rules).
- Memory/CHANGELOG bookkeeping.

## Known limitations / follow-ups (non-blocking)
- Real dual-NB **S2O3** and **LRGBSHO** e2e goldens deferred (corpus capture, or point the skip
  cases at NGC281W S2O3).
- Carried minor code-review items (e.g. rating-DB migration-failure should log distinctly vs
  corruption; a per-voxel `read_pixel_dense` copy on the Phase-B hot path is a throughput nit) —
  tracked for the final review triage, none affect correctness.
