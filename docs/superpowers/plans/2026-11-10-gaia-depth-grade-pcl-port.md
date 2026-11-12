# Gaia Depth Grade PCL Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Gaia Depth Grade's Python sidecar and PJSR scripts with one native PCL module, `GaiaDepthGrade-pxm.so`, that does everything they do today. That covers plate solving, the star split, the local and online Gaia catalogues, the star depth grade, the nebula grade, preview and execute. It must lose no functionality, match the Python core numerically, and ship by 2026-12-12.

**Architecture:** A PCL-free C++ library, `gdg_core`, ports the Python numerics line for line. It works in the Python core's own pixel frame: rows in FITS bottom-up order, pixel centres on integers. Catch2 tests compare it against goldens dumped from the Python core. A thin PCL layer in `src/module/` adapts PixInsight to that core:
- image buffers, through `ImageBridge` (PixInsight row order, see the frame contract);
- the astrometric solution, through an `AstrometricMetadata` sky adapter;
- the local Gaia catalogue (`GaiaDatabaseFile`) and the online Gaia catalogue (`NetworkTransfer` TAP client);
- StarXTerminator and StarAlignment, through `ProcessInstance`;
- a native plate solver (StarDetector, Gaia, StarAlignment, spline fit, and `BlindAstrometricSolver` as the fallback);
- the `GaiaDepthGrade` process and its interface.

The recon core (plan tasks 1–8 of `2026-11-05-nebula-3d-depth-reconstruction.md`) is ported into `gdg_core/recon` with the same golden discipline, and no UI. That plan's tasks 9–13 (2D render, `reconstruct` CLI, GLB export, orbit animation, NAS validation) were never written in Python; Scott ruled on 2026-11-12 that they are built only in native C++, here, as Tasks 21–26 (Phase G2), with no Python or PJSR in what ships or in their tests.

**Tech Stack:**
- **C++20**: PCL 2.11 and 2.13 build with `-std=c++20` [verified, both `src/pcl/linux/g++/makefile-x64`].
- PCL **2.13.0 build 1072**: the sources bundled with PixInsight 1.9.6, the only PCL that has `BlindAstrometricSolver`. See "PCL pin" below.
- Rocky 9 build container (`tools/build-env`).
- Catch2: NukeX's amalgamated copy.
- Python 3.14 golden generator in the existing `gaia-depth-grade/.venv` (test-only).
- `tools/pi-headless.sh` for in-PI probes.

**Spec:**
- `docs/superpowers/specs/2026-11-10-gaia-depth-grade-pcl-port-design.md`. Read the **Addendum at the end first**: it supersedes sections 2, 3 (S2) and 6.
- Recon carry-over: `docs/superpowers/specs/2026-11-05-nebula-3d-depth-reconstruction-design.md` and `docs/superpowers/plans/2026-11-05-nebula-3d-depth-reconstruction.md`.

## Global Constraints

**Scope**
- No functionality is lost (Scott's ruling). Every row of the Feature Parity Checklist below must be green before the cut-over (Task 20).
- No shipped Python, no shipped JS/PJSR, no external program launched by the module. PJSR probe scripts under `modules/gaia-depth-grade/test/` are test tooling and are never packaged.
- Never modify the user's master. Prepare works on a duplicate `<targetId>_gdgwork`, as `duplicateWindow()` does today.
- Online mode talks only to `https://gea.esac.esa.int/` over HTTPS with peer and host verification (`SetSSL(true, true, true, true)`). Its URL list is documented under CPD 6.1. The offline default makes no network connection.

**Parity and numerics**
- Bit-for-bit decision parity with the Python core at the commit recorded in `test/golden/MANIFEST.json`. Tolerances are set per task; the defaults are float64 paths ≤ 1e-9 relative, and image outputs ≤ 1e-6 absolute on [0,1] data.
- Defaults are copied verbatim from the sources:
  - **Dialog** (`GaiaDepthGradeDialog.js`): Brightness 0.65, Size 0.85, Contrast 0.12, Saturation 0.4, Halo sigma 2.0, p_low 5.0, p_high 95.0, nebula off; Nebula depth 1.0, atmospheric 1.0, structure 1.0; detect σ 6.0, match tolerance 4.0 px; Offline on; G limit 18.0.
  - **Engine** (`config.py`): `detect_fwhm` 3.0, `min_match_rate` 0.3, `neutral_strength` 0.0, `gaia_parallax_snr` 5.0, `halo_min_refs` 8, `halo_radius_sigma` 8.0, `halo_moffat_beta` 2.5, `halo_gain` 1.0. The online row cap is 600000 and online retries are 3.
- Threading uses `nukex::parallel_for_dynamic` semantics (copy `modules/nukex/src/lib/gpu/include/nukex/gpu/parallel_for.hpp` into `src/core/Parallel.h`). No OpenMP. All statistics are reentrant.
- `pcl::View` / `ImageWindow` handles are never held past a window close (memory `pcl-view-handle-use-after-free`). Run ASan before release.

**Build and release**
- The module is built only in the Rocky 9 container against PCL 2.13.0 (Task 2), and must pass `verify-portable.sh`. The minimum PixInsight is **1.9.6**, published in its own `<platform os="linux" arch="x64" version="1.9.6:2.0.0">` block.
- IDs are stable once shipped:
  - module id `GaiaDepthGrade`;
  - process id `GaiaDepthGrade`;
  - process category `scarter4work` (the script's menu path is `scarter4work > Gaia Depth Grade`);
  - the parameter ids listed in Task 16.
- Copy rule: the stellar layer is *measured* (Gaia), and the nebula layer is *structure-driven, Gaia-calibrated*, never "measured". The honesty tag text is `GAIA depth-derived; physically motivated, not per-pixel correct for gas` (verbatim from `cli.HONESTY`).

## PCL pin: which release has BlindAstrometricSolver (evidence, 2026-11-10)

- **PCL 2.13.0 only.**

  | Source | PCL version | `BlindAstrometricSolver.h` / `AstrometricIndex*` |
  |---|---|---|
  | `~/PCL` (rewritten today to gitlab master) | 2.11.0 build 1070 | absent [verified `ls`] |
  | gitlab `pixinsight/PCL` master `5a39021` (2026-09-28, "PixInsight 1.9.5 / PCL 2.11.0") | 2.11.0 | absent from the whole tree [verified `git ls-tree`] |
  | Container pin `3a4af80` (2025-05-09) | older | absent |
  | PixInsight 1.9.6 bundle: `/opt/PixInsight/include/pcl` (released 2026-10-28) and `/opt/PixInsight/src/pcl` | 2.13.0 build 1072 [verified `Version.cpp`] | present: `BlindAstrometricSolver.cpp` 1,016 LOC, `AstrometricIndex.cpp`, `AstrometricIndexFile.cpp`, `AstrometricIndexGenerator.cpp` [verified] |

- **There is a matching libPCL, if we build it.** Upstream has not published 2.13, but the 1.9.6 bundle carries the **full buildable PCL 2.13 source tree**: `src/pcl/linux/g++/{Makefile,makefile-x64}` with `-std=c++20`, and `src/3rdparty/{cminpack,lcms,lz4,RFC6234,zlib,zstd}` [verified]. The container can build `libPCL-pxi.a` 2.13 from it with the same make loop it already uses for the pinned commit (Containerfile l.78–96).
- **A module built against 2.11 or 2.9.4 cannot cleanly use the solver.**
  - Its sources need 2.13-only headers (`FileMapping.h`, absent from 2.11) and a changed `StarDatabaseFile.h` [verified include list and diff].
  - Vendoring them beside an older libPCL would put two different `StarDatabaseFile` / `GaiaDatabaseFile` layouts in one binary. That is an ODR violation [inferred, not tried], so it is ruled out.
  - The solver sources make 0 core-API calls (`api->`), and neither does `FileMapping.cpp` [verified]. So nothing ties them to a newer *core* beyond 1.9.6.
- **Consequences, carried into Task 2:**
  1. GaiaDepthGrade builds against a second PCL tree, `/opt/pcl-2.13`, built in the image from a sha256-pinned tarball of the 1.9.6 bundle. The tarball is made locally by `tools/build-env/pcl-2.13/make-tarball.sh` and is **not committed**; only its `SHA256SUMS` is.
  2. GaiaDepthGrade and gdg_core build with `CMAKE_CXX_STANDARD 20`.
  3. The toolchain is the image's gcc-toolset-14 (Containerfile l.23–29), which supports C++20.
  4. `verify-portable.sh` keeps its floor unchanged (`MAX_GLIBCXX=3.4.30`, GLIBC 2.34) and must pass on `GaiaDepthGrade-pxm.so`. gcc-toolset links newer libstdc++ pieces statically, so the dynamic GLIBCXX should stay at the floor [inferred]. Task 2 Step 4 proves it.
  5. NukeX and Planetary keep `PCL_COMMIT` (2.9.4). Moving the whole image to 2.13 waits for upstream to publish it (D4).
  6. Host builds of the module are not supported (`~/PCL` is 2.11 and has no solver). Host builds cover `gdg_core` and its tests only, which are PCL-free. In-PI probes use the container `.so`, as Planetary's `PLANETARY_SO=build-portable/...` does.

## Assumed decisions

These are Scott's rulings, or my recommendations where he has not ruled. A change to one hits the named tasks.

| # | Decision | Status | Tasks |
|---|---|---|---|
| D1 | Online Bailer-Jones mode kept, ported to NetworkTransfer | Scott | 13 |
| D2 | Module solves the image itself, natively; stock ImageSolver through `Console::ExecuteScriptOn` is a contingency only if the Task 15 gate fails, and only with Scott's OK | Scott (rec.) | 14, 15 |
| D3 | Minimum PI 1.9.6; separate platform block | rec. | 2, 20 |
| D4 | PCL 2.13 enters the image as a **second** tree `/opt/pcl-2.13`, from a sha256-pinned tarball of `/opt/PixInsight/{include,src}`; NukeX and Planetary keep `PCL_COMMIT` | rec. | 2 |
| D5 | Star detection = exact DAOFIND port (photutils 3.0.0), not `pcl::StarDetector` (the solver uses StarDetector, as ImageSolver does) | rec. | 6, 14 |
| D6 | Synthetic G→σ knot table kept for parity; switching to Gaia parallax-quality flags is a later, separate change | rec. | 12 |
| D7 | Recon core ported (tasks 1–8) with no process/UI; tasks 9–13 are built in C++ only, never Python | Scott (2026-11-12) | 18, 21–26 |
| D10 | Recon Part 2 ships as a second process, `NebulaReconstruct` (global, category `scarter4work`, id stable once shipped), in `GaiaDepthGrade-pxm`. Its command-line interface is the native form of the Python plan's `reconstruct`/`export3d` subcommands. No GUI interface in this plan (the Python plan's "Part 2" panel) | rec. | 25 |
| D11 | Orbit animation = own GIF89a encoder in `gdg_core` (one 256-colour median-cut palette, LZW); no new third-party dependency | rec. | 24 |
| D12 | In-PI tests of Tasks 25–26 launch `pi-headless.sh … -r=<one-line .scp the harness generates> --force-exit` and drive C++ through the probe's command-line interface; no new PJSR or Python, committed or generated | Scott (no scripts, 2026-11-12); `.scp` route rec. | 25, 26 |
| D13 | NAS validation masters do not exist (subs only); the freeze probe integrates them once, uncalibrated, from the 2023 subs | rec. | 26 |
| D8 | Python core moves to `archive/gaia-depth-grade-python/` at cut-over; goldens stay committed | rec. | 20 |
| D9 | Process defaults = dialog defaults; the headless script's engine defaults (0.5/0.4/0.3/0.3, online) are reproducible as parameter values | rec. | 16 |

## Corrections (found during execution)

| # | Task | Plan said | Evidence | Now |
|---|---|---|---|---|
| C1 | 11 | Core frame y = 0 is the FITS bottom row; `ImageBridge` flips; `PclSky` uses `y_pi = H − y − 0.5` | PixInsight saves FITS `ROWORDER = 'TOP-DOWN'` (headless save + astropy read, 2026-11-10); `writeWcsKeywords` mirrored the solution's bottom-up WCS (`GetWCSvalues` / PCL `ComputeWCSKeywords` use `F→I = (x−0.5, H+0.5−y)`) to match. Probe: flipped mapping off by up to 1080 px | Core y = PixInsight row; no flip; `y_pi = y + 0.5` |
| C2 | 11 | `sky_adapter_convention` ≤ 0.3 px in the central 50% | The exported linear WCS differs from the spline solution by up to 0.31 px in the central 50% (0.43 px whole field) on the Full master; the mean offset is 0.011 px | Convention criterion = mean offset ≤ 0.05 px; max ≤ 0.5 px, RMS ≤ 0.2 px (measured 0.31 / 0.14) |
| C3 | 11 | `PclSky(const AstrometricMetadata&, int height)` | With no flip the height is not needed | `PclSky(const AstrometricMetadata&)` |
| C4 | 11 | The probe's linear WCS comes from `solver-reference.js` / PJSR `AstrometricMetadata` | The NAS masters already carry ImageSolver 6.4/6.5 solutions; a `-r=` script including `pjsr/astrometry/AstrometricMetadata.js` (ES6 classes) never ran (no result written; the known ES6-class limit) | The C++ probe generates the WCS with `AstrometricMetadata::UpdateWCSKeywords(generate)` + `WCSKeywords::MirrorVertically` (the same formula and flip) |
| C5 | 11 | Matcher uses `pcl::KDTree` | `gdg_core` is PCL-free | A uniform grid searched within the tolerance (same pairs; tested against brute force) |
| C7 | 12 | `local_gaia_equals_gaia_process`: same (ra, dec, parx, magG) rows | 22405-row cone (NGC 7635, 0.75 deg, G ≤ 18): parallax, G, BP, RP, flags exact; ra/dec bit-identical in 22259 rows, 1 ulp apart (≤ 5.7e-14 deg) in 146: the core's build of the inline `XPSD::IndexTree::Unproject` and ours round differently | Pair on the stored catalogue values; positions to ≤ 1e-12 deg |
| C8 | 12 | Discovery is a port of `discoverGaiaDbPaths` | Its regex is unanchored, so `EDR3DatabaseFilePathNN` would be read as a DR3 key; it reads settings files in directory order | Keys anchored on `k="`, files in name order, XML entities decoded; the parsing is the PCL-free `gaia_db_paths_from_settings` (ctest) |
| C9 | 12 | "Cygnus-sized cone on `/opt/gia3`" | This box's DR3 slot points at the DR3SP files (`gdr3sp-1.0.0-*`), the base `gdr3-*` files are present but not configured | The probe uses whatever discovery returns (DR3SP here) |
| C10 | 13 | Core holds `Adql`/`TapText`; the retry/poll flow and the cache live in the module (`TapClient`, `OnlineCache`) | The Review Focus 3 failure modes need unit tests without PixInsight or the network | The flow is PCL-free `core/TapJob` over an `HttpTransport` interface (unit-tested with answers recorded live 2026-11-10 in `test/fixtures/tap/`, 3 KB); `core/OnlineCache` (POSIX); the module's `TapClient` is only the NetworkTransfer transport (+ `OnlineGaiaSource`) |
| C11 | 13 | Cache at `File::HomeDirectory()/.cache/GaiaDepthGrade/gaia/` | The product honoured `$XDG_CACHE_HOME` (gdgWorkDir); the CSV is what is fetched | `$XDG_CACHE_HOME` (absolute) or `~/.cache`, then `GaiaDepthGrade/gaia/gaia_<key>.csv`, written atomically 0600; owner = the effective uid (the PJSR used the home directory's owner, having no getuid) |
| C12 | 13 | `parse_tap_csv`: empty means NaN | The archive's CSV prints REAL columns (r_*_geo, bp_rp) with float precision (`575.11194`) | REAL columns parse as float and widen (the values Python's VOTable path had); `null`/`NaN` also NaN |
| C13 | 13 | — | Live: the job ran ~25 s for 5 rows; `results/result` answers HTTP 500 while the job is EXECUTING; the 303 body carries `Location:`; notices answer 404 | Poll `phase` first (1, 2, 4, 8, 8 … s, first poll right after the POST); a non-CSV 200 is retried |
| C14 | 13 → 17 | — | `TapClient` uses `NetworkTransfer` with a `Control` receiver (GUI-thread objects); verified from `ExecuteGlobal` only | Task 17's worker-thread Prepare must run the online fetch where a Control can live, or prove NetworkTransfer off the GUI thread |
| C6 | 11 | In-PI probes call module adapters | A module with no processes cannot be reached from PJSR | Test-only `GaiaDepthGradeProbe` process, built only with `-DGDG_BUILD_PROBES=ON`; `build-modules.sh` fails if it is in the shipped `.so` |
| C15 | 14 | Core `parse_seed(keywords, properties)` with its own sexagesimal parsing | PCL ships the parser ImageSolver's `ExtractMetadata` mirrors (`pcl::WCSKeywords`: OBJCTRA/OBJCTDEC, FOCALLEN, XPIXSZ, DATE-OBS/END, RADESYS) | Module reads `pcl::WCSKeywords`; core keeps the arithmetic (`seed_from_metadata`, `seed_is_valid` = `hasValidSeed`, `observation_jd`, `resolution_from_focal`); seed precedence: existing solution, then linear WCS keywords, then acquisition metadata |
| C16 | 14 | Render the projected catalogue as a synthetic reference window for StarAlignment | ImageSolver 6.6 (`registerWithTemplate`, l.376) passes two CSV star lists: `referenceIsFile` + `targetCSVFilePath`; no window is rendered | Same: CSVs in a `mkdtemp` 0700 directory, removed after the solve |
| C17 | 14 | StarAlignment `outputData_*` constants; enum ids "recorded by Task 2" | Introspection (`Process::Parameters`/`TableColumns`, probe `introspect`): columns `totalPairMatches` (UInt32), `referenceStarX/Y`, `targetStarX/Y` (Block: packed float64). Core processes publish no enum element ids (API error); `StarAlignment.prototype` gives OutputMatrix 8, NoIntersection 0, Always 2. String parameters set with the default row index are refused | Read columns by id; enum values hardcoded in `SolverAlign.h` and checked each run by probe `solver_enums`; every `SetParameterValue` passes row 0 |
| C18 | 14 | `fit_affine` normal equations + `pcl::LevenbergMarquardt` TAN refinement, iterate twice | ImageSolver fits a projective `Math.homography` (= `pcl::Homography`, DLT) or a `SplineWorldTransformation`, re-matching until convergence (delta < 0.005 px and RMS change < 0.01, or 5 iterations without improvement) | Port of that loop; `fit_affine`/gnomonic helpers dropped from the core (unused); the linear mode writes the affine part of the homography, as ImageSolver serializes it |
| C19 | 14 | Catalogue positions = `LocalGaiaSource` rows | ImageSolver's Gaia XPSD catalogue excludes `NoPM` sources and computes astrometric places at the observation time from the J2016.0 epoch (proper motion, parallax) | Same, with `pcl::Position::Astrometric`; mutating the epoch to J2000 fails `solve_spline_residuals` |
| C20 | 15 | Index path from `Settings::ReadGlobal` or the core settings file | The Gaia process exposes `astrometricIndexFilePath` through `get-info` (ImageSolver's `astrometricIndexFilePath()`); `configure` sets it | Query Gaia `get-info` through `ProcessInstance`; the probe selects the index in the test slot's Gaia preferences only. The index used is Pleiades' `gdr3-idx-1.0.0-g17.xpsd`, downloaded by Scott from pixinsight.com/dist on 2026-10-30 (browser history) |
| C21 | 14 → 16 | Online TAP positional cone when no local DB exists | Scott's Task 14 brief names the local Gaia catalogue only | Not in `SolveImage`; without a local DR3 the solver fails with `DiscoverGaiaDatabases`' error. Decide in Task 16 with the catalogue-mode parameter |
| C22 | 14 | — | ImageSolver retries the alignment assuming apparent coordinates (`mightBeApparent`, no RADESYS) and offers exhaustive matching (off by default) | Not ported: neither path ran on the gate masters. Listed for the Task 19 parity checklist |

## Review Focus

1. **Unsolvable input.** The image has no RA/Dec/focal/pixel-size metadata and no astrometric index is configured. Expect one error that names both remedies (add metadata, or select an index in Gaia preferences); no silent fallback. Test: Task 15 `solve_fails_loudly_without_seed_or_index`.
2. **Mono target.** A one-channel master must run end to end: no tint or saturation steps, and no nebula colour tilt (the Python `ndim==2` branches). Tests: Tasks 9, 10 and 19 (`mono_*`).
3. **Online failure modes.** The TAP job reaches `ERROR`/`ABORTED`, the POST answers non-303, the 303 body has no `Location:` line, or the Location points at another host. Expect a retry up to 3 times, then the error `Gaia TAP query failed: Gaia async query failed after 3 attempts: <last>`. Never treat an empty table as success. Test: Task 13 `tap_*` cases.
4. **Windows closed mid-session.** The user closes the target, or the `_gdgwork` window, between Prepare and Execute. Expect a clear "target was closed, Prepare again" error and no crash or stale handle. Test: Task 17 GUI smoke step `close_target_then_execute`.
5. **StarXTerminator absent.** Expect the message "StarXTerminator is required (a paid PixInsight module) but is not installed…" when split mode is StarXTerminator, and supplied stars/starless views must work without it. Tests: Task 16 `split_requires_sxt_message` and `supplied_views_mode`.

---

## File structure

```
modules/gaia-depth-grade/
  CMakeLists.txt                  # options GDG_BUILD_MODULE (OFF), GDG_BUILD_TESTS (ON); mirrors modules/planetary/CMakeLists.txt
  cmake/PCLConfig.cmake           # copy of planetary's; PCLDIR default /opt/pcl-2.13
  package-description.xml
  doc/GaiaDepthGrade/GaiaDepthGrade.html
  src/core/                       # gdg_core: PCL-free, Python frame, double precision
    Image.h  Parallel.h  Log.h  Config.h
    Numerics.{h,cpp}              # gaussian_filter, percentile, median, mad, interp, nanmedian
    SigmaClip.{h,cpp}  Morphology.{h,cpp}
    DaoFind.{h,cpp}  Detect.{h,cpp}
    Sky.h  Footprint.{h,cpp}  Match.{h,cpp}
    Catalog.h  Distances.{h,cpp}  Adql.{h,cpp}  TapText.{h,cpp}
    Transform.{h,cpp}  Modulate.h  HaloProfile.{h,cpp}  RenderStars.{h,cpp}
    Nebula.{h,cpp}  Display.{h,cpp}  Pipeline.{h,cpp}
    SolveMath.{h,cpp}             # seed parsing, gnomonic projection, affine LSQ
    recon/ Model.h Channels.{h,cpp} Ionization.{h,cpp} Ionizer.{h,cpp} Extinction.{h,cpp} Cavity.{h,cpp}
    recon/ Render2D Reconstruct Glb Orbit Validate .{h,cpp}   # Phase G2 (Tasks 21-26)
    Gif.{h,cpp}                   # GIF89a writer (Task 24)
  src/module/                     # PCL glue
    NebulaReconstruct{Process,Parameters,Instance}.{h,cpp}  ReconBridge.{h,cpp}   # Task 25
    GaiaDepthGradeModule.{h,cpp}  GaiaDepthGradeVersion.h  pixinsight-module.map
    GaiaDepthGradeProcess.{h,cpp} GaiaDepthGradeParameters.{h,cpp} GaiaDepthGradeInstance.{h,cpp} GaiaDepthGradeInterface.{h,cpp}
    ImageBridge.{h,cpp}  PclSky.{h,cpp}  LocalGaia.{h,cpp}  TapClient.{h,cpp}  OnlineCache.{h,cpp}
    StarSplit.{h,cpp}  Solver.{h,cpp}  SolverAlign.{h,cpp}  SolverBlind.{h,cpp}
  test/
    CMakeLists.txt  util/Npy.h  util/TanSky.h  unit/test_*.cpp
    golden/make_goldens.py  golden/MANIFEST.json  golden/synthetic/*.npy|*.json
    run-real-parity.sh  run-probe.sh  gui-smoke.sh  probe-*.js  solver-reference.js
```

**Frame contract (load-bearing; corrected in Task 11, see Corrections C1).**
- `gdg::Image` stores `data[(y*width + x)*channels + c]`, with **y = 0 PixInsight's top row**. That is exactly the numpy array the Python core sees after `astropy.io.fits` reads a PixInsight-saved FITS: PixInsight writes FITS with `ROWORDER = 'TOP-DOWN'` by default, so `data[0]` is the top row (measured 2026-11-10).
- `ImageBridge` copies rows in PixInsight order (no flip).
- `PclSky` converts core pixel coordinates to PixInsight image coordinates as `x_pi = x + 0.5`, `y_pi = y + 0.5`. **Proven** in Task 11 (probe `sky_adapter_convention`: mean offset 0.011 px against the exported WCS; the bottom-up mapping `y_pi = H − y − 0.5` is off by up to 1080 px).
- This keeps star order, and so the overlapping-window render order, identical to Python.

---

## Phase A: Foundation

### Task 1: Golden generator and synthetic goldens

**Files:**
- Create: `modules/gaia-depth-grade/test/golden/make_goldens.py`, `test/golden/MANIFEST.json`, `test/golden/synthetic/` (generated, committed, ≤ 5 MB total)
- Create: `modules/gaia-depth-grade/test/util/Npy.h` (read little-endian `<f8`/`<f4`/`<i8`/`|b1` C-order `.npy` v1/v2), `test/unit/test_npy.cpp`

**Interfaces:**
- Produces: `make_goldens.py synthetic <outdir>` and `make_goldens.py real <stars.fits> <starless.fits> <catalog.tsv|online.ecsv> <outdir>`.
  - It runs under `gaia-depth-grade/.venv/bin/python -I` and puts `gaia-depth-grade/src` on `sys.path` explicitly.
  - It writes one `.npy` per stage plus `stage.json` (scalars and parameters).
  - It records `git rev-parse HEAD`, the numpy/scipy/astropy/photutils versions and the generator arguments in `MANIFEST.json`.
- Produces: `gdg::test::NpyArray { std::vector<size_t> shape; std::vector<double> data; }` and `NpyArray load_npy(const std::string& path)`.
- **Stages dumped:**
  - `sigma_clip` (mean, median, std);
  - `dao_table` (x, y, flux, peak, sharpness, roundness1, roundness2);
  - `footprint`;
  - `catalog` (with bp_rp);
  - `catalog_xy` (astropy `world_to_pixel_values`);
  - `match_table` and `match_stats`;
  - `strength`, `confidence`, `effective`;
  - `modulation`;
  - `halo_profile` (r_grid, values, is_empirical);
  - `rendered_stars`;
  - `budget`, `structural_depth`, `nebula_out` for strength/atmos/structure ∈ {(1,1,1), (2,0.5,1.5)};
  - `screen_blend`, `autostretch_u8`;
  - the primitive goldens `gauss_{sigma}` for σ ∈ {0.8, 1, 2, 2.5, 4, 65.0}, `percentile`, `grey_closing_15`, `interp`;
  - for recon, every function output in `recon/*` on the plan's synthetic blister fixture.
- **Synthetic inputs:**
  - the `simple_wcs_header` fixture from `gaia-depth-grade/tests/conftest.py`;
  - a 512×384 colour star field (seeded `np.random.default_rng(20261110)`, 300 Gaussian stars, 12 bright isolated stars for the empirical halo, sky noise σ = 0.002);
  - a mono copy;
  - a starless field with dark lanes;
  - a 64×48 odd-size image for edge cases.

- [ ] **Step 1: Write `test_npy.cpp`.** `load_npy` round-trips `golden/synthetic/npy_selftest_f8.npy` (shape (3,2), values 0..5) and `npy_selftest_b1.npy`; a truncated file throws `std::runtime_error`.
- [ ] **Step 2: Run** `ctest -R test_npy`. Expected: FAIL (no files yet).
- [ ] **Step 3: Implement `make_goldens.py`** (stage dumps above; it fails loudly if any stage raises) and `Npy.h`. Generate `synthetic/`.
- [ ] **Step 4: Run** `gaia-depth-grade/.venv/bin/python -I modules/gaia-depth-grade/test/golden/make_goldens.py synthetic modules/gaia-depth-grade/test/golden/synthetic`, then `ctest -R test_npy`. Expected: PASS. `MANIFEST.json` lists the stages and the Python commit.
- [ ] **Step 5: Commit** `test(gdg): golden generator + synthetic goldens from the Python core`.

### Task 2: Module skeleton, PCL 2.13 in the image, build/release wiring, id probe

**Files:**
- Create: `modules/gaia-depth-grade/CMakeLists.txt`, `cmake/PCLConfig.cmake`, `src/module/GaiaDepthGradeModule.{h,cpp}`, `GaiaDepthGradeVersion.h` (`GDG_MODULE_VERSION_*` = 1.0.0.0), `pixinsight-module.map`, `test/CMakeLists.txt` (`gdg_add_test(name src libs...)` modelled on `planetary_add_test`), `package-description.xml`
- Create: `tools/build-env/pcl-2.13/` with `make-tarball.sh` (tars `/opt/PixInsight/include/pcl` and `/opt/PixInsight/src/{pcl,3rdparty}` and writes `SHA256SUMS`)
- Modify: `tools/build-env/Containerfile` (new stage building `/opt/pcl-2.13` from the tarball with the same make loop and `__isoc23` check as the existing PCL step, l.78–96), `tools/build-env/image-tag.sh` (hash the tarball), `tools/build-env/build-modules.sh` (configure, build, ctest and `.so` check for `modules/gaia-depth-grade` with `-DPCLDIR=/opt/pcl-2.13`), `tools/build-env/verify-portable.sh` (include `GaiaDepthGrade-pxm.so`)
- Modify: `release.sh` at every hand-listed module point (spec §4 "Wiring" list), registering the package and the new platform block. The sidecar/script removal is Task 20.
- Create: `modules/gaia-depth-grade/test/probe-ids.js` and `test/run-probe-ids.sh` (headless through `tools/pi-headless.sh`, marker-file output, slot 95)

**Interfaces:**
- Produces: a loadable `GaiaDepthGrade-pxm.so` with module id `GaiaDepthGrade` and no processes yet. `gdg_add_test()` for every later task.
- Produces, from `probe-ids.js`, written to `build/probe-ids.json`:
  - `Process("StarXTerminator")` existence and its parameter ids (`stars`, `unscreen` expected);
  - `Gaia` parameter ids (`command`, `centerRA`, `centerDec`, `radius`, `magnitudeLow`, `magnitudeHigh`, `sourceLimit`, `databaseFilePaths`, `sources`, `dataRelease`);
  - `StarAlignment` ids used by ImageSolverEngine.js l.378–440 (`referenceImage`, `referenceIsFile`, `mode`, `intersection`, `outputData`, and the `outputData_*` column indices);
  - confirmation that no process with id `ImageSolver` or `BlindSolver` exists;
  - whether a scratch `updates.xri` with two linux-x64 `<platform>` blocks (1.8.0:2.0.0 and 1.9.6:2.0.0) is accepted by the 1.9.6 updater's validation.

- [ ] **Step 1: Write the failing checks.**
  - Add `test_module_loads` to `test/run-probe-ids.sh`: sign the container-built `.so`, load it with `-m=`, assert `probe-ids.json` has `"module":"GaiaDepthGrade"` and `"imageSolverProcess":false`.
  - Add `test_pcl213_solver_links` to `build-modules.sh`: `nm -C /opt/pcl-2.13/lib/x64/libPCL-pxi.a | grep -q 'pcl::BlindAstrometricSolver::Solve'`, plus a 10-line `src/module/PclProbe.cpp` that constructs `pcl::AstrometricIndexFile` and `pcl::BlindAstrometricSolver` and links into the module.
  - Add `verify-portable.sh` on `GaiaDepthGrade-pxm.so` (GLIBCXX ≤ 3.4.30, GLIBC ≤ 2.34, no RPATH, real dlopen on Ubuntu 22.04 / Debian 12 / Rocky 9).
- [ ] **Step 2: Run** `bash modules/gaia-depth-grade/test/run-probe-ids.sh`. Expected: FAIL (no module).
- [ ] **Step 3: Implement** the tarball, the image stage, the CMake files, the empty module and the release.sh wiring.
- [ ] **Step 4: Run** `tools/build-env/build-modules.sh` (all modules still build; NukeX and Planetary ctest still green), then `run-probe-ids.sh`. Expected: PASS. `probe-ids.json` records every id above. **Copy the ids into Task 16's parameter notes before starting Task 14.**
- [ ] **Step 5: Commit** `build(gdg): GaiaDepthGrade module skeleton on PCL 2.13 + id probe`.

## Phase B: Numerics (gdg_core)

### Task 3: scipy-exact Gaussian filter

**Files:** Create `src/core/Image.h`, `src/core/Parallel.h`, `src/core/Numerics.{h,cpp}` (Gaussian part), `test/unit/test_gaussian.cpp`

**Interfaces:**
- Produces: `struct gdg::Image { int width, height, channels; std::vector<double> data; double& at(int x, int y, int c = 0); double at(int x, int y, int c = 0) const; }`.
- Produces: `gdg::Image gdg::gaussian_filter(const gdg::Image& in, double sigma)`. It filters along y and x only; for colour images it is per channel, matching `axes=(0,1)`.
- **scipy semantics to reproduce:**
  - kernel radius `int(4.0*sigma + 0.5)`;
  - weights `exp(-0.5*(i/sigma)^2)` normalised to sum 1;
  - `mode='reflect'` (half-sample symmetric: `d c b a | a b c d`), including reflections longer than the axis;
  - correlation along axis 0, then axis 1, as `gaussian_filter1d` does.
- σ ≥ 20 may use an FFT path only if it stays within tolerance of the golden.

- [ ] **Step 1: Write failing tests** `gauss_matches_scipy_{0.8,1,2,2.5,4,65}` (max |Δ| ≤ 1e-12 on synthetic data, ≤ 1e-9 for σ = 65), `gauss_odd_tiny_image` (64×48, σ = 4, so the reflection exceeds the edge), and `gauss_colour_is_per_channel`.
- [ ] **Step 2: Run** `ctest -R test_gaussian`. Expected: FAIL.
- [ ] **Step 3: Implement** the separable direct convolution with `parallel_for_dynamic` over rows/columns.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): scipy-exact gaussian_filter`.

### Task 4: Order statistics and interpolation

**Files:** Modify `src/core/Numerics.{h,cpp}`; create `test/unit/test_orderstats.cpp`

**Interfaces:**
- Produces:
  - `double percentile(std::vector<double> v, double q)` (numpy default "linear" method: `(n-1)*q/100`, interpolated between neighbours; uses `nth_element`);
  - `std::vector<double> percentiles(std::vector<double> v, const std::vector<double>& qs)`;
  - `double median(std::vector<double> v)` (mean of the two middle values when n is even);
  - `double mad(const std::vector<double>& v)` (= `median(|v - median(v)|)`, unscaled; callers multiply by 1.4826 where Python does);
  - `std::vector<double> interp(const std::vector<double>& xq, const std::vector<double>& xp, const std::vector<double>& fp, double left, double right)` (np.interp, including the clamp defaults);
  - `std::vector<double> nanmedian_rows(const std::vector<std::vector<double>>& stack)`.
- NaN policy: `percentile`/`median` require finite input; callers filter first, exactly where Python does.

- [ ] **Step 1: Failing tests:** `percentile_matches_numpy` (q ∈ {0.1, 5, 10, 20, 60, 95, 99, 99.5, 99.9}), `median_even_odd`, `mad_matches`, `interp_left_right_clamp`, `nanmedian_ignores_nan`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS (exact equality for percentile/median on the goldens).
- [ ] **Step 5: Commit** `feat(gdg-core): numpy-exact percentile/median/mad/interp`.

### Task 5: Sigma clipping and grey closing

**Files:** Create `src/core/SigmaClip.{h,cpp}`, `src/core/Morphology.{h,cpp}`, `test/unit/test_sigmaclip.cpp`, `test/unit/test_morphology.cpp`

**Interfaces:**
- Produces: `struct gdg::ClipStats { double mean, median, std; }` and `ClipStats sigma_clipped_stats(const std::vector<double>& v, double sigma = 3.0, int maxiters = 5)`.
  - astropy 8.0 defaults: `cenfunc='median'`, `stdfunc='std'` with ddof = 0, symmetric bounds, `grow=False`.
  - Iterate until no change or maxiters.
  - Port the bound comparison from `.venv/.../astropy/stats/sigma_clipping.py` (`_sigmaclip_fast` / `_compute_bounds`), keeping its strictness.
- Produces: `gdg::Image grey_closing(const gdg::Image& mono, int size)`. A flat `size×size` square, dilation then erosion, scipy `mode='reflect'`, origin centred (for even sizes follow scipy's `origin=0` offset). Separable van Herk / Gil-Werman max/min is acceptable.

- [ ] **Step 1: Failing tests:** `sigma_clip_matches_astropy` (synthetic sky + stars, |Δ| ≤ 1e-12), `sigma_clip_converges_early`, `grey_closing_15_matches_scipy` (exact), `grey_closing_edges_reflect`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): astropy sigma clip + scipy grey closing`.

## Phase C: Star pipeline

### Task 6: DAOFIND port

**Files:** Create `src/core/DaoFind.{h,cpp}`, `src/core/Detect.{h,cpp}`, `test/unit/test_daofind.cpp`; add `THIRD_PARTY_NOTICES` (photutils BSD-3) to `modules/gaia-depth-grade/`

**Interfaces:**
- Produces: `struct gdg::DaoParams { double threshold; double fwhm; double ratio = 1.0, theta = 0.0, sigma_radius = 1.5; double sharp_lo = 0.2, sharp_hi = 1.0, round_lo = -1.0, round_hi = 1.0; double min_separation = -1.0 /* -1 => 2.5*fwhm */; bool exclude_border = false; bool scale_threshold = true; }`.
- Produces: `struct gdg::DaoStar { double x, y, flux, peak, sharpness, roundness1, roundness2; }` and `std::vector<DaoStar> dao_find(const gdg::Image& mono, const DaoParams&)`. Output order must equal photutils' row order.
- Produces: `struct gdg::StarRow { double x, y, flux; double r_med = NAN, r_lo = NAN, r_hi = NAN, bp_rp = NAN; bool matched = false; }` and `std::vector<StarRow> detect_stars(const gdg::Image& lum, double fwhm, double threshold_sigma)`.
  - `detect_stars` does `sigma_clipped_stats(sigma = 3)`, then `dao_find(lum - median, {threshold = threshold_sigma*std, fwhm})`.
  - `measure_fwhm` is **not** ported: its column is never read.
- **Port source:** photutils 3.0.0 in `gaia-depth-grade/.venv/lib/python3.14/site-packages/photutils/`. Port:
  - `detection/daofinder.py` (`DAOStarFinder._get_raw_catalog`, `_DAOStarFinderCatalog`);
  - `detection/core.py` (`_StarFinderKernel` l.714+, `_find_stars` l.75+);
  - `utils/_convolution.py` (`_filter_data`, `mode='constant'`, fill 0);
  - `detection/peakfinder.py` where `_find_stars` uses it.
  - `flux` = sum of the data inside the kernel footprint (photutils 3.0 definition, daofinder.py l.387).

- [ ] **Step 1: Failing tests:** `dao_matches_photutils_synthetic` (same count, same order, x/y ≤ 1e-9 px, flux/sharpness/roundness rel ≤ 1e-9), `dao_min_separation_default_is_2p5_fwhm`, `dao_rejects_by_sharpness_roundness`, `detect_empty_field_returns_empty`, `detect_mono_and_lum_of_colour`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** the port.
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): exact DAOStarFinder (photutils 3.0.0) port`.

### Task 7: Config, transform, modulation

**Files:** Create `src/core/Config.h`, `src/core/Transform.{h,cpp}`, `src/core/Modulate.h`, `src/core/Log.h`, `test/unit/test_transform.cpp`

**Interfaces:**
- Produces: `struct gdg::Gains { double brightness = 0.5, size = 0.4, contrast = 0.3, saturation = 0.3, halo_gain = 1.0; }`.
- Produces: `struct gdg::GradeConfig` with every `config.py` field and default: `p_low` 5, `p_high` 95, `match_tolerance_px` 4, `min_match_rate` 0.3, `detect_fwhm` 3, `detect_threshold_sigma` 6, `base_sigma_px` 2, `neutral_strength` 0, `std::optional<double> gaia_mag_limit = 18`, `gaia_parallax_snr` 5, `halo_min_refs` 8, `halo_radius_sigma` 8, `halo_moffat_beta` 2.5. No `cache_dir`; that is Task 13's.
- Produces: `struct gdg::Log { std::function<void(const std::string&)> info, warning; }`.
- Produces: `std::vector<double> depth_strength(const std::vector<double>& r_med, double p_low, double p_high, double neutral)`, `confidence(r_med, r_lo, r_hi)`, `effective_strength(r_med, r_lo, r_hi, p_low, p_high, neutral)`.
- Produces: `struct gdg::Modulation { std::vector<double> brightness, size, contrast, saturation; }` and `Modulation compute_modulation(const std::vector<double>& s, const Gains&)`.

- [ ] **Step 1: Failing tests:** `strength_matches_golden`, `strength_degenerate_spread_is_neutral` (hi ≤ lo → neutral), `confidence_nan_is_zero`, `modulation_matches_golden`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS (exact).
- [ ] **Step 5: Commit** `feat(gdg-core): config defaults, depth strength, modulation`.

### Task 8: Star-shine halo profile

**Files:** Create `src/core/HaloProfile.{h,cpp}`, `test/unit/test_halo.cpp`

**Interfaces:**
- Produces: `struct gdg::HaloProfile { std::vector<double> r_grid, values; bool is_empirical; std::vector<double> sample(const std::vector<double>& r) const; }`.
  - `sample` gives 0 beyond `r_grid.back()`; left uses `values[0]`.
- Produces: `HaloProfile moffat_profile(double base_sigma_px, double beta, double radius_px)`.
- Produces: `HaloProfile build_halo_profile(const gdg::Image& stars, const std::vector<StarRow>& table, double base_sigma_px, const GradeConfig&, const Log&)`.
- Constants, verbatim from `psf.py`:
  - saturation ceiling 0.9;
  - isolation radius `base_sigma*4`, neighbour cut `0.5*peak`;
  - stop after `max(halo_min_refs*3, halo_min_refs)` stamps;
  - flux-descending order with numpy `argsort(...)[::-1]` tie behaviour (stable argsort, reversed);
  - `np.minimum.accumulate` monotone clamp.
- Warning text, verbatim: `star-shine: only %d clean reference star(s) (need %d) — using a Moffat halo this run, not the image's own PSF`.

- [ ] **Step 1: Failing tests:** `halo_empirical_matches_golden` (r_grid/values ≤ 1e-12, `is_empirical`), `halo_falls_back_to_moffat_with_warning` (a field with 3 bright stars; warning captured through `Log`), `moffat_matches_golden`, `sample_zero_beyond_grid`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): empirical star-shine halo profile with Moffat fallback`.

### Task 9: Per-star render

**Files:** Create `src/core/RenderStars.{h,cpp}`, `test/unit/test_render.cpp`

**Interfaces:**
- Consumes: Tasks 3, 4, 7, 8.
- Produces: `gdg::Image render_stars(const gdg::Image& stars, const std::vector<StarRow>& table, const Modulation& m, double base_sigma_px, const HaloProfile* halo, double halo_gain)`.
  - When `halo` is null, use `moffat_profile(base_sigma_px, 2.5, 8*base_sigma_px)`.
- Behaviour, verbatim from `render.py`:
  - windows are processed **in table order** and mutate the accumulator;
  - window radius `ceil(4*base_sigma*max(1, size))`, clipped to the image as `_window_slice` does (`int(x)` truncation, not rounding);
  - Hann taper;
  - brightness feather;
  - one-sided halo only when `size > 1 + 1e-9`, peak from the core radius `max(base_sigma, 1)`, colour tint from `_core_color`;
  - noise-cored unsharp: the window is blurred with `gaussian_filter(win, base_sigma)`, so **reflection happens at the window crop edge**; coring at `0.75*1.4826*MAD`;
  - colour-only saturation;
  - final clip to [0,1].

- [ ] **Step 1: Failing tests:** `render_matches_golden_colour` (max |Δ| ≤ 1e-9), `render_matches_golden_mono` (Review Focus 2), `render_far_star_never_negative_ring` (size < 1 adds no halo), `render_edge_star_window_clipped`, `render_overlap_order_matches_python` (two overlapping stars; golden equality proves the order).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): per-star depth render (halo, feather, cored contrast, saturation)`.

### Task 10: Nebula grade, screen blend, preview pixels

**Files:** Create `src/core/Nebula.{h,cpp}`, `src/core/Display.{h,cpp}`, `test/unit/test_nebula.cpp`, `test/unit/test_display.cpp`

**Interfaces:**
- Produces:
  - `double gaia_depth_budget(const std::vector<double>& r_med, double ref_dex = 0.5)` (needs ≥ 20 finite values, else 0);
  - `gdg::Image structural_depth(const gdg::Image& lum, double frac = 0.03, double sky_pct = 20.0)`;
  - `gdg::Image render_nebula(const gdg::Image& starless, const std::vector<StarRow>& table, double strength, double atmos, double structure, std::optional<double> budget = std::nullopt)`.
  - Constants: 0.18, 0.15, 0.025 tilt (red ×(1+tilt), last channel ×(1−0.5·tilt)), structural pop 0.45 with σ = 2 blur and the same coring; return the input unchanged when `strength*budget ≤ 1e-6`.
- Produces:
  - `gdg::Image screen_blend(const gdg::Image& a, const gdg::Image& b)` (`1-(1-a)(1-b)`);
  - `std::vector<uint8_t> autostretch_u8(const gdg::Image& rgb, double target_bg = 0.25, double shadows_clip = -2.8)` (STF median/MAD/MTF, `(out*255+0.5)` truncation);
  - `gdg::Image crop(const gdg::Image&, int x, int y, int w, int h)`;
  - `std::vector<uint8_t> downscale_u8(const std::vector<uint8_t>& rgb, int w, int h, int channels, int max_width)` (box-average; display-only, so parity is not required against PIL).

- [ ] **Step 1: Failing tests:**
  - `budget_matches_golden`, `budget_zero_below_20_matches`;
  - `structural_depth_matches_golden` (σ = 0.03·min(h,w));
  - `nebula_matches_golden_{111,2_05_15}`;
  - `nebula_mono_has_no_colour_tilt` (Review Focus 2);
  - `nebula_zero_strength_identity`;
  - `screen_blend_matches`;
  - `autostretch_matches_golden` (exact u8);
  - `downscale_keeps_aspect_760`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): nebula grade, screen blend, STF preview pixels`.

## Phase D: Catalogue and astrometry

### Task 11: Sky abstraction, footprint, matcher, PCL sky adapter

**Files:** Create `src/core/Sky.h`, `src/core/Footprint.{h,cpp}`, `src/core/Catalog.h`, `src/core/Match.{h,cpp}`, `src/core/Pipeline.{h,cpp}` (prepare/render orchestration), `test/util/TanSky.h`, `test/unit/test_match.cpp`, `test/unit/test_pipeline.cpp`; `src/module/PclSky.{h,cpp}`, `src/module/ImageBridge.{h,cpp}`; probe `test/probe-sky.js` + `test/run-probe.sh` case `sky_adapter_convention`

**Interfaces:**
- Produces: `class gdg::SkyTransform { public: virtual ~SkyTransform(); virtual bool pixel_to_sky(double x, double y, double& ra, double& dec) const = 0; virtual bool sky_to_pixel(double ra, double dec, double& x, double& y) const = 0; }`, in core-frame pixels.
- Produces: `struct gdg::Footprint { double center_ra, center_dec, radius_deg; }` and `Footprint field_footprint(const SkyTransform&, int ny, int nx)`.
  - Centre `((nx-1)/2, (ny-1)/2)`; corners (0,0), (nx−1,0), (0,ny−1), (nx−1,ny−1); great-circle separation by the Vincenty formula, as astropy's `separation` uses.
- Produces: `struct gdg::CatalogRow { double ra, dec, r_med, r_lo, r_hi; long long source_id; double bp_rp = NAN; }`, `using Catalog = std::vector<CatalogRow>`, and `class CatalogSource { virtual Catalog distances_for(const Footprint&) = 0; }`.
- Produces: `struct gdg::MatchStats { int n_detected, n_matched; double match_rate, median_offset_px; }` and `std::vector<StarRow> cross_match(const std::vector<StarRow>& detected, const Catalog&, const SkyTransform&, double tol_px, MatchStats& stats)`.
  - Nearest detection per catalogue row (PCL 2.13 `KDTree::NearestNeighbor`, or brute force for n < 64).
  - Reject when d > tol.
  - Each detection keeps the catalogue row with strictly smaller d, in catalogue order (the dict semantics of `match.py`).
  - `median_offset_px` = median of the kept distances in dict-insertion order (order-independent anyway).
- Produces: `struct gdg::QA { int n_detected, n_matched; double match_rate, median_offset_px; bool low_match_warning; }`, `struct PrepResult { std::vector<StarRow> table; QA qa; }`, `PrepResult prepare_grade(const gdg::Image& stars, const SkyTransform&, const GradeConfig&, CatalogSource&, const Log&)` and `gdg::Image render_from_prep(const gdg::Image& stars, const PrepResult&, const GradeConfig&, const Log&)`.
  - Warning text: `LOW MATCH RATE %.2f < %.2f — depth grade is unreliable`.
- Produces (module):
  - `gdg::Image ImageBridge::ToCore(const pcl::ImageVariant&)` (PixInsight row order, normalised to double; C1);
  - `void ImageBridge::ToPcl(const gdg::Image&, pcl::Image&)`;
  - `class PclSky : public gdg::SkyTransform { explicit PclSky(const pcl::AstrometricMetadata&); }` with `x_pi = x + 0.5`, `y_pi = y + 0.5` (C1, C3).

- [ ] **Step 1: Failing tests:**
  - `footprint_matches_astropy` (TanSky built from the golden header, |Δ| ≤ 1e-10 deg);
  - `match_matches_golden` (fed golden `catalog_xy` through a lookup SkyTransform: identical matched set, r_* and bp_rp copied, stats exact);
  - `match_empty_inputs`;
  - `prepare_matches_golden_qa`;
  - `render_from_prep_matches_golden`.
  - Probe `sky_adapter_convention`: on a real master solved by ImageSolver (`solver-reference.js`, test-only), `PclSky.pixel_to_sky` agrees with the exported linear FITS WCS read by TanSky to ≤ 0.3 px over the field (the linear-vs-spline residual is ~2 px at the corners per `config.py`, so test inside the central 50%, where ≤ 0.3 px holds).
- [ ] **Step 2: Run** `ctest -R 'test_match|test_pipeline'` and `bash test/run-probe.sh sky_adapter_convention`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg): footprint, cross-match, prepare/render pipeline, PCL sky adapter`.

### Task 12: Local Gaia DR3 source

**Files:** Create `src/core/Distances.{h,cpp}`, `test/unit/test_distances.cpp`; `src/module/LocalGaia.{h,cpp}`; probe case `local_gaia_equals_gaia_process`

**Interfaces:**
- Produces (core):
  - `double sigma_parallax_from_g(double g)` (the knot table verbatim from `local_gaia.py`);
  - `Catalog parallax_to_distance_table(const std::vector<double>& ra, dec, plx, g, bp_rp, double snr_min, const Log&)`;
  - warning texts verbatim: `local Gaia: dropped %d/%d sources failing the parallax S/N >= %.1f cut …`, `local Gaia: magnitude cut G<%.1f excludes %d source(s).`, and the zero-usable error.
- Produces (module): `class LocalGaiaSource : public gdg::CatalogSource { LocalGaiaSource(pcl::StringList xpsdPaths, std::optional<double> magLimit, double snrMin, const gdg::Log&); }`.
  - It searches every file with `GaiaDatabaseFile::Search`: `magnitudeLow = -5`, `magnitudeHigh = magLimit`, `sourceLimit = 4294967295`, flags 0, matching `_searchGaia`.
  - `bp_rp = magBP − magRP`, NaN when the `NoGBPMag`/`NoGRPMag` flags are set. This fixes the dropped-colour bug.
- Produces (module): `pcl::StringList DiscoverGaiaDatabases()`.
  - Order: the module setting `GaiaDepthGrade/GaiaDatabaseFiles` when set; otherwise parse `~/.PixInsight/core-*-pxi.settings` for `DR3DatabaseFilePathNN`, then `DR3SPDatabaseFilePathNN` (port of `discoverGaiaDbPaths`).
  - Errors verbatim from `queryLocalGaiaDR3` when it finds none.

- [ ] **Step 1: Failing tests:** `knot_sigma_matches`, `parallax_table_matches_golden`, `snr_cut_logs_drop_count`, `zero_usable_throws`. Probe `local_gaia_equals_gaia_process`: for the Cygnus-sized cone on this box's `/opt/gia3`, the C++ source returns the same (ra, dec, parx, magG) rows as the Gaia process `search` (row multiset equality).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg): local Gaia DR3 source via GaiaDatabaseFile (+BP/RP)`.

### Task 13: Online Bailer-Jones source (ESA TAP over NetworkTransfer)

**Files:** Create `src/core/Adql.{h,cpp}`, `src/core/TapText.{h,cpp}`, `test/unit/test_tap.cpp`; `src/module/TapClient.{h,cpp}`, `src/module/OnlineCache.{h,cpp}`; probe case `online_live` (exits 77 when `GDG_LIVE_TAP` is unset)

**Interfaces:**
- Produces (core):
  - `std::string build_adql(const Footprint&, long top, std::optional<double> mag_gt, std::optional<double> mag_lt)`: byte-identical to `distances.build_adql`, using Python `repr`-style float formatting (port it explicitly: shortest round-trip, `%.17g` then trimmed to Python's repr);
  - `std::string parse_uws_location(const std::string& body)`: returns the URL from a line `Location: https://gea.esac.esa.int/tap-server/tap/async/<id>`, and throws on a missing line or a foreign host;
  - `Catalog parse_tap_csv(const std::string& csv, const Log&)`: columns `ra,dec,r_med_geo,r_lo_geo,r_hi_geo,source_id,phot_g_mean_mag,bp_rp`, empty means NaN; throws on missing required columns (`Gaia result missing columns: [...]`) and on zero rows (`Gaia query returned zero rows for this field`);
  - `std::string cache_key(const Footprint&, std::optional<double> mag)`: the `%.6f` key and the first 16 hex characters of SHA-1, as `_cache_path` (`pcl::RFC6234`-based SHA-1 or a local implementation).
- Produces (module): `class OnlineGaiaSource : public gdg::CatalogSource`.
  - **Steps:** fetch notices (GET `https://gea.esac.esa.int/tap-server/notification?action=GetNotifications`; any non-200 → console warning `Gaia archive notices unavailable (HTTP %d)` and continue); log the magnitude-cut warning verbatim; POST to `/tap-server/tap/async` with `REQUEST=doQuery&LANG=ADQL&FORMAT=csv&PHASE=RUN&QUERY=<urlencoded>` and expect 303 → `parse_uws_location`; poll `<job>/phase` (1 s doubling to 8 s, 20 min cap) until `COMPLETED`, or throw on `ERROR`/`ABORTED`; GET `<job>/results/result`.
  - **Failure handling:** 3 attempts in all, then `Gaia TAP query failed: Gaia async query failed after 3 attempts: <last>`; a row-cap (600000) warning verbatim.
  - **Cache:** `OnlineCache` stores the CSV under the module's private cache directory (`File::HomeDirectory() + "/.cache/GaiaDepthGrade/gaia/"`, mode 0700, with the symlink/owner checks ported from `gdgPrivateDir`). A cache hit logs `using cached Gaia result <path>`.
  - `TapClient` holds a `pcl::Control` subclass receiving `OnDownloadDataAvailable` (the handler must be a Control member).

- [ ] **Step 1: Failing tests:**
  - `adql_byte_identical_to_python` (goldens for mag_lt ∈ {None, 18.0}, mag_gt set);
  - `tap_location_parsed` (the probe body captured 2026-11-10);
  - `tap_location_foreign_host_rejected`, `tap_location_missing_rejected`;
  - `tap_csv_parses_with_empty_bp_rp`, `tap_csv_missing_column_throws`, `tap_csv_zero_rows_throws`;
  - `cache_key_matches_python`.
  - Probe `online_live`: a known small field returns ≥ 1 row and writes then reuses the cache.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R test_tap` and `GDG_LIVE_TAP=1 bash test/run-probe.sh online_live`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg): online Bailer-Jones source over ESA TAP (NetworkTransfer)`.

## Phase E: Native plate solver

Rationale and evidence: spec Addendum A2. The algorithm follows ImageSolver 6.6.0 (`/opt/PixInsight/src/scripts/ImageSolver/ImageSolverEngine.js`, PCL License 2.0). Ported files carry its copyright notice.

### Task 14: Seeded solve (metadata seed, catalogue, StarAlignment match, linear solution)

**Files:** Create `src/core/SolveMath.{h,cpp}`, `test/unit/test_solvemath.cpp`; `src/module/Solver.{h,cpp}`, `src/module/SolverAlign.{h,cpp}`; `test/solver-reference.js` (test-only: runs stock ImageSolver on a fixture and saves its solution) and probe case `solve_seeded_matches_imagesolver`

**Interfaces:**
- Produces (core):
  - `struct gdg::Seed { std::optional<double> ra, dec, focal_mm, pixel_um, scale_arcsec; }`;
  - `Seed parse_seed(const std::vector<std::pair<std::string,std::string>>& keywords, const std::vector<std::pair<std::string,std::string>>& properties)`: port of `extractImageMetadata` (ImageSolverEngine.js l.332–370), with the same keyword/property precedence and sexagesimal parsing;
  - `struct gdg::Pt { double x, y; }` and `bool fit_affine(const std::vector<gdg::Pt>& image, const std::vector<gdg::Pt>& tangent, double out[6], double& rms)`: closed-form linear least squares (normal equations in double). The core stays PCL-free, so `pcl::LevenbergMarquardt` is used only in the module-side TAN refinement;
  - gnomonic helpers `project_tan`/`unproject_tan`.
- Produces (module): `struct SolverParams { pcl::StringList gaiaDatabaseFiles; pcl::String indexFile; double magLimit = 0 /* 0 = auto from the field size, as ImageSolver does */; bool distortion = true; }`, `struct SolveReport { bool ok; String method; int matches; double rmsPx; String error; }` and `SolveReport SolveImage(pcl::ImageWindow& work, const SolverParams& p, gdg::CatalogSource* onlineForSolve, const gdg::Log&)`.
  - Seed priority: an existing astrometric solution (`ImageWindow::HasAstrometricSolution`), then `parse_seed`.
  - Catalogue: `LocalGaiaSource` positions (the online TAP plain cone `SELECT ra,dec,phot_g_mean_mag FROM gaiadr3.gaia_source` when no local DB exists).
  - Detection: `pcl::StarDetector` with ImageSolver defaults (5 scales, sensitivity 0.5, peak response 0.5, bright threshold 3, max distortion 0.6).
  - Matching: render the projected catalogue as a synthetic reference window, run StarAlignment through `ProcessInstance` in OutputMatrix mode with the ids recorded by Task 2, and read the `outputData` pairs (target coordinates +0.5, as l.439).
  - Fit the affine/TAN, iterate twice, and write a linear solution with `AstrometricMetadata::Write(work)`.
  - Temporary windows are force-closed in a scope guard.

- [ ] **Step 1: Failing tests:** `parse_seed_ra_dec_formats`, `parse_seed_focal_pixel_to_scale`, `fit_affine_recovers_known_transform`. Probe `solve_seeded_matches_imagesolver`, on two real masters from `/mnt/qnap/astro_data/processing-archive-2026-10-03/master/` (FILTER-HaO3 and FILTER-Full): centre within 1″, scale within 0.1%, rotation within 0.05° of the ImageSolver reference.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg): seeded native plate solve (StarDetector + Gaia + StarAlignment)`.

### Task 15: Distortion model, blind fallback, solver gate

**Files:** Modify `src/module/Solver.cpp`; create `src/module/SolverBlind.{h,cpp}`; probe cases `solve_spline_residuals`, `solve_blind_no_metadata`, `solve_fails_loudly_without_seed_or_index`

**Interfaces:**
- Consumes: Task 14 pairs; `/opt/pcl-2.13` from Task 2 (BlindAstrometricSolver exists only in PCL 2.13.0; see "PCL pin").
- Produces: the spline refinement. Build `SplineWorldTransformation` from the matched pairs (ImageSolver defaults: thin-plate/DDM per the reference summary, as `ImageSolverEngine.js` configures it) and write it through `AstrometricMetadata`.
- Produces: `bool BlindSolve(const pcl::ImageWindow&, const String& indexPath, gdg::Seed hint, pcl::BlindAstrometricSolver::Solution&)`.
  - Index path from `Settings::ReadGlobal("/Gaia/AstrometricIndexFile…")`, or by parsing the core settings file as in Task 12.
  - The key name is recorded by the Task 2 probe from 1.9.6's settings after Scott selects an index; none is configured on this box today (spec A2).
  - Detection, BlindSolver doc §1.1: up to three attempts (default parameters; then +1 scale with permissive limits on a k×k-binned copy, k = median FWHM of the 200 brightest / 3 clamped to [2, 8]; then the full image).
  - The blind result seeds Task 14.
- Error text (Review Focus 1): `Plate solve failed: the image has no usable coordinates (RA/DEC, focal length, pixel size) and no astrometric index is selected in Gaia preferences. Add the metadata, or select an Astrometric Index File in the Gaia process preferences.`

- [ ] **Step 1: Failing probes:**
  - `solve_spline_residuals`: RMS ≤ ImageSolver's reported surface residual × 1.25 on both masters;
  - `solve_blind_no_metadata`: keywords stripped, index configured, solution within 2″ of the reference (skips with 77 until an index is configured);
  - `solve_fails_loudly_without_seed_or_index`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: GATE, 2026-11-21.** If `solve_seeded_matches_imagesolver` or `solve_spline_residuals` is not green, stop and report to Scott with the residual numbers. The contingency (D2) needs his OK.
  - **Passed 2026-11-10** (host probe build, PI 1.9.6, ImageSolver 6.6.0 headless with default settings, same DR3 files). Residuals of our solution vs ImageSolver's over a 17×9 grid + centre, Full / HaO3 masters: RMS 0.019 / 0.024 px, max 0.058 / 0.122 px, centre 0.004 / 0.016 px, corners ≤ 0.025 / ≤ 0.029 px; centre 0.007″ / 0.005″, scale −0.0002 %, rotation 0.00003°. Matching RMS 0.107 px (2468 stars) vs 0.108 (2469); 0.125 (3121) vs 0.128 (3123). Blind (stripped metadata, G17 index): same solution, blind step 0.04 s.
- [ ] **Step 6: Commit** `feat(gdg): spline distortion + BlindAstrometricSolver fallback`.

## Phase F: Process and interface

### Task 16: Process, parameters, instance (headless ExecuteOn)

**Files:** Create `src/module/GaiaDepthGrade{Process,Parameters,Instance}.{h,cpp}`, `src/module/StarSplit.{h,cpp}`; modify `GaiaDepthGradeModule.cpp`; probe cases `execute_on_offline`, `split_requires_sxt_message`, `supplied_views_mode`, `master_never_modified`, `output_naming_and_tag`, `replace_target_mode`

**Interfaces:**
- Produces: process `GaiaDepthGrade` (category `scarter4work`). Parameter ids, types and defaults:

  | Parameter id | Type | Default / range | Notes |
  |---|---|---|---|
  | `brightness` | double | 0.65 [0,2] | |
  | `size` | double | 0.85 [0,2] | |
  | `contrast` | double | 0.12 [0,2] | |
  | `saturation` | double | 0.40 [0,2] | |
  | `haloGain` | double | 1.0 [0,4] | |
  | `haloSigma` | double | 2.0 [0.5,6] | base_sigma_px |
  | `pLow` | double | 5.0 [0,50] | |
  | `pHigh` | double | 95.0 [50,100] | |
  | `nebulaEnabled` | bool | false | |
  | `nebulaStrength` | double | 1.0 [0,2] | |
  | `nebulaAtmos` | double | 1.0 [0,2] | |
  | `nebulaStructure` | double | 1.0 [0,2] | |
  | `detectSigma` | double | 6.0 [1,30] | |
  | `detectFWHM` | double | 3.0 | |
  | `matchTolerance` | double | 4.0 [1,12] | |
  | `minMatchRate` | double | 0.3 | |
  | `neutralStrength` | double | 0.0 | |
  | `catalog` | enum | Offline* / Online | |
  | `magLimitEnabled` | bool | true | |
  | `magLimit` | double | 18.0 | |
  | `parallaxSNR` | double | 5.0 | |
  | `haloMinRefs` | int | 8 | |
  | `haloRadiusSigma` | double | 8.0 | |
  | `haloMoffatBeta` | double | 2.5 | |
  | `splitMode` | enum | StarXTerminator* / SuppliedViews | |
  | `starsViewId` | string | | |
  | `starlessViewId` | string | | |
  | `outputMode` | enum | NewWindow* / ReplaceTarget | |
  | `solveIndexFile` | string | empty = from Gaia preferences | |
  | `gaiaDatabaseFiles` | table of string | empty = discover | |
  | `outNDetected`, `outNMatched`, `outMatchRate`, `outMedianOffset`, `outLowMatch` | read-only | | QA outputs |

- Produces: `bool GaiaDepthGradeInstance::ExecuteOn(View&)`, which does:
  1. Duplicate to `<id>_gdgwork`, copying keywords.
  2. `SolveImage` on the duplicate.
  3. The split:
     - **StarXTerminator mode:** `ProcessInstance(Process("StarXTerminator"))`, `stars = true`, `unscreen = true`, ExecuteOn the duplicate; the new stars window is captured by diffing `ImageWindow::AllWindows()`.
     - **SuppliedViews mode:** both views must exist and match the target geometry, otherwise an error.
  4. `ImageBridge` to the core, `prepare_grade` (catalogue per `catalog`), `render_from_prep`, `render_nebula` when enabled, `screen_blend`.
  5. Output:
     - **NewWindow mode:** a new window `<id>_depthgraded`, 32-bit float, with the target's keywords plus `DEPTHTAG` = honesty text and a HISTORY line;
     - **ReplaceTarget mode:** writes into the target (this is what makes ImageContainer batch and headless file-out work).
  6. Force-close the temporary windows; QA goes to the console and the read-only parameters.
- Produces: `class PrepareSession { /* holds gdg::Image stars, starless; PrepResult prep; target id + geometry; IsStale(View) */ }`, which Task 17 consumes.
- Error texts verbatim from the PJSR (`splitStars`, `solveAndSplit`, `queryLocalGaiaDR3`).

- [ ] **Step 1: Failing probes:**
  - `execute_on_offline` (real master: an output window exists, QA match_rate ≥ the Python golden's on the same frozen inputs, Task 19);
  - `split_requires_sxt_message` (Review Focus 5, with StarXTerminator masked by `splitMode` plus a process lookup stub through env `GDG_TEST_HIDE_SXT=1` honoured only in test builds);
  - `supplied_views_mode`;
  - `master_never_modified` (target checksum before = after);
  - `output_naming_and_tag` (`<id>_depthgraded`, DEPTHTAG present);
  - `replace_target_mode`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg): GaiaDepthGrade process (solve, split, prepare, render, blend)`.

### Task 17: Interface (dialog parity)

**Files:** Create `src/module/GaiaDepthGradeInterface.{h,cpp}`; `test/gui-smoke.sh`, `test/gui_drive.py` (copy Planetary's XTest driver pattern; private Xvfb, slot 97)

**Interfaces:**
- Consumes: Task 16 `PrepareSession` and the parameters. Layout mirrors the dialog:
  - target view selector (main views; defaults to the active window) and `Browse…` (opens the file as a window and selects it);
  - preview control 760×420 (autostretched screen blend at ≤ 760 px wide; an orange 2 px border when stale);
  - 1:1 inset 256 px (click sets the full-res centre; the default is the image centre; clamped);
  - **"Look (live — Preview to apply)"** group: Brightness, Size, Contrast, Saturation, Halo sigma, p_low, p_high, the "Grade the nebula too (depth from structure, scaled to Gaia)" checkbox, Nebula depth, atmospheric, structure;
  - **"Matching (changing these needs re-Prepare)"** group: `detect_threshold_sigma`, `match_tolerance_px`, the "Offline (local Gaia DR3) — fast" checkbox with the existing tooltip;
  - an **Advanced** collapsible section with the remaining engine parameters and split/output/catalogue-file settings;
  - buttons Prepare / Preview / Update / Execute / Close (Preview and Execute disabled until prepared; prepare-stale disables them);
  - a status line with `[GDG]` console mirroring, the QA summary `Prepared: %d/%d matched (rate %.2f, offset %.1f px)` and `  ⚠ LOW MATCH — depth grade unreliable`, and errors as `ERROR: …`.
- Prepare and render run on a `pcl::Thread` with StatusMonitor progress. The `PrepareSession` re-validates the target view before Preview and Execute (Review Focus 4).

- [ ] **Step 1: Failing GUI smoke steps:** `controls_and_defaults` (reads each control's value), `prepare_enables_preview`, `slider_marks_preview_stale`, `detect_edit_marks_prepare_stale`, `inset_click_moves_center`, `execute_creates_depthgraded`, `close_target_then_execute` (Review Focus 4). Screenshots go to `<out-dir>`.
- [ ] **Step 2: Run** `bash modules/gaia-depth-grade/test/gui-smoke.sh /tmp/gdg-gui`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg): GaiaDepthGrade interface with Prepare/Preview/Execute parity`.

## Phase G: Recon carry-over

### Task 18: Port recon core (plan tasks 1–8)

**Files:** Create `src/core/recon/{Model.h, Channels, Ionization, Ionizer, Extinction, Cavity}.{h,cpp}`, `test/unit/test_recon_*.cpp`

**Interfaces:**
- Produces C++ equivalents with the same names and defaults as `gaia-depth-grade/src/gaia_depth_grade/recon/*.py`:
  - `ChannelSet`, `IonizationMap`, `Ionizer`, `ReconConfig` (every default), `CavityModel`, `validate_shapes`;
  - `from_mono_sho`, `from_duoband`, `from_osc_rgb`, `looks_linear` (`LINEAR_MEDIAN_FRAC_MAX` 0.10), `extract_channels`;
  - `ionization_ratio` (σ 2);
  - `pick_ionizer` (flux weight 0.4; fallbacks `gaia-blue` → `gaia-bright` → `field-center`), `ionizer_from_xy`;
  - `dust_from_structure` (lane 15, sky pct 5, bright pct 99, surround frac 0.05), `dust_from_balmer` (Case B 2.86, saturation 3.0, signal gate);
  - `geometry_prior`, `ionization_depth`, `emission_measure_relief` (σ 1/4, gain 0.5, 99.5 pct), `reconstruct_cavity` (footprint 10/99.5 pct, emissivity 99.9 pct over the stacked channels, and the meta keys verbatim).
- No process or UI (D7). Part 2 builds plan tasks 9–13 in C++ on top of this.

- [ ] **Step 1: Failing tests:** one golden-equality test per Python function (|Δ| ≤ 1e-6 on float32 outputs, exact on confidences and meta), plus the Python tests' edge cases ported 1:1 from `gaia-depth-grade/tests/recon/*.py` (sii None, mismatched shapes throw, stretched input zeroes confidence, no star → field-center).
- [ ] **Step 2: Run** `ctest -R test_recon`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): recon core (cavity, ionization, dust, ionizer) ported with golden parity`.

## Phase G2: Recon Part 2, native (the Python plan's Tasks 9–13)

Scott's ruling (2026-11-12): these are written only in PCL C++, never Python. Numbered 21–26 so the existing references to Tasks 19 and 20 stay valid; Task 20's gate covers them (see its Step 4). The Python versions were never written, so there are **no Python goldens**: tests assert against analytic truth on a C++ synthetic blister, and against real data in Task 26.

**Rules for this phase (in addition to the Global Constraints):**
- Nothing here ships or depends on Python or PJSR, including test fixtures: the synthetic blister is generated in C++ (`test/util/Blister.h`), and the NAS validation is a ctest executable. Bash is used only as the thin PixInsight-launch harness, in the `run-probe.sh` style (D12).
- New core code is PCL-free, in `gdg_core`, threaded with `gdg::parallel_for_dynamic` / `parallel_for_if` (`src/core/Parallel.h`), no OpenMP, no global mutable state; each task carries a `*_concurrent_calls_identical` test (two threads, same input, bit-identical output).
- No new third-party code. PNG output in Task 26's test reuses the already vendored `modules/nukex/third_party/stb/stb_image_write.h` (test-only, like Catch2).
- Frame: core y = PixInsight row (C1). 3D exports put +Y up, so `Y = −(y − apex_y)`.
- Honesty text, verbatim from the Python plan's Task 10, on every recon output (FITS keyword `RECONTAG`, GLB `asset.extras.honesty`, doc): `Reconstructed HII cavity; physically motivated, not measured gas depth`.

**Review Focus (Phase G2):**
1. **Stretched input.** A finished RGB fed to the linear path must run geometry-only and say so (`degraded = "stretched-input"`, a console warning), never emit physics-flavoured output silently. Tests: Task 22 `reconstruct_stretched_input_degrades`, Task 25 `recon_stretched_input_flagged`.
2. **Channel views not co-registered.** Different geometry → one error naming both views and their sizes; no output window. Test: Task 25 `recon_mismatched_geometry_error`.
3. **No Gaia distance** (no reference view, ionizer unmatched, or override without distance). No physical scale: GLB units `px`, `outPcPerPx` = 0, no crash. Tests: Task 22 `reconstruct_no_distance_no_scale`, Task 23 `glb_units_px_without_scale`.
4. **Unwritable or existing output path.** Loud error naming the path; never a partial file; an existing file is replaced only by the atomic rename. Tests: Task 23 `glb_write_atomic_and_errors`, Task 24 `gif_write_errors`.
5. **Full-size masters.** 6072×4042 at decimate 1 must stay under glTF's 4 GiB uint32 limit or fail naming `glbDecimate`. Test: Task 23 `glb_size_limit_error`.

### Task 21: Render the CavityModel to a registered linear image

**Files:** Create `src/core/recon/Render2D.{h,cpp}`, `test/util/Blister.h`, `test/unit/test_recon_render2d.cpp`; modify `src/core/recon/Model.h` (add `double render_limb_gain = 0.5;` to `ReconConfig`, after `render_dust_strength`), `src/core/CMakeLists.txt` (recon block), `test/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 18 (`CavityModel`, `ReconConfig`, `reconstruct_cavity`, `from_mono_sho`, `ionization_ratio`, `dust_from_structure`, `ionizer_from_xy`); Task 3 `gaussian_filter`; Task 4 `percentile`.
- Produces (test util): `struct gdg::test::BlisterTruth { double apex_x, apex_y, radius_px; double lane_y0, lane_y1; }`, `struct gdg::test::Blister { gdg::Image ha, oiii, sii, rgb; BlisterTruth truth; }` and `Blister make_blister(int nx = 128, int ny = 96, std::uint64_t seed = 20261112)`.
  - A paraboloid cavity about the apex (0.55 nx, 0.45 ny), radius 0.4 min(nx, ny); [O III] bright for r < 0.5 R, Hα and [S II] on the 0.6–1.0 R rim; a dark near-side lane (rows `lane_y0..lane_y1`, 4 px wide) attenuating all channels ×0.2 inside the footprint; sky 0.002, Gaussian noise σ 0.0005 from `std::mt19937_64(seed)`; `rgb` = (ha + sii, 0.5 oiii, oiii). Deterministic for a seed.
- Produces: `gdg::Image gdg::recon::render_cavity_2d(const CavityModel& model, const ReconConfig& cfg)`: (ny, nx, 3), float32-valued, in [0, 1], same grid as the model. Throws `std::invalid_argument` unless `emissivity.channels == 3`. Per pixel, with a = `render_ambient`, s = `render_depth_shading`, γ = `render_gamma`:
  - `z_rel = z − min(z)` (a flat model has no depth cue);
  - `shade = (a + (1 − a)·clip(1 − s·z_rel, 0, 1))^γ` (γ shapes the shading only; the data is never stretched);
  - limb: `g = |∇ gaussian_filter(z, 1)|` (central differences, one-sided at edges, as `np.gradient`); `q` = 99.5th percentile of the g > 0 values (limb term off when none); `limb = 1 + render_limb_gain·clip(g/q, 0, 1)`;
  - occlusion: `T = 1 − clip(render_dust_strength·dust, 0, 1)`;
  - `out_c = clip(f32(E_c·shade·limb·T), 0, 1)`.

- [ ] **Step 1: Failing tests:**
  - `blister_is_deterministic` (same seed → identical planes);
  - `render2d_output_registered_linear_rgb` (shape (ny, nx, 3), every value finite and in [0, 1]);
  - `render2d_core_reads_concave` (blister through `reconstruct_cavity` with an `ionizer_from_xy` at the truth apex: mean of out/E over r ≤ 0.1 R is at least 0.15 below its mean over 0.8–1.0 R);
  - `render2d_dust_occludes` (where dust ≥ 0.5 and `render_dust_strength` = 1: out ≤ 0.5 E);
  - `render2d_degenerate_is_identity` (z ≡ 0.3, dust ≡ 0: max |out − clip(E, 0, 1)| ≤ 1e-7);
  - `render2d_rejects_mono_emissivity`;
  - `render2d_concurrent_calls_identical`.
- [ ] **Step 2: Run** `cmake --build build && (cd build && ctest -R test_recon_render2d --output-on-failure)`. Expected: FAIL.
- [ ] **Step 3: Implement** `render_cavity_2d` and `make_blister`, rows through `parallel_for_dynamic`.
- [ ] **Step 4: Run** the same command, then `ctest -R test_recon` (the Task 18 goldens stay green: the new `ReconConfig` field is not in the meta). Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): render CavityModel to a registered linear dimensional image`.

### Task 22: Reconstruct pipeline (the `reconstruct` subcommand's core)

**Files:** Create `src/core/recon/Reconstruct.{h,cpp}`, `test/unit/test_recon_reconstruct.cpp`; modify `src/core/CMakeLists.txt`, `test/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 18 (all of `recon/*`, `ionizer_table`), Task 11 `StarRow`, Task 7 `Log`, Task 21.
- Produces:
  - `inline constexpr const char* gdg::recon::RECON_HONESTY = "Reconstructed HII cavity; physically motivated, not measured gas depth";`
  - `gdg::Image channel_mean(const gdg::Image&)`: mono pass-through; colour → per-pixel channel mean (f32). Duoband inputs that are OSC masters (Scott's D1/D2 on the ASI2400MC) enter `from_duoband` this way: an OSC D1 frame's total signal is Hα + [O III].
  - `struct ReconInputs { std::string mode; std::map<std::string, gdg::Image> channels; std::optional<gdg::Image> hb; std::vector<gdg::StarRow> stars; std::optional<Ionizer> ionizer_override; std::function<std::optional<double>(double x, double y)> deg_per_px_at; };` (mode and channel keys as `extract_channels`; `deg_per_px_at` null = no astrometry).
  - `struct ReconResult { CavityModel model; IonizationMap ionization; Ionizer ionizer; gdg::Image lum; std::string degraded; };`
  - `ReconResult reconstruct(const ReconInputs&, const ReconConfig&, const gdg::Log&)`:
    1. duoband channels through `channel_mean`; `extract_channels(channels, mode)`;
    2. `ionization_ratio`;
    3. `lum = f32(ha + oiii + (sii or 0))`;
    4. ionizer = override, else `pick_ionizer(ionizer_table(stars), ny, nx)`;
    5. `dust = dust_from_structure(lum)`; with `hb`: `dust = max(dust, dust_from_balmer(ha, hb))` (hb of another shape throws `std::invalid_argument`);
    6. `pc_per_px = distance_pc · deg_per_px_at(x, y) · π/180` when both exist, else none;
    7. `reconstruct_cavity(...)`;
    8. `degraded = "stretched-input"` when `!channels.is_linear`, with the warning, verbatim: `recon: input looks stretched (non-linear); ratio term off, geometry prior only. The result is not physically constrained.`

- [ ] **Step 1: Failing tests:**
  - `reconstruct_sho_end_to_end_concave` (blister SHO: mean z over r ≤ 0.1 R > mean z over 0.8–1.0 R; `meta.capture_mode == "sho"`; degraded empty);
  - `reconstruct_duoband_rgb_uses_channel_mean` (RGB d1/d2 give the same model as their precomputed channel means, bit-identical);
  - `reconstruct_osc_low_confidence_still_concave`;
  - `reconstruct_stretched_input_degrades` (asinh-stretched blister RGB: degraded `"stretched-input"`, `meta.ratio_confidence == 0`, the warning captured through `Log`; Review Focus G2-1);
  - `reconstruct_mismatched_shapes_throw`, `reconstruct_hb_shape_mismatch_throws`;
  - `reconstruct_override_ionizer_used` (`source == "override"`, apex = the override);
  - `reconstruct_pc_per_px` (distance 1000 pc at 1″/px → 1000 · (1/3600) · π/180 pc/px, rel ≤ 1e-12);
  - `reconstruct_no_distance_no_scale` (no stars, no override → `field-center`, `pc_per_px` none; Review Focus G2-3);
  - `reconstruct_concurrent_calls_identical`.
- [ ] **Step 2: Run** `(cd build && ctest -R test_recon_reconstruct --output-on-failure)`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): reconstruct pipeline (channels -> CavityModel), degraded-input flag, Gaia pc scale`.

### Task 23: Native GLB writer (rotatable 3D model)

**Files:** Create `src/core/recon/Glb.{h,cpp}`, `test/util/GlbRead.h` (header + chunk parser and typed reads of the BIN chunk; test-only), `test/unit/test_recon_glb.cpp`; modify `src/core/CMakeLists.txt`, `test/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 18 `CavityModel`, `cavity_meta_json`; Task 22 `RECON_HONESTY`.
- Produces: `struct ExportConfig { int decimate = 2; double depth_ratio = 0.5; double dust_offset_frac = 0.02; double dust_min = 0.01; };` (`decimate` 2 is the Python plan's default), `std::vector<std::uint8_t> build_glb(const CavityModel&, const ExportConfig&)`, and `void write_glb(const std::string& path, const CavityModel&, const ExportConfig&)` (writes `path + ".tmp"`, fsyncs, renames; any failure throws `std::runtime_error("cannot write <path>: <strerror>")` and removes the temporary file).
- **Layout contract** (glTF 2.0 binary; the tests rely on it):
  - Grid: columns `x = i·d` for i = 0 … (nx − 1)/d, rows likewise; vertex index `row·cols + col`.
  - Positions (float32 VEC3, accessor min/max set): `X = (x − ax)·u`, `Y = −(y − ay)·u`, `Z = −z·D·u`. (ax, ay) = `apex_xy`; `D = depth_ratio · R`, R = the largest apex distance of a pixel with z > 0 (the whole image when none); `u = pc_per_px`, or 1 when absent.
  - Surface colour: `COLOR_0` float32 VEC3 = clip(emissivity, 0, 1) (glTF vertex colours are linear, as the model is).
  - Indices: uint32, two triangles per cell, counter-clockwise seen from +Z.
  - Material 0: `KHR_materials_unlit` (listed in `extensionsUsed`), `doubleSided`.
  - Dust mesh, only when max(dust) ≥ `dust_min`: the same grid at `Z + dust_offset_frac·D·u` (nearer the viewer); `COLOR_0` float32 VEC4 (0.05, 0.04, 0.03, dust); material 1 unlit, `alphaMode` `BLEND`; it reuses the index accessor.
  - bufferViews and accessors in this order: surface positions, surface colours, indices, then dust positions and dust colours.
  - `asset`: `{"version":"2.0","generator":"GaiaDepthGrade NebulaReconstruct","extras":{"honesty":RECON_HONESTY,"units":"pc"|"px","depth_ratio":…,"meta":<cavity_meta_json object>}}`.
  - JSON chunk space-padded and BIN chunk zero-padded to 4 bytes; magic `glTF`, version 2.
  - A total over 4 294 967 295 bytes throws `std::length_error("GLB would exceed 4 GiB; raise glbDecimate (now <d>)")` before allocating.

- [ ] **Step 1: Failing tests:**
  - `glb_header_and_chunks_valid` (magic, version, total length = file size, chunk types JSON/BIN, 4-byte alignment);
  - `glb_vertex_grid_count` (9×7 model, d = 2 → 5×4 = 20 vertices, 72 indices; d = 1 → 63 vertices);
  - `glb_surface_is_concave` (blister model: Z at the apex vertex < mean Z of the rim vertices);
  - `glb_position_min_max_match_data`;
  - `glb_front_faces_toward_viewer` (z ≡ 0: the first triangle's normal has +Z);
  - `glb_units_pc_when_scale_known`, `glb_units_px_without_scale` (Review Focus G2-3);
  - `glb_dust_mesh_only_when_dust`;
  - `glb_honesty_and_meta_in_extras`;
  - `glb_write_atomic_and_errors` (unwritable directory → the error names the path, no `.tmp` left; Review Focus G2-4);
  - `glb_size_limit_error` (a 6072×4042 model at d = 1 is sized arithmetically and passes; a synthetic over-limit size throws the message above, without allocating; Review Focus G2-5);
  - `glb_concurrent_calls_identical`.
- [ ] **Step 2: Run** `(cd build && ctest -R test_recon_glb --output-on-failure)`. Expected: FAIL.
- [ ] **Step 3: Implement** (hand-written JSON; the BIN chunk filled in parallel by rows).
- [ ] **Step 4: Run.** Expected: PASS. Then load one written blister `.glb` in any glTF viewer available on the box and attach a screenshot to the task report (manual look, not a gate).
- [ ] **Step 5: Commit** `feat(gdg-core): native GLB export of the cavity model (unlit emissive surface + dust layer)`.

### Task 24: Orbit animation (native GIF89a)

**Files:** Create `src/core/Gif.{h,cpp}`, `src/core/recon/Orbit.{h,cpp}`, `test/util/GifRead.h` (GIF89a + LZW decoder, test-only), `test/unit/test_gif.cpp`, `test/unit/test_recon_orbit.cpp`; modify `src/core/Display.{h,cpp}` (split `autostretch_u8` into the two functions below; its behaviour is unchanged), `src/core/CMakeLists.txt`, `test/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 10 (`downscale_u8`), 21, 23 (`ExportConfig`, the `D` of its contract).
- Produces (Display): `struct StfParams { std::vector<double> c0, m; };` (one entry per channel), `StfParams stf_params(const gdg::Image& rgb, double target_bg = 0.25, double shadows_clip = -2.8)` and `std::vector<std::uint8_t> apply_stf_u8(const gdg::Image& rgb, const StfParams&)`; `autostretch_u8(x) == apply_stf_u8(x, stf_params(x))`.
- Produces (GIF): `void write_gif(const std::string& path, const std::vector<std::vector<std::uint8_t>>& frames_rgb, int width, int height, int delay_cs, int loops = 0)`.
  - GIF89a, one global 256-entry palette by median cut over all frames' pixels (every pixel up to 4 M samples, else every k-th, k = ⌈total / 4 M⌉).
  - Nearest palette colour by squared RGB distance, lowest index on ties, through a 32³ cache.
  - LZW: minimum code size 8, clear code when the table reaches 4096.
  - `NETSCAPE2.0` loop extension; one graphic control extension per frame carrying `delay_cs`.
  - Atomic write and errors as `write_glb`.
- Produces (orbit): `struct OrbitConfig { int frames = 36; double amplitude_deg = 12.0; int max_width = 760; int delay_cs = 8; };` (36 and 12 are the Python plan's defaults), `std::vector<gdg::Image> orbit_views(const CavityModel&, const ReconConfig&, const ExportConfig&, const OrbitConfig&)` and `void write_orbit_gif(const std::string& path, const CavityModel&, const ReconConfig&, const ExportConfig&, const OrbitConfig&)`.
  - Frame k: `θ_k = amplitude·sin(2πk / frames)`, a small yaw parallax orbit.
  - Base = `render_cavity_2d(model, cfg)`. Each pixel moves to `x' = x + tan θ_k·(z·D − z_apex·D)` (D as in Task 23 with u = 1, z_apex = z at the rounded apex), splatted to `round(x')` with a z-buffer (smaller z wins).
  - Holes are filled by linear interpolation along the row, nearest value at the ends. θ = 0 gives the base image exactly.
  - `write_orbit_gif`: STF parameters from frame 0, applied to every frame (no flicker), `downscale_u8` to `max_width`, then `write_gif`.

- [ ] **Step 1: Failing tests:**
  - `autostretch_split_is_identical` (and Task 10's `autostretch_matches_golden` stays green);
  - `gif_roundtrip_exact_for_256_colours` (an image with ≤ 256 colours decodes bit-exact);
  - `gif_lzw_table_reset` (320×240 noise over 256 colours forces clear codes; decodes exact);
  - `gif_frame_count_delay_and_loop`;
  - `gif_quantization_error_bounded` (a 256×64 truecolour gradient: mean |error| ≤ 4 per channel);
  - `gif_write_errors` (Review Focus G2-4);
  - `orbit_frame0_equals_render`;
  - `orbit_frames_differ` (blister: frame 0 vs frame `frames/4`, mean |Δ| > 1e-3);
  - `orbit_flat_model_frames_identical`;
  - `orbit_gif_has_n_frames_and_width` (decoded: 36 frames, width ≤ 760);
  - `orbit_concurrent_calls_identical`.
- [ ] **Step 2: Run** `(cd build && ctest -R 'test_gif|test_recon_orbit|test_display' --output-on-failure)`. Expected: FAIL (test_display passes).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg-core): parallax orbit animation with a native GIF89a writer`.

### Task 25: `NebulaReconstruct` process (reconstruct + export, headless and command line)

**Files:** Create `src/module/NebulaReconstruct{Process,Parameters,Instance}.{h,cpp}`, `src/module/ReconBridge.{h,cpp}`, `src/module/probe/ProbeRecon.cpp`, `doc/NebulaReconstruct/NebulaReconstruct.html`, `test/run-probe-recon.sh`; modify `src/module/GaiaDepthGradeModule.cpp` (register the process), `src/module/probe/ProbeProcess.{h,cpp}` (command-line interface), `src/module/CMakeLists.txt`, `release.sh` (`GDG_DOC_IDS=(GaiaDepthGrade NebulaReconstruct)`)

**Interfaces:**
- Consumes: Tasks 21–24; Task 11 `ImageBridge`; Task 16 `RunPrepare`, `PrepareSession`, `GradeOptions` and `SplitWithStarXTerminator` (names as in the Task 16 work in progress, `src/module/PrepareSession.h` / `StarSplit.h`; use whatever Task 16 commits).
- Produces: process `NebulaReconstruct` (D10): category `scarter4work`, global (`CanExecuteGlobal`/`ExecuteGlobal`; `ExecuteOn` is refused with "NebulaReconstruct is a global process: set the channel views and execute globally").

  | Parameter id | Type | Default / range | Notes |
  |---|---|---|---|
  | `source` | enum | Channels* / ModelViews | ModelViews re-renders/re-exports a saved model (the `.npz` of the Python plan) |
  | `mode` | enum | SHO / Duoband* / OSC | |
  | `haViewId` `oiiiViewId` `siiViewId` `d1ViewId` `d2ViewId` `rgbViewId` `hbViewId` | string | | linear, starless, co-registered; `hbViewId` optional |
  | `referenceViewId` | string | empty = no Gaia | a linear image of the same field; Task 16's `RunPrepare` on it gives the star table and the solution |
  | `catalog` `magLimitEnabled` `magLimit` `parallaxSNR` `detectSigma` `detectFWHM` `matchTolerance` `splitMode` `starsViewId` `starlessViewId` `solveIndexFile` `gaiaDatabaseFiles` | | ids, types and defaults exactly as Task 16 | passed to `RunPrepare` as `GradeOptions` |
  | `ionizerOverride` | bool | false | |
  | `ionizerX`, `ionizerY` | double | 0 | core-frame pixels |
  | `ionizerDistancePc` | double | 0 = none | |
  | `wGeometry` `wIonization` `wEmission` `emissionSign` `wDust` `footprintFrac` `geometryPower` `ionizationGamma` | double | `ReconConfig` defaults (1.0, 0.6, 0.15, 1.0, 0.8, 0.05, 2.0, 1.0) | |
  | `renderGamma` `renderAmbient` `renderDepthShading` `renderDustStrength` `renderLimbGain` | double | 1.0, 0.05, 0.5, 1.0, 0.5 | |
  | `zViewId` `emissivityViewId` `dustViewId` | string | | ModelViews source |
  | `outputModel` | bool | true | z / emissivity / dust windows |
  | `outputRender` | bool | true | |
  | `glbPath` | string | empty = none | |
  | `glbDecimate` | int32 | 2 [1,64] | |
  | `depthRatio` | double | 0.5 [0.05,4] | |
  | `orbitPath` | string | empty = none | |
  | `orbitFrames` | int32 | 36 [2,360] | |
  | `orbitAmplitude` | double | 12 [0,45] | degrees |
  | `outIonizerX` `outIonizerY` `outPcPerPx` | double | read-only | 0 = no scale |
  | `outIonizerSource` `outCaptureMode` `outDegraded` | string | read-only | |

- **Outputs** (base = the ha/d1/rgb view id; the z view's id under ModelViews):
  - Windows `<base>_recon_z`, `<base>_recon_emissivity`, `<base>_recon_dust` and `<base>_recon` (the render), 32-bit float, not shown.
  - Each carries the base's keywords plus `RECONTAG` = `RECON_HONESTY` and a HISTORY line, and the reference's astrometric solution when there is one.
  - `<base>_recon_z` carries storable properties `GaiaDepthGrade:Recon:ApexX`, `:ApexY`, `:PcPerPx` (absent when none), `:Meta` (`cavity_meta_json`) and `:Degraded`. ModelViews reads them back; a view without `:Meta` → `not a NebulaReconstruct model: <id> lacks GaiaDepthGrade:Recon:Meta`.
- **Errors:**
  - Geometry mismatch (Review Focus G2-2), verbatim: `channel views are not co-registered: <idA> is <w>x<h>, <idB> is <w>x<h>`.
  - Missing view: `view not found: <id> (<parameter>)`.
  - Every core exception → `pcl::Error` with its text.
  - No window is left open on failure.
- **Command-line interface** (`CanProcessCommandLines` true): `NebulaReconstruct [--mode=sho|duoband|osc] [--ha= --oiii= --sii= | --d1= --d2= | --rgb=] [--hb=] [--reference=] [--ionizer=x,y[,dist]] [--from-model=z,emissivity,dust] [--model=<dir>] [--image=<file>] [--glb=<file>] [--decimate=N] [--depth-ratio=R] [--orbit=<file.gif>] [--frames=N] [--amplitude=deg]`.
  - Each value is a view id or an image file; files are opened as windows and closed afterwards.
  - `--model` saves the three model windows as XISF (`recon_z.xisf`, `recon_emissivity.xisf`, `recon_dust.xisf`, properties kept).
  - `--image` saves the render.
  - Exit 0 on success; non-zero with the error on the console. This is the native form of the Python plan's `reconstruct` and `export3d` subcommands.
- **Test harness** (D12): `GaiaDepthGradeProbe` gains `CanProcessCommandLines` / `ProcessCommandLine` for `GaiaDepthGradeProbe --case=<id> --out=<json>`, with case inputs taken from the environment as today.
  - `run-probe-recon.sh` sources `harness.sh`. Per case it writes a one-line `<case>.scp` into `$GDG_RUN_DIR` (never committed) and launches `pi_headless timeout 600 "$PI" -n="$GDG_SLOT" --automation-mode --no-startup-scripts -m="$SO" -r="$GDG_RUN_DIR/<case>.scp" --force-exit`.
  - It asserts on the JSON with `jq`, as `run-probe.sh` does. The probe builds its synthetic inputs from `test/util/Blister.h` (probe sources are test-only, `-DGDG_BUILD_PROBES=ON`).

- [ ] **Step 1: Failing probes** (`bash modules/gaia-depth-grade/test/run-probe-recon.sh`):
  - `scp_launch_reaches_probe` **first**: proves `-r=<file>.scp` runs a command script under `--automation-mode` and `--force-exit` exits. If it does not, **stop and report to Scott**; no PJSR fallback.
  - `recon_execute_global_sho`: blister windows → ExecuteGlobal → four windows, `RECONTAG` on each, z concave, properties present, read-only outputs set.
  - `recon_model_views_roundtrip`: `--model` save, then `--from-model` → render max |Δ| ≤ 1e-6 against the first run; the GLB is written.
  - `recon_command_line_files`: `NebulaReconstruct --mode=osc --rgb=<blister.xisf> --image=… --glb=… --orbit=…` from the `.scp` → three files; GLB header valid; GIF frame count 36.
  - `recon_mismatched_geometry_error` (Review Focus G2-2: the exact text, no window left).
  - `recon_stretched_input_flagged` (Review Focus G2-1: `outDegraded == "stretched-input"`).
  - `recon_execute_on_refused`.
  - `recon_reference_gaia_scale`: the NAS HaO3 master as both d1 and the reference, SXT split → `outPcPerPx > 0`, `outIonizerSource` ∈ {gaia-blue, gaia-bright}. Exits 77 without the NAS or StarXTerminator.
  - `process_category` (both processes under `scarter4work`).
- [ ] **Step 2: Run.** Expected: FAIL (no process).
- [ ] **Step 3: Implement** the process, `ReconBridge` (views ↔ `ReconInputs`; model ↔ windows/properties), the command line, the probe command line and the doc page. The doc page covers what it does, the honesty statement, inputs and capture modes, outputs, files written (only the named paths), and network (only through `referenceViewId` with `catalog = Online`).
- [ ] **Step 4: Run** `tools/build-env/build-modules.sh` (container build, ctest, `verify-portable.sh`), then `GDG_SO=<host probe build> bash modules/gaia-depth-grade/test/run-probe-recon.sh`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(gdg): NebulaReconstruct process (reconstruct, re-render, GLB, orbit; command line)`.

### Task 26: NAS real-data validation (acceptance)

**Files:** Create `src/core/recon/Validate.{h,cpp}`, `test/unit/test_recon_validate.cpp`, `src/module/probe/ProbeReconFreeze.cpp`, `src/module/probe/NpyWrite.h` (`<f4`/`<f8` C-order `.npy` v1 writer; test-only), `test/run-recon-freeze.sh`, `test/unit/test_recon_real.cpp`, `test/golden/RECON_VALIDATION.md`; modify `src/core/CMakeLists.txt`, `test/CMakeLists.txt` (`test_recon_real` with `SKIP_RETURN_CODE 77`; it includes `modules/nukex/third_party/stb`)

**Interfaces:**
- Consumes: Tasks 10 (`render_nebula`, `downscale_u8`), 16 (`RunPrepare`, `SplitWithStarXTerminator`), 21–25.
- Produces (core):
  - `double cavity_depth_index(const CavityModel&)`: footprint = z > 0, R as in Task 23; = mean z over r ≤ 0.1 R − mean z over 0.8 R < r ≤ R; NaN when either set is empty.
  - `double interior_vs_front_ratio(const CavityModel&, const IonizationMap&)`: inside the footprint, interior = `oiii_fraction` ≥ its 75th percentile there, front = ≤ its 25th; = mean z(front) − mean z(interior). Positive means the front is correctly farther.
- Produces (freeze; probe case `freeze_recon_fixtures`, launched by `run-recon-freeze.sh` through the `.scp` route of Task 25). It writes `$GDG_FIXTURES/recon/<target>/` (default `~/.cache/gdg-fixtures`, on disk, not the tmpfs `/tmp`) and reads the NAS read-only.

  | Target | Subs | Mode |
  |---|---|---|
  | `M42-duoband` | `9_24_2023/M42` HaO3 ×20, S2O3 ×26 | duoband |
  | `M42-broadband` | `2_4_2023/M42_1-<k>`: the panel whose first sub solves nearest M42 (RA 83.8221°, Dec −5.3911°) | osc |
  | `M8-duoband` | `8_8_2023/M8` HaO3 ×5, S2O3 ×5 | duoband |
  | `M8-broadband` | `5_10_2023/M8` ×40 | osc |

  - Per target, in C++ through `ProcessInstance`: Debayer (pattern from `BAYERPAT`; error if absent), StarAlignment (every filter of a target registered to the first sub of its first filter), then ImageIntegration with PixInsight's defaults (D13: no calibration frames).
  - Then `RunPrepare` on the D1/broadband master (offline Gaia, StarXTerminator); SXT on the D2 master.
  - Writes `starless_<filter>.npy` (`<f4`, (ny, nx, 3)), `stars.npy` (`<f8`, N×5: x, y, flux, bp_rp, r_med), and `fixture.json`: subs with sha256, `deg_per_px` at every star from `ResolutionAt`, the PixInsight version, and `"calibrated": false`.
  - A target whose `fixture.json` exists is skipped unless `GDG_REFREEZE=1`. `$GDG_FIXTURES/recon/MANIFEST.json` lists the frozen targets.
- Produces (ctest `test_recon_real`): exits 77 when the manifest is absent. Per target it loads the fixtures into `ReconInputs` (`deg_per_px_at` = the nearest star's value) and runs `reconstruct`. It writes `out/metrics.json`, plus `out/<target>_before.png` (Task 10 nebula grade of the starless at strength/atmos/structure 1) and `out/<target>_after.png` (`render_cavity_2d`), both with the STF parameters of `before`, ≤ 1600 px wide, through `stb_image_write`. For the two duoband targets it also writes `<target>.glb` and `<target>_orbit.gif`.

- [ ] **Step 1: Failing tests:**
  - `test_recon_validate`: `depth_index_positive_for_bowl`, `depth_index_nan_without_footprint`, `interior_vs_front_positive_when_front_far`, `interior_vs_front_negative_when_inverted` (synthetic models with known ordering).
  - `test_recon_real`, for every target: z finite and in [0, 1]; `degraded` empty (masters are linear); (a) `interior_vs_front_ratio > 0`. Across targets: (b) `cavity_depth_index(M8-x) > cavity_depth_index(M42-x)` for x ∈ {duoband, broadband}; (c) duoband and broadband of one target agree in the sign of both metrics.
- [ ] **Step 2: Run** `(cd build && ctest -R test_recon_validate --output-on-failure)`. Expected: FAIL.
- [ ] **Step 3: Implement** the metrics, then the freeze probe; run `bash modules/gaia-depth-grade/test/run-recon-freeze.sh` once (it reports per-target sub counts, integration time and fixture sizes).
- [ ] **Step 4: Run** `(cd build && ctest -R 'test_recon_validate|test_recon_real' --output-on-failure)`. Expected: PASS, with `test_recon_real` executed, not skipped.
  - A red (a)–(c) on real data is fixed in the owning task's code or reported to Scott with the numbers. Never fixed by loosening the assertion or deselecting a target.
  - Assertion (b) compares the reconstruction's relative concavity (z is normalised to [0, 1]). The physical stand-off difference (M8 1–1.8 pc vs M42 0.23 pc) is a design expectation the metric does not measure; `RECON_VALIDATION.md` says so.
- [ ] **Step 5: Acceptance.** Record the metrics in `test/golden/RECON_VALIDATION.md`. Scott judges the before/after renders of M42 and M8 (duoband) against the Robles M8 reference: (a) the bright core reads as a concave cavity; (b) [O III] interior / Hα–[S II] front ordering is visible; (c) M8 reads deeper than M42. His verdict is recorded as a dated row in that file.
- [ ] **Step 6: Commit** `test(gdg): recon real-data validation on NAS M42/M8 (duoband + broadband)`.

## Phase H: Real-data parity and cut-over

### Task 19: Real-data parity harness

**Files:** Create `test/run-real-parity.sh` (host; reads the NAS, writes only under `$GDG_FIXTURES`, default `~/.cache/gdg-fixtures`), `test/probe-freeze.js` (test-only: StarXTerminator split + ImageSolver solve of a master, saving `stars.fits` / `starless.fits` with the same `writeWcsKeywords` Y-flip, plus a local Gaia TSV), `test/unit/test_real_parity.cpp` (skips with 77 when fixtures are absent)

**Interfaces:**
- Uses masters `masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-HaO3_RGB.xisf` and `…FILTER-Full_RGB.xisf`, plus a mono extraction of the HaO3 master (Review Focus 2).
- Steps: freeze the inputs once, run `make_goldens.py real` on them, then run the C++ core with `TanSky` from the same FITS header and the same catalogue.

- [ ] **Step 1: Failing test** `real_parity_end_to_end`. For each fixture:
  - DAO table identical (count; x/y ≤ 1e-6 px);
  - match set identical;
  - halo profile ≤ 1e-9;
  - `rendered_stars` and `nebula_out` max |Δ| ≤ 1e-6;
  - QA numbers equal.

  Also `in_pi_match_not_worse`: Task 16's in-PI run (full spline solution) gives match_rate ≥ Python's and median offset ≤ Python's.
- [ ] **Step 2: Run** `bash test/run-real-parity.sh`. Expected: FAIL.
- [ ] **Step 3: Fix discrepancies in the owning task's code**, never in the tolerance.
- [ ] **Step 4: Run.** Expected: PASS. Record the numbers in `test/golden/REAL_PARITY.md`.
- [ ] **Step 5: Commit** `test(gdg): real-data parity on frozen NAS masters`.

### Task 20: Docs, sidecar removal, release

**Files:**
- Create: `modules/gaia-depth-grade/doc/GaiaDepthGrade/GaiaDepthGrade.html`. Port `gaia-depth-grade/doc/GaiaDepthGrade/GaiaDepthGrade.html`, updating:
  - the CPD 6.1 network section (the exact endpoints from Task 13, offline = none);
  - "Programs it launches: none";
  - "Files it writes": the online cache directory only;
  - plate solving (native; metadata or astrometric index);
  - the minimum PI version 1.9.6.
- Modify: `release.sh` (remove `GAIA_ZIP`, `SIDECAR_TGZ`, the `GaiaDepthGrade`/`GaiaDepthGradeSidecar` `PKG_DESC` entries, step 2c script signing and step 3c packaging; add the module to the 1.9.6 platform block), `tools/test-release-manifest.sh` (two platform blocks), `repository/updates.xri` (via release.sh), `CLAUDE.md` ("What ships" bullet), `README` mentions.
- Move: `gaia-depth-grade/` → `archive/gaia-depth-grade-python/` (D8), keeping `test/golden/` committed in the module.

- [ ] **Step 1: Failing checks:** `bash tools/test-release-manifest.sh` with the new expectations (no sidecar or script package; the GaiaDepthGrade module is in a `1.9.6:2.0.0` block); `release.sh --package-test` lists the module.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Then confirm on a scratch slot what the 1.9.6 updater does with the old installed sidecar and scripts once their packages leave the manifest. Report the result to Scott; do not guess.
- [ ] **Step 4: Run the full gate:** container build with ctest, ASan host build of `gdg_core` tests, `verify-portable.sh`, every probe in `run-probe.sh`, `gui-smoke.sh`, `run-real-parity.sh`, `run-probe-recon.sh` and `test_recon_real` (executed on the frozen NAS fixtures, Task 26), the Feature Parity Checklist and the Phase G2 table ticked with evidence. Then `./release.sh`. Expected: green, with a signed manifest.
- [ ] **Step 5: Commit and push** `release(gdg): GaiaDepthGrade 1.0.0.0 native module; retire sidecar + scripts`. Scott then runs the repo-pull test.

---

## Feature Parity Checklist

Every item must be ticked with its test before Task 20 Step 4. "Removed" items are internal artifacts with no user-visible surface. They are listed so that dropping them is a deliberate, reviewed choice.

### Dialog: target and flow

| # | Feature (source) | Default / detail | Task | Test |
|---|---|---|---|---|
| F1 | Target view list, main views; defaults to the active window | — | 17 | gui `controls_and_defaults` |
| F2 | Browse… opens an image file as the target | — | 17 | gui `controls_and_defaults` (opens a fixture) |
| F3 | Prepare works on duplicate `<id>_gdgwork` (keywords carried); master untouched | — | 16 | `master_never_modified` |
| F4 | Prepare = solve + StarXTerminator split + detect + Gaia + match | — | 14–16 | `execute_on_offline` |
| F5 | Preview / Update: recombined, autostretched preview ≤ 760 px wide in a 760×420 control | target_bg 0.25, shadows −2.8 | 10, 17 | `autostretch_matches_golden`, gui |
| F6 | Stale-preview orange border | — | 17 | gui `slider_marks_preview_stale` |
| F7 | 1:1 inset 256 px; click moves its centre; defaults to the image centre | — | 17 | gui `inset_click_moves_center` |
| F8 | Execute → new window `<targetId>_depthgraded`, shown | — | 16, 17 | `output_naming_and_tag`, gui |
| F9 | Close button | — | 17 | gui |
| F10 | Preview/Execute disabled until prepared; prepare-time edits re-disable them | — | 17 | gui `detect_edit_marks_prepare_stale` |
| F11 | Status line, QA summary text, LOW MATCH flag, `ERROR:` messages, `[GDG]` console lines | — | 16, 17 | gui, `prepare_matches_golden_qa` |

### Dialog: controls

| # | Control | Default [range] | Task | Test |
|---|---|---|---|---|
| F12 | Brightness | 0.65 [0,2] | 16, 17 | gui `controls_and_defaults` |
| F13 | Size | 0.85 [0,2] | 16, 17 | gui `controls_and_defaults` |
| F14 | Contrast | 0.12 [0,2] | 16, 17 | gui `controls_and_defaults` |
| F15 | Saturation | 0.4 [0,2] | 16, 17 | gui `controls_and_defaults` |
| F16 | Halo sigma | 2.0 [0.5,6] | 16, 17 | gui `controls_and_defaults` |
| F17 | p_low | 5 [0,50] | 16, 17 | gui `controls_and_defaults` |
| F18 | p_high | 95 [50,100] | 16, 17 | gui `controls_and_defaults` |
| F19 | Nebula checkbox | off | 16, 17 | gui `controls_and_defaults` |
| F20 | Nebula depth | 1.0 [0,2] | 16, 17 | gui `controls_and_defaults` |
| F21 | atmospheric | 1.0 [0,2] | 16, 17 | gui `controls_and_defaults` |
| F22 | structure | 1.0 [0,2] | 16, 17 | gui `controls_and_defaults` |
| F23 | detect_threshold_sigma | 6 [1,30] | 16, 17 | gui `controls_and_defaults` |
| F24 | match_tolerance_px | 4 [1,12] | 16, 17 | gui `controls_and_defaults` |
| F25 | Offline checkbox (on) with tooltip text | on | 16, 17 | gui `controls_and_defaults` |

### Engine behaviour

| # | Behaviour | Detail | Task | Test |
|---|---|---|---|---|
| F26 | Local Gaia DR3 cone | G < 18, S/N ≥ 5, loud drop counts, DR3/DR3SP discovery and error texts | 12 | `local_gaia_equals_gaia_process`, `snr_cut_logs_drop_count` |
| F27 | Online Bailer-Jones | async TAP, G < 18, 3 attempts, 600000-row-cap warning, per-field cache, notices to the console | 13 | `tap_*`, `online_live` |
| F28 | Plate solve every run using metadata | existing solution, else keywords (port of `extractImageMetadata`) | 14 | `solve_seeded_matches_imagesolver` |
| F29 | Blind solve when there is no metadata | what 1.9.6's ImageSolver provides | 15 | `solve_blind_no_metadata` |
| F30 | Distortion-accurate solution | spline | 15 | `solve_spline_residuals` |
| F31 | StarXTerminator split | stars = true, unscreen = true; missing-SXT message | 16 | `split_requires_sxt_message` |
| F32 | DAO detection | fwhm 3, 6σ, sigma-clipped background | 5, 6 | `dao_matches_photutils_synthetic`, real parity |
| F33 | Cross-match | 4 px; closest per detection; QA; LOW MATCH < 0.3 | 11 | `match_matches_golden` |
| F34 | Depth strength | log-distance percentiles, confidence damping, neutral for unmatched | 7 | `strength_matches_golden` |
| F35 | Brightness feather | Hann taper; no "little squares" | 9 | `render_matches_golden_*` |
| F36 | Star-shine halo | empirical from the frame's bright, unsaturated, isolated stars; tinted; one-sided; Moffat fallback with warning | 8, 9 | `halo_*`, `render_far_star_never_negative_ring` |
| F37 | Noise-cored local contrast | — | 9 | `render_matches_golden_*` |
| F38 | Saturation modulation | colour only | 9 | `render_matches_golden_colour` |
| F39 | Nebula grade | Gaia budget, structural depth, atmospheric brightness/colour/tilt, structural pop | 10 | `nebula_matches_golden_*` |
| F40 | Screen-blend recombine | ~((~A)*(~B)) | 10, 16 | `screen_blend_matches` |
| F41 | Output clipped [0,1], 32-bit float | — | 9, 10, 16 | goldens |
| F42 | Honesty tag | `DEPTHTAG` + HISTORY (the sidecar wrote it on its intermediate FITS; the module puts it on the output window) | 16 | `output_naming_and_tag` |
| F43 | Every engine knob previously reachable through the sidecar's `--config` TOML | detect_fwhm, min_match_rate, neutral_strength, gaia_mag_limit (incl. None), gaia_parallax_snr, halo_min_refs, halo_radius_sigma, halo_moffat_beta, halo_gain, p_low/p_high, base_sigma | 16 | probe sets each parameter and checks it reaches the core (`params_reach_core`) |

### Headless script, platform, docs, recon

| # | Feature | Detail | Task | Test |
|---|---|---|---|---|
| F44 | Headless `gaia_depth_grade.js` on the active window or `GAIA_DEPTH_INPUT`, saved to `GAIA_DEPTH_OUTPUT` | Replaced by process ExecuteOn: process icon, ImageContainer with `outputMode=ReplaceTarget`, or PI's command line. Its defaults (0.5/0.4/0.3/0.3, online) are parameter values. Its in-place starless side effect on the input was a defect and is not reproduced. | 16 | `replace_target_mode`, `execute_on_offline` |
| F45 | Linux x64 only | unchanged (container build) | 2 | `verify-portable.sh` |
| F46 | Documentation: what it does, network, programs, files | — | 20 | doc review in the parity sign-off |
| F47 | Menu location `scarter4work > Gaia Depth Grade` | process category `scarter4work` | 16 | probe `process_category` |
| F48 | Recon core (branch) | functions and defaults | 18 | `test_recon_*` |

### Recon Part 2 (Phase G2): the Python plan's Tasks 9–13, never shipped in Python

| # | Feature (Python plan task) | Detail | Task | Test |
|---|---|---|---|---|
| G1 | 2D render of the model (9) | occlusion, limb-brightening, depth-keyed shading; registered, linear [0,1]; flat model = identity | 21 | `render2d_*` |
| G2 | `reconstruct` subcommand + multi-master reader (10) | SHO / duoband / OSC, Hβ, ionizer override, co-registration error, stretched-input degrade, honesty tag | 22, 25 | `reconstruct_*`, `recon_execute_global_sho`, `recon_command_line_files` |
| G3 | Portable model (10, `.npz`) | model windows + properties, saved XISF, re-render/export from them | 25 | `recon_model_views_roundtrip` |
| G4 | GLB export + `export3d` (11) | decimated emissive surface + dust layer, honesty in extras | 23, 25 | `glb_*` |
| G5 | Orbit animation (12) | 36 frames, ±12°, animated GIF | 24, 25 | `orbit_*`, `gif_*` |
| G6 | Real-data validation (13) | M42/M8 duoband + broadband; metrics (a)–(c); Scott's before/after sign-off | 26 | `test_recon_real`, `RECON_VALIDATION.md` |

### Removed internal artifacts (deliberate)

| # | Artifact | Why it goes |
|---|---|---|
| R1 | Private work dir `~/.cache`-style `gdgWorkDir` with `stars_full.fits`, `starless_full.fits`, `preview_full.png`, `preview_inset.png`, `prep.npz`, `qa.json`, `prepare.toml`, `gaia_local.tsv`, `graded_full.fits`, `nebula_full.fits` | Replaced by in-memory state. Only the online cache directory remains (F27). |
| R2 | Sidecar `--version` handshake, `footprint` subcommand, `debug` subcommand | `debug` was unimplemented; the others were plumbing. |
| R3 | `fwhm` column of the detection table | Never read (verified). |

## Schedule and critical path

Sizes are focused days per task:

| Task | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Days | 1 | 2 | 1 | 0.5 | 1 | 3.5 | 0.5 | 1 | 1.5 | 1.5 | 1.5 | 1.5 | 2.5 | 3 | 3 | 2.5 | 3 | 3 | 1.5 | 2 |

That is **37 days serial**, which **does not fit** 2026-12-12 (32 calendar days from 2026-11-10).

Run as tracks after Tasks 1–2:

| Track | Tasks | Done by about |
|---|---|---|
| A, stars | 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 | day 13 |
| B, sky | 11 → 12 → 13 → 14 → 15 (Task 11 needs only Task 3 and the Pipeline types) | day 14; solver gate 2026-11-21 |
| C | 18 (after Task 4), any time | — |

Then 16 → 17 → 19 → 20 on top of both tracks: **about 19 days of critical path**. That gives a **release candidate by 2026-12-01** and about 11 days for Scott's repo-pull test and fixes.

The deadline holds **only** with parallel tracks (subagent-driven, at least 2 concurrent implementers on disjoint files) and the solver gate passing. If the gate fails, the D2 contingency costs about 1 day but needs Scott's approval. If the critical path slips past 2026-12-04, Task 18 (recon) moves out of the release first. It ships nothing user-visible.

### Re-plan with Phase G2 (2026-11-12)

State on 2026-11-12, from the branches:
- Tasks 1–15 and 18 are committed (`gdg/phaseB…E`, `gdg/phaseG-recon`, merged into `gdg/phaseE-solver`); the solver gate passed.
- Tasks 16–17 are in progress, uncommitted, on `gdg/phaseF-process`.
- Left: 16, 17, 19 and 20, about 9 focused days.

| Task | 21 | 22 | 23 | 24 | 25 | 26 |
|---|---|---|---|---|---|---|
| Days | 1.5 | 1 | 1.5 | 2 | 3 | 2.5 |

Phase G2 adds **11.5 days**; serial, everything left is about 20.5 days. Tracks from 2026-11-13:

| Track | Tasks | Needs |
|---|---|---|
| M | 16 → 17 → 19 | — |
| R (PCL-free, starts now) | 21 → 22, then 23 ∥ 24 | Task 18 only |
| then | 25 → 26 → 20 | 16 and 21–24; 26 also needs PixInsight time for four integrations |

Critical path: R (~4.5 days) ∥ 16, then 25 + 26 + 20 ≈ **12 focused days**, so a release candidate about **2026-11-28** with two concurrent implementers. That leaves about two weeks for Scott's repo-pull test and the Task 26 sign-off.

Valve: if Task 25 or 26 is not green by 2026-12-04, `NebulaReconstruct` is not registered in the 12-12 build (Tasks 21–24 stay in `gdg_core`, invisible), and Part 2 ships in the next release.
