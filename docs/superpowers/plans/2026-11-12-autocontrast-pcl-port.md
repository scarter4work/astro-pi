# AutoContrast PCL Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace AutoContrast's Python sidecar and PJSR scripts with one native PCL module, `AutoContrast-pxm.so` (processes `AutoContrast` and `AutoContrastReferences`), that does everything they do, matches the Python reference numerically, and loses no functionality.

**Architecture:** A PCL-free C++20 library, `ac_core` (`modules/autocontrast/src/core/`), ports the Python package line for line in double precision, row-major, y = 0 = top row. Catch2 tests compare it with goldens dumped from the Python reference. A thin PCL layer (`src/module/`) adapts PixInsight to it: view pixels (`ImageBridge`), the astrometric solution, stock processes as the optimizer's executor (`PiExecutor`, `ProcessInstance`), `NetworkTransfer` (the only network code), consent, and the two processes with their interfaces. A dev CLI (`src/tool/ac-tool`) exposes the Phase 0 evaluation harness. Gaia Depth Grade's native plate solver is factored into `modules/shared/platesolve/` and reused for WCS tier 3.

**Tech Stack:** C++20, PCL 2.13.0 build 1072 (Rocky 9 container, `tools/build-env`), SQLite 3.53.4 (repo `third_party/sqlite`), nlohmann/json (FetchContent, PI Copilot's pin), libjpeg-turbo / libpng / libtiff (FetchContent, SHA-256 pinned, static `-fPIC`), Catch2 (NukeX's amalgamated copy), Python 3.14 reference in `tools/autocontrast/.venv` (golden generation only), `tools/pi-headless.sh`.

**Spec:** `docs/superpowers/specs/2026-11-12-autocontrast-pcl-port.md` (read sections 5–12 before any task). Python reference: `tools/autocontrast/src/autocontrast/` at the commit recorded in `modules/autocontrast/test/golden/MANIFEST.json`. Design source of truth: `tools/autocontrast/autocontrast-design-v0.1.md` §2 and §12 (non-negotiable).

## Global Constraints

- No functionality lost: every row of the Feature Parity Checklist (end of plan) is green before Task 23.
- No shipped Python, PJSR or external program launched by the module. Python is dev-only golden tooling; in-PI tests drive process command lines from a generated one-line `.scp` (never PJSR) through `tools/pi-headless.sh`.
- Parity defaults (a task may tighten, never loosen without a recorded measurement): primitives bit-exact or ≤ 1e-12; fingerprint components ≤ 1e-9 relative; D ≤ 1e-9 absolute; EMD ≤ 1e-9 absolute plus a dual certificate; loop runs identical in every discrete decision.
- Constants copied verbatim and cited by Python file:line: weights 0.40/0.25/0.20/0.15; `_STRENGTH` gentle 0.25 / moderate 0.5 / strong 0.85; `GAP_GENTLE_MAX 0.075`, `GAP_MODERATE_MAX 0.15`; `BeamConfig(width 3, top_k 3, iteration_cap 20, convergence_window 3, epsilon 1e-3, epsilon_improve 1e-3, max_attempts 3·top_k)`; `GuardrailLimits(0.15, 1e-4, 1e-4, 0.05, 0.20, 0.20, 1e-3, 20.0, 0.10)`; `HUE_CHROMA_FLOOR 1.0`; `NUM_HUE_BINS 360`; `CHROMA_BINS 16`, `CHROMA_EXTENT 100.0`; quantiles `[1,5,10,25,50,75,90,95,99,99.9]`; `max_dim 1600`; `n_scales 7`; `psf 2.0`; crawl `1.0 s + U(0,0.25)`, `8000` requests, `256 MiB`, `3` retries, `DRIFT_THRESHOLD 0.5`, `SYNC_SAFETY_DAYS 7`, `RESULTS_PER_PAGE 50`, `EPOCH_YEAR 1990`; discover tolerance `0.25 · radius`, floor `1.0′`.
- Never re-tune a calibrated value (phase2 spec §3.7). Open calibration questions are recorded, not tuned around.
- Ids stable once shipped: module `AutoContrast`, processes `AutoContrast` and `AutoContrastReferences`, category `scarter4work`, the parameter ids in spec §8. Test-only process `AutoContrastProbe` exists only with `-DAC_BUILD_PROBES=ON`.
- Network only to `https://esahubble.org`, `https://www.eso.org`, `https://cdn.esahubble.org`, `https://cdn.eso.org`; GET only; `SetSSL(true,true,true,true)`; User-Agent `AutoContrast/<version> (PixInsight module; +https://github.com/scarter4work/astro-pi)`; opt-in `allowNetwork = false`; consent version 1.
- Never modify the user's view. Optimize writes a new window `<id>_autocontrast` (full-resolution replay) or nothing.
- Threading: `ac::parallel_for` (semantics of `modules/nukex/src/lib/gpu/include/nukex/gpu/parallel_for.hpp`), no OpenMP, deterministic chunked reductions (bit-identical at 1 and N threads), no mutable statics. `pcl::View`/`ImageWindow` handles never outlive their window.
- Build: host dev builds `cmake -B build -DPCLDIR=$HOME/PCL -DAC_BUILD_MODULE=ON && cmake --build build -j8`; ctest `-j8`. Shipped/probed module = container build (`tools/build-env/build-modules.sh`); check `podman ps` and never start a second container build while one runs. Build dirs and large outputs stay in the worktree or `~/projects`, never `/tmp`.
- Every task: TDD (test first, seen failing), green ctest, spec + code-quality review, commit ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, push `ac/pcl-port`. No force-push, no release, no `updates.xri` change, no merge to `main`, no commit on `gdg/*` or `feat/*`.
- `/tmp/.pi_codesign_pass` missing → recreate it from memory note `ez_stretch_bsc_signing` before any signing probe.

## Assumed decisions

Spec section 12 (A1–A9) plus:

| # | Decision | Tasks |
|---|---|---|
| D1 | Golden real-data inputs are the 10 Phase 0 renders + the 35 cached discovery renders on this box; only derived goldens (fingerprints, verdicts, small arrays) are committed, never the renders (CC BY images are gitignored today) | 1, 5, 10, 11, 20 |
| D2 | `ac-tool` (dev CLI) replaces `python -m autocontrast.eval.ranking` and `…eval.degrade`; `…db.discover sync` becomes `AutoContrastReferences --sync`. `ac-tool` is built and tested, never packaged | 17, 20 |
| D3 | Linear-input refusal: the target is "linear" when the median of L* < 5.0 **and** p99.9 of L* < 30.0 on the proxy (a linear master's sky sits near 0 and its stars barely register in L*); calibrated in Task 16 on the M42 linear stack, the NAS masters (linear), `eso1103a` and the finished print (stretched) — the thresholds must separate all of them with margin, else the task stops and reports | 16 |
| D4 | §3.6 replays at full resolution with scale-denominated parameters recomputed from the full-res pixel scale (MLT layer = `layer_for_scale(scale_arcsec, fullres_scale)`, LHE radius in full-res pixels), then downsamples with the same resampler before measuring | 16 |
| D5 | Proxy built by the Pillow-exact float resampler (spec 5.1 step 6), not PI `Resample` | 10, 16 |

## Review Focus

1. **Mono or two-channel target view.** Promoted to three channels as `load_raster` does; chroma terms go to 0, the hue guardrail trips on any invented colour; Analyze and Optimize run end to end. Test: Task 16 `probe_mono_view_runs`.
2. **Tiny or extreme-aspect images** (64×48, 1600×40): starlet kernels wider than the axis (repeated mirror), LHE radius below 16 px, `local_contrast` layer above `floor(log2(min(H,W)))`. Expected: no hang, executor failures logged per candidate, run completes. Tests: Task 4 `starlet_kernel_wider_than_image`, Task 9 `approx_executor_rejects_unsupportable_layer`, Task 15 `probe_lhe_radius_below_16_logged`.
3. **NaN/Inf or out-of-range pixels in the target view.** Refused with an error that names the count (Planetary's rule), never silently zeroed. Test: Task 16 `probe_nonfinite_refused`.
4. **Store or index unusable** (path missing → created; read-only; corrupt record JSON; written by Python). Expected: Python-written DBs open unchanged; read-only/corrupt → a loud error naming the file. Tests: Task 12 `store_opens_python_written_db`, `store_corrupt_record_errors`, `store_readonly_errors`.
5. **User aborts mid-run.** `session.json` holds the last completed iteration, no new window, no hidden window leaked. Test: Task 16 `probe_abort_leaves_no_windows`.

---

## File structure

```
modules/autocontrast/
  CMakeLists.txt                 # AC_BUILD_MODULE (OFF), AC_BUILD_TESTS (ON), AC_BUILD_PROBES (OFF), AC_BUILD_TOOL (ON)
  cmake/PCLConfig.cmake          # copy of modules/gaia-depth-grade/cmake/PCLConfig.cmake
  cmake/Deps.cmake               # sqlite (third_party), nlohmann_json, libjpeg-turbo, libpng, libtiff
  package-description.xml  THIRD_PARTY_NOTICES
  doc/AutoContrast/AutoContrast.html  doc/AutoContrastReferences/AutoContrastReferences.html
  src/core/                      # ac_core (namespace ac)
    Image.h  Parallel.h  Json.h
    num/  Percentile.{h,cpp} Interp.h Histogram.{h,cpp} Convolve.{h,cpp} Gaussian.{h,cpp} Label.{h,cpp}
    fingerprint/ Starlet Energy BandLimit Color Metrics Palette Extract Emd Distance .{h,cpp}
    optimize/ Actions Recipe Propose Guardrails Beam Loop Session ApproxExecutor .{h,cpp}  Executor.h
    io/ Raster.{h,cpp} Resample.{h,cpp} Xmp.{h,cpp}
    db/ Records Store Index Skymath Wcs Avm Ingest Seed .{h,cpp}
    discover/ Gallery Fetch Crawl Discover .{h,cpp}
    eval/ Ranking Degrade .{h,cpp}
  src/module/                    # PCL glue (namespace pcl)
    AutoContrastModule.{h,cpp} AutoContrastVersion.h pixinsight-module.map
    AutoContrast{Process,Parameters,Instance,Interface}.{h,cpp}
    AutoContrastReferences{Process,Parameters,Instance,Interface}.{h,cpp}
    ImageBridge PiExecutor TargetWcs PclTransport NetworkConsent RunReport .{h,cpp}
    probe/ AutoContrastProbe{Process,Instance}.{h,cpp}   # AC_BUILD_PROBES only
  src/tool/ac-tool.cpp
  test/ CMakeLists.txt  util/Npy.h  unit/test_*.cpp  golden/make_goldens.py golden/MANIFEST.json golden/synthetic/ golden/real/
        fixtures/gallery/*.html  harness.sh  run-probe.sh  run-probe-live.sh  gui-smoke.sh  run-exit-criterion.sh
modules/shared/platesolve/       # Task 19: GDG's solver, factored out
```

**Frame contract.** `ac::Image { int width, height, channels; std::vector<double> data; }`, `data[(y*width + x)*channels + c]`, y = 0 = top row = numpy row 0 of a Pillow-decoded raster = PixInsight row 0. `ac::Plane` is the one-channel form. No flips anywhere except the FITS-header WCS mapping proven in Task 11/16.

---

## Phase A: Foundation

### Task 1: Golden generator and goldens (Sonnet)

**Files:** Create `modules/autocontrast/test/golden/make_goldens.py`, `golden/MANIFEST.json`, `golden/synthetic/*`, `golden/real/*` (derived only, ≤ 8 MB total), `test/util/Npy.h`, `test/unit/test_npy.cpp`. Modify `tools/autocontrast/pyproject.toml` only if a dev dependency is missing from it (it is not: `pypng` is declared; install it into the venv).

**Interfaces:**
- Produces `make_goldens.py synthetic <outdir>` and `make_goldens.py real <outdir>`; runs as `tools/autocontrast/.venv/bin/python -I`, puts `tools/autocontrast/src` on `sys.path` explicitly, fails loudly if any stage raises, records `git rev-parse HEAD` of `tools/autocontrast`, numpy/scipy/astropy/Pillow/tifffile/pypng/pyavm versions and Pillow's libjpeg-turbo version in `MANIFEST.json`.
- Produces `ac::test::NpyArray { std::vector<size_t> shape; std::vector<double> data; std::string dtype; }`, `NpyArray load_npy(const std::string&)` (`<f8 <f4 <i8 <u2 |u1 |b1`, C order, v1/v2), plus `nlohmann::json load_json(const std::string&)`.
- **Synthetic stages** (seeded `np.random.default_rng(20261112)`): primitive goldens (`percentile` on 1001/1000 random values at every QUANTILE_LEVEL and at 0/100; `median` odd/even; `interp` incl. out-of-range; `histogram` uniform with values on edges; `histogram2d` weighted; `convolve1d_mirror` for kernel widths 5, 9, 17, 257 on 7×5 and 64×48; `gaussian_filter` σ ∈ {0.5, 1, 2.355, 3, 8}; `label` on a random mask); `starlet` planes+residual for n_scales 1/5/7 on 96×80, 64×48, 7×5; `energy`, `band_limit` (usable, `hi <= lo`, `< min_bins`); `rgb_to_lab`; every metric; `extract` → `fingerprint.json`; `chroma_distance` / `emd` on 20 random histogram pairs incl. sparse (90 % zero bins) and neutral; `fingerprint_distance` incl. palette mismatch and unusable band; `available_actions`, `propose_actions` (top_k 3 and None) for 12 (ref, target, applied_kinds) cases; every guardrail verdict on synthetic images built to trip exactly one; `advance`/`run_to_convergence` with `NumpyExecutor` on (a) a 160×120 synthetic "nebula" vs its own fingerprint (must decline), (b) `flatten(synthetic, 0.6)` vs the original, (c) a guardrail-hostile input — recording every iteration's beam, `distance_history`, `guardrail_log`, `outcome`; `flatten`/`over_sharpen`/`desaturate`/`oversaturate`; `palette_class_from_filters`/`_gallery_bands` over a 40-row table; gallery parse results for the six fixture HTMLs; `separation_arcmin` table; `_geometry` on 4 synthetic TAN WCS (rotated, flipped, CD and CDELT forms).
- **Real stages** (D1): for each of the 5 references, 5 amateurs and 35 cached renders: `load_raster(max_dim=1600)` → shape + SHA-256 of the float64 bytes + 64×64 centre crop `.npy`; Pillow decode of the full-res first 16 rows (`.npy`, uint8); `image_dimensions`; pyavm `AVM.from_image` → `{present, ok, error_type, wcs:{crpix,crval,cd}}`; `_from_avm` → `WcsResult`; `extract` → fingerprint JSON; leave-one-out and local-minimum reports (`eval.ranking`, `eval.degrade`) as JSON.

- [ ] **Step 1:** Install `pypng` into `tools/autocontrast/.venv`; run `.venv/bin/python -m pytest -q -m 'not live'` there. Expected: 384 passed (the 16-bit PNG failure was the missing package). Record the count in the task report.
- [ ] **Step 2: Write `test_npy.cpp`:** `load_npy` round-trips `synthetic/npy_selftest_f8.npy` (shape (3,2), values 0..5) and `npy_selftest_b1.npy`; a truncated file throws `std::runtime_error`. Build it with a throwaway `add_executable` against Catch2 until Task 2 lands the CMake (or do Task 2 Step 3 first). Run → FAIL (no files).
- [ ] **Step 3: Implement** `make_goldens.py` and `Npy.h`; generate `synthetic/` and `real/`.
- [ ] **Step 4: Run** the generator twice and `diff -r` the outputs (deterministic); `test_npy` → PASS; `du -sh golden` ≤ 8 MB.
- [ ] **Step 5: Commit** `test(autocontrast): golden generator + synthetic and derived real-data goldens from the Python reference`.

### Task 2: Module skeleton, build wiring, ids, `.scp` probe route (Sonnet)

**Files:** Create `modules/autocontrast/CMakeLists.txt`, `cmake/PCLConfig.cmake`, `cmake/Deps.cmake` (sqlite + nlohmann_json now; image libs in Task 10), `src/core/{Image.h,Parallel.h,Json.h}`, `src/core/CMakeLists.txt` (`ac_core` static, PCL-free, `-fPIC`), `src/module/{AutoContrastModule,AutoContrastVersion.h,pixinsight-module.map,CMakeLists.txt}`, empty `AutoContrastProcess`/`AutoContrastReferencesProcess` (ids registered, `ExecuteOn`/`ExecuteGlobal` throw "not implemented in this build" until Tasks 16/17 — removed there), `src/module/probe/AutoContrastProbe{Process,Instance}` (command line `AutoContrastProbe --case=<id> --out=<json>`), `test/CMakeLists.txt` (`ac_add_test(name src libs...)` modelled on `gdg_add_test`, `AC_TEST_DIR`), `test/harness.sh` (slot 94, private tmp under the worktree, `ac_require_signing_pass` → exit 2, `ac_run_scp <case> <one-line>`), `test/run-probe.sh`, `test/unit/test_version.cpp`, `test/unit/test_parallel.cpp`.
**Modify:** `tools/build-env/build-modules.sh` (configure/build/ctest `modules/autocontrast` with `-DAC_BUILD_MODULE=ON`; add `--only <module>` and `BUILD_JOBS` (default `nproc`) so a task can build just AutoContrast at `-j8`; fail if `AutoContrastProbe` is in the shipped `.so`; a second probe build dir `build-portable-probes`), `tools/build-env/verify-portable.sh` (include `AutoContrast-pxm.so`).

**Interfaces:**
- Produces `AC_MODULE_VERSION_{MAJOR,MINOR,RELEASE,BUILD}` = 0.1.0.0, `AC_VERSION_STRING`; module id `"AutoContrast"`, category `scarter4work`.
- Produces `ac::parallel_for(size_t n, F&& body)` (dynamic chunks, `std::thread`, exceptions rethrown on the caller) and `template<class F> double ac::deterministic_sum(size_t n, F&& term)` (fixed 4096-element chunks, partials combined in chunk order).
- Produces `ac::Json` = `nlohmann::json` alias and `std::string ac::dump_python(const Json&)` matching Python `json.dumps` (separators `", "`/`": "`, shortest round-trip floats, `Infinity`/`NaN` literals as Python writes them).

- [ ] **Step 1: Failing checks.** `test_parallel`: `deterministic_sum` over 1e6 terms gives bit-identical results with 1, 3 and 32 threads; `parallel_for` propagates an exception. `test_version`: version macros parse. `run-probe.sh` case `ids`: the container `.so` (probe build), signed and loaded with `-m=`, reached through `-r=<run-dir>/ids.scp` holding `AutoContrastProbe --case=ids --out=…`, writes JSON with `module == "AutoContrast"`, processes `["AutoContrast","AutoContrastReferences","AutoContrastProbe"]`, category `scarter4work`, and `--force-exit` returns. **`scp_launch_reaches_probe` is the first case; if `.scp` does not run under `--automation-mode`, stop and report (no PJSR fallback).**
- [ ] **Step 2: Run** → FAIL (nothing exists).
- [ ] **Step 3: Implement** the skeleton and wiring.
- [ ] **Step 4: Run** host build + ctest (`-j8`), then `tools/build-env/build-modules.sh --only autocontrast` (container, `BUILD_JOBS=8`), `verify-portable.sh`, `bash modules/autocontrast/test/run-probe.sh ids`. Expected: all PASS; `python3 tools/check-pi-launches.py` clean.
- [ ] **Step 5: Commit** `build(autocontrast): module skeleton, container/portable wiring, process ids, .scp probe route`.

## Phase B: Numerics and fingerprint (`ac_core`, PCL-free)

### Task 3: numpy/scipy-exact primitives (Sonnet)

**Files:** Create `src/core/num/{Percentile,Histogram,Convolve,Gaussian,Label}.{h,cpp}`, `num/Interp.h`, `test/unit/test_num.cpp`.

**Interfaces (Produces):**
- `double percentile(std::vector<double> v, double q)`; `std::vector<double> percentiles(std::vector<double> v, const std::vector<double>& qs)` (one sort-free selection per q is fine; must equal numpy 2.x `method="linear"` bit for bit: virtual index `(n−1)·(q/100)`, `_lerp(a,b,t) = t < 0.5 ? a + (b−a)·t : b − (b−a)·(1−t)`); `double median(std::vector<double> v)`.
- `double interp(double x, const std::vector<double>& xp, const std::vector<double>& fp)` (clamps outside).
- `std::vector<double> histogram_uniform(const double* x, const double* w /*nullable*/, size_t n, int bins, double lo, double hi)` (numpy fast path + edge corrections; NaN dropped as numpy does).
- `std::vector<double> histogram2d(const Plane& a, const Plane& b, const Plane& w, int bins, double extent)` → row-major `bins×bins`, index `[ia*bins + ib]`.
- `Plane convolve1d_mirror(const Plane&, const std::vector<double>& kernel, int axis)`; `Plane gaussian_filter(const Plane&, double sigma)` (reflect, truncate 4.0).
- `struct Box { int y0, y1, x0, x1; }` (half-open); `struct Labels { int count; std::vector<int> label; std::vector<Box> boxes; }`; `Labels label4(const std::vector<uint8_t>& mask, int w, int h)` (scipy label order).

- [ ] **Step 1: Tests** `test_num.cpp`: each function vs its `synthetic/` golden — `percentile_matches_numpy_bitwise` (REQUIRE `==`), `median_even_odd`, `interp_clamps`, `histogram_edge_values_match_numpy`, `histogram2d_matches_numpy` (bins equal, sums ≤ 1e-12 rel), `convolve1d_mirror_kernel_wider_than_axis` (≤ 1e-14), `gaussian_matches_scipy` (≤ 1e-12), `label4_matches_scipy` (labels and boxes equal), `reductions_thread_invariant`.
- [ ] **Step 2: Run** `ctest -R test_num` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(ac-core): numpy/scipy-exact primitives with golden parity`.

### Task 4: Starlet, energy spectrum, band-limiting (Opus)

**Files:** Create `src/core/fingerprint/{Starlet,Energy,BandLimit}.{h,cpp}`, `test/unit/test_starlet.cpp`, `test/unit/test_bandlimit.cpp`.

**Interfaces (Produces):**
- `struct Starlet { std::vector<Plane> planes; Plane residual; }`; `Starlet starlet_transform(const Plane& image, int n_scales)` (throws `std::invalid_argument` for `n_scales < 1`, message as Python).
- `struct EnergySpectrum { std::vector<double> centers_arcsec, energy; }`; `EnergySpectrum energy_spectrum_from_planes(const std::vector<Plane>&, double pixel_scale_arcsec)`.
- `struct BandLimitResult { double lo_arcsec, hi_arcsec; bool usable; std::string reason; std::vector<double> grid_arcsec, ref_shape, target_shape; }`; `std::pair<double,double> comparable_band(...)`; `BandLimitResult band_limit(const EnergySpectrum& ref, const EnergySpectrum& target, double ref_psf, double target_psf, int min_bins = 3)` (reason strings with Python's `%.3g` formatting).

- [ ] **Step 1: Tests:** `starlet_planes_match_scipy` (all n_scales/sizes, ≤ 1e-12), `starlet_reconstructs` (`Σplanes + residual == image` ≤ 1e-12), `starlet_kernel_wider_than_image` (7×5 at n_scales 7, matches golden), `starlet_thread_invariant` (bitwise), `energy_centers_are_2pow_i_times_scale`, `band_limit_usable_matches_golden`, `band_limit_no_overlap_reason`, `band_limit_too_few_bins_reason`, `starlet_1600px_7scales_under_300ms` (single-threaded, Release build only; tagged `[perf]`, runs in ctest).
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement** (separable passes; the column pass in cache blocks; arithmetic order = scipy's tap order). **Step 4: Run** → PASS. **Step 5: Commit** `feat(ac-core): starlet transform, energy spectrum, band-limiting (parity)`.

### Task 5: CIELAB, metrics, palette, extract, fingerprint JSON (Sonnet)

**Files:** Create `src/core/fingerprint/{Color,Metrics,Palette,Extract}.{h,cpp}`, `test/unit/test_fingerprint.cpp`, `test/unit/test_palette.cpp`.

**Interfaces (Produces):**
- `void rgb_to_lab(const Image& rgb, Plane& L, Plane& a, Plane& b)` (spec 6.3: fresh port; NukeX's rounded constants are not used).
- `extern const std::array<double,10> kQuantileLevels`; `std::array<double,10> tonal_quantiles(const Plane& L)`, `saturation_quantiles(const Plane& C)`; `std::vector<double> chroma_histogram(const Plane& a, const Plane& b, int bins = 16, double extent = 100.0)`; `double floor_to_peak_ratio(const Plane& L)`; `double structure_to_gradient_ratio_from_transform(const Starlet&)` (returns `+inf` / 0 as Python); `std::vector<double> background_channel_balance(const Image& rgb, double percentile = 25.0)`.
- `struct Background { double floor_to_peak_ratio, structure_to_gradient_ratio; std::vector<double> channel_balance; }`; `struct FingerprintData { EnergySpectrum energy; std::array<double,10> tonal_quantiles, saturation_quantiles; std::vector<double> chroma_hist /*16×16 row-major*/; Background background; double pixel_scale_arcsec, psf_fwhm_arcsec; std::string palette_class; Json to_json() const; static FingerprintData from_json(const Json&); }` — keys exactly `extract.py::to_dict` (chroma_hist as a list of 16 lists).
- `FingerprintData extract(const Image& rgb, double pixel_scale_arcsec, int n_scales, double psf_fwhm_arcsec, const std::string& palette_class)` (one starlet shared, as Python).
- `std::string palette_class_from_filters(const std::vector<std::string>&)`, `palette_class_from_gallery_bands(const std::vector<std::string>&)`, `bool palette_chroma_compatible(const std::string&, const std::string&)`, `extern const std::set<std::string> kPaletteClasses`.

- [ ] **Step 1: Tests:** `lab_matches_python` (≤ 1e-12), each metric vs golden (quantiles bit-exact given bit-exact Lab inputs; ≤ 1e-12 otherwise), `extract_synthetic_matches_golden` (every component ≤ 1e-9 rel), `extract_real_renders_match_golden` (the 10 Phase 0 renders loaded from `golden/real/*` inputs once Task 10 exists — until then this test reads the Python-dumped 1600-px arrays only if present and is registered in Task 10; here it covers synthetic), `json_round_trip_python_bytes` (to_json → dump_python equals the Python file byte for byte), palette table (40 rows), `chroma_family_gate` (L-only and unknown never compatible, even with themselves).
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ac-core): CIELAB, metrics, palette gate, fingerprint extraction (parity)`.

### Task 6: Exact EMD and the distance function (Opus)

**Files:** Create `src/core/fingerprint/{Emd,Distance}.{h,cpp}`, `test/unit/test_emd.cpp`, `test/unit/test_distance.cpp`.

**Interfaces (Produces):**
- `struct EmdResult { double cost; std::vector<double> u, v; /* duals */ std::vector<std::tuple<int,int,double>> flows; }`; `EmdResult emd_exact(const std::vector<double>& supply, const std::vector<double>& demand, const std::vector<double>& cost /* n×m row-major */)` — supply and demand each normalised to sum 1 inside (scipy's normalisation); network simplex, strongly feasible basis, block-search pivot, `std::runtime_error` on unbounded/infeasible (cannot happen with equal totals; checked).
- `struct DistanceWeights { double spectrum = 0.40, tonal = 0.25, chroma = 0.20, background = 0.15; }`, `struct DistanceComponents { double spectrum, tonal, chroma, background; }`; `double combine_distance(const DistanceComponents&, const DistanceWeights&, bool palette_match)`; `std::pair<double,bool> spectrum_distance(const FingerprintData& ref, const FingerprintData& target)`; `double tonal_distance(...)`, `double chroma_distance(..., double extent = 100.0)`, `double background_distance(...)`; `double fingerprint_distance(const FingerprintData& ref, const FingerprintData& target, const DistanceWeights& = {})`.

- [ ] **Step 1: Tests:** `emd_dual_certificate` (for 200 random sparse/dense problems: primal feasible ≤ 1e-12, every reduced cost ≥ −1e-12, basic arcs = 0 ± 1e-12, `cost == Σ u·supply + Σ v·demand` ≤ 1e-12), `emd_matches_scipy` (20 golden pairs, ≤ 1e-9 abs after `/diagonal`), `emd_degenerate_identical_histograms_zero`, `emd_single_bin_masses`, `chroma_distance_neutral_is_zero`, `combine_palette_mismatch_ignores_chroma` (chroma 0 vs 1 → same D), `combine_equal_components_returns_value`, `distance_unusable_band_redistributes`, `fingerprint_distance_matches_golden` (≤ 1e-9), `emd_256x256_under_20ms` (`[perf]`).
- [ ] **Step 2–4:** FAIL → implement → PASS. If a scipy golden disagrees beyond 1e-9 while the certificate holds, stop and report both numbers (do not loosen). **Step 5: Commit** `feat(ac-core): exact network-simplex EMD with dual certificate; fingerprint distance (parity)`.

## Phase C: Optimizer core

### Task 7: Actions, recipe, proposer (Sonnet)

**Files:** Create `src/core/optimize/{Actions,Recipe,Propose}.{h,cpp}`, `test/unit/test_actions.cpp`, `test/unit/test_propose.cpp`.

**Interfaces (Produces):**
- `extern const std::array<const char*,3> kLevels`; `extern const std::set<std::string> kOnceOnly`; `struct Action { std::string kind, level; std::optional<double> scale_arcsec; Json params; std::string key() const; double strength() const; bool operator==(const Action&) const /* kind, level, scale only */; }`; `double scale_for_layer(int, double)`; `int layer_for_scale(double scale, double pixel_scale)` (throws as Python, `round` = Python's round-half-even on `log2`).
- `std::vector<Action> available_actions(double pixel_scale_arcsec, double psf_fwhm_arcsec, int n_scales, bool palette_compatible, const std::set<std::string>& applied_kinds)` (menu order exactly Python's).
- `extern const std::map<std::string,std::string> kProcessForKind`; `Json action_to_json(const Action&)`, `Action action_from_json(const Json&)`, `Json pixinsight_step(const Action&)`; `struct Recipe { std::vector<Action> actions; Recipe extend(const Action&) const; std::string key() const; std::set<std::string> applied_kinds() const; Json to_json() const; static Recipe from_json(const Json&); Json to_pixinsight_steps() const; }`.
- `std::string level_for_gap(double)`; `std::map<std::string,double> component_gaps(ref, target)`; `std::vector<double> band_deficits(ref, target)`; `std::vector<Action> propose_actions(const FingerprintData& ref, const FingerprintData& target, const std::set<std::string>& applied_kinds, int n_scales, std::optional<int> top_k)`.

- [ ] **Step 1: Tests:** `action_key_format` (`local_contrast@2.000/gentle`, `star_split@-/-`), `strength_rejects_unknown_level`, `layer_for_scale_below_pixel_scale_throws`, `menu_matches_python` (12 golden cases, order and params), `pixinsight_step_matches_python`, `recipe_json_round_trip`, `propose_matches_python` (12 cases, top_k 3 and None, identical action lists), `propose_withholds_chroma_on_mismatch`, `group_sort_numeric_not_string` (16.000 after 2.000).
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ac-core): action menu, recipe rendering, gap-ranked proposer (parity)`.

### Task 8: Guardrails (Sonnet)

**Files:** Create `src/core/optimize/Guardrails.{h,cpp}`, `test/unit/test_guardrails.cpp`.

**Interfaces (Produces):** `struct GuardrailLimits { double noise_growth = 0.15, shadow_clip_fraction = 1e-4, highlight_clip_fraction = 1e-4, star_count_drop = 0.05, star_fwhm_growth = 0.20, star_ecc_growth = 0.20, hue_invention_mass = 1e-3, hue_tolerance_deg = 20.0, channel_ratio_drift = 0.10; }`; `struct GuardrailVerdict { std::string name; bool ok; std::string reason; double value, limit; }`; `double mrs_noise_sigma(const Plane& gray)`; `struct StarStats { int count; double median_fwhm, median_ecc; }`; `StarStats detect_stars(const Plane& gray, double k_sigma = 5.0)`; one `check_*` per guardrail; `std::vector<GuardrailVerdict> evaluate_guardrails(const Image& candidate, const Image& baseline, const Image& source, const GuardrailLimits& = {})` (all six, in Python's order, no short-circuit). Reason strings reproduce Python's f-string formats (`:.1%`, `:.4%`, `:.2f`, `:.4f`, `:.3f`, `:.0f`).

- [ ] **Step 1: Tests:** one golden case per guardrail tripping alone, plus the pass case, comparing `ok`, `value` (≤ 1e-12) and `reason` (exact string): `noise_floor_trips`, `noise_floor_nonpositive_baseline_fails_closed`, `shadow_clip_trips`, `highlight_clip_trips`, `star_count_drop_trips`, `star_fwhm_trips`, `star_ecc_trips`, `starless_baseline_noted_not_failed`, `hue_invention_angle_not_cell` (1.8× saturation boost passes; a 116° shift trips), `hue_wraps_at_360`, `neutral_source_any_chroma_trips`, `channel_ratio_drift_trips`, `guardrails_thread_invariant`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ac-core): the six §7 guardrails (parity, angle-based hue invention)`.

### Task 9: Beam, loop, session, approx executor, loop parity (Opus)

**Files:** Create `src/core/optimize/{Executor.h,Beam,Loop,Session,ApproxExecutor}.{h,cpp}`, `test/unit/test_loop.cpp`, `test/unit/test_session.cpp`.

**Interfaces (Produces):**
- `class Executor { public: virtual ~Executor(); virtual Image apply(const Image& parent, const Action&, double pixel_scale_arcsec) = 0; };` (throws on failure; the loop logs `executor` and continues).
- `class ImageStore { public: std::string put(std::shared_ptr<const Image>, std::string id_hint); std::shared_ptr<const Image> get(const std::string&) const; void release_unreferenced(const std::set<std::string>& live); };`
- `struct BeamConfig { int width = 3, top_k = 3, iteration_cap = 20, convergence_window = 3; double epsilon = 1e-3, epsilon_improve = 1e-3; int max_attempts = 0 /* 0 → 3·top_k */; int resolved_max_attempts() const; }`; `struct Branch { Recipe recipe; std::string image_id; double distance; bool alive = true; std::optional<FingerprintData> fingerprint; }`; `std::vector<Branch> prune(std::vector<Branch>, int width)` (stable sort by distance, Python's order for ties).
- `struct Session { std::string session_id, work_dir, source_path, proxy_id, reference_id; FingerprintData reference_fp; double pixel_scale_arcsec, psf_fwhm_arcsec; std::string palette_class; int n_scales; BeamConfig config; int iteration = 0; std::vector<Branch> branches; Branch best; double baseline_distance; std::vector<double> distance_history; bool converged = false; std::string convergence_reason; Json guardrail_log = Json::array(); Json to_json() const; }`.
- `Session begin(const std::string& source_path, const std::string& proxy_id, const std::string& reference_id, const FingerprintData& reference_fp, const std::string& work_dir, double pixel_scale_arcsec, double psf_fwhm_arcsec, const std::string& palette_class, int n_scales, const std::string& session_id, const ImageStore&, BeamConfig = {})`; `void advance(Session&, Executor&, ImageStore&, const GuardrailLimits& = {}, const std::function<void(const Session&)>& on_iteration = {})` (candidate ids `cand-<iteration>-<index>` as Python's paths); `void run_to_convergence(...)`; `Json outcome(const Session&)` (Python keys; `result_path`/`proxy_path` carry image ids).
- `class ApproxExecutor final : public Executor` — test-only port of `NumpyExecutor` (lives in `ac_core` but is linked only by tests and `ac-tool`), including the `_max_supportable_layer` refusal.

- [ ] **Step 1: Tests:** `prune_distinct_by_recipe`, `prune_ignores_dead`, `convergence_iteration_cap`, `convergence_window_epsilon`, `no_op_candidate_logged_not_kept`, `executor_exception_logged_run_continues`, `attempt_cap_noted`, `approx_executor_matches_numpy` (each action on the synthetic image, ≤ 1e-12), `approx_executor_rejects_unsupportable_layer`, `loop_declines_at_floor_matches_python`, `loop_improves_flattened_matches_python`, `loop_guardrail_hostile_exhausts_matches_python` — the three loop goldens compare every iteration's branch recipe keys, distances (≤ 1e-9), `distance_history`, the full `guardrail_log` (iterations, action keys, failed/noted names, reasons) and `outcome` (`improved`, recipe, steps, convergence_reason); `session_json_schema_matches_python` (keys and nesting of `Session.save` minus the dropped batched fields `pending/cursors/candidates/max_dim/candidate_suffix`, which are written as Python's empty defaults for schema compatibility).
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ac-core): beam search loop, fail-safe outcome, session log, approx executor (loop parity with Python)`.

## Phase D: Images, WCS, store, discovery (`ac_core`)

### Task 10: Raster decoding and Pillow-exact resampling (Sonnet)

**Files:** Create `src/core/io/{Raster,Resample}.{h,cpp}`, `test/unit/test_raster.cpp`, `test/unit/test_resample.cpp`. Modify `cmake/Deps.cmake` (libjpeg-turbo, libpng, libtiff: FetchContent, URL + SHA-256, static, `-fPIC`, `WITH_SIMD=OFF`, hidden visibility; zlib from PCL's `zlib-pxi` in module builds and the system zlib only for host test builds), `THIRD_PARTY_NOTICES` (libjpeg-turbo IJG+BSD, libpng, libtiff, Pillow HPND for the ported resampler).

**Interfaces (Produces):**
- `std::set<std::string> supported_suffixes()` (= Python's 11); `std::pair<int,int> image_dimensions(const std::string& path)` (header only, no pixel decode; refuses unsupported suffixes with Python's message).
- `struct Decoded { int width, height, channels, bits; std::vector<uint8_t> u8; std::vector<uint16_t> u16; std::vector<float> f32; }`; `Decoded decode_jpeg(path)`, `decode_png(path)` (8- or 16-bit, palette/gray/alpha as Pillow/pypng present them), `decode_tiff(path)` (native depth; >3-D refused).
- `std::vector<uint8_t> resize_u8(const uint8_t* src, int w, int h, int c, int nw, int nh)` (Pillow 8bpc bicubic, fixed point, streaming horizontal pass); `std::vector<float> resize_f32(const float* src, int w, int h, int nw, int nh)` (Pillow "F" mode).
- `Image load_raster(const std::string& path, std::optional<int> max_dim)` (Python dispatch: TIFF → native path; PNG IHDR bit depth 16 → native path; else 8-bit Pillow path; promote gray, drop alpha, full-scale normalisation, clip); `double downsample_factor(const std::string& path, const Image& loaded)`.

- [ ] **Step 1: Tests:** `jpeg_decode_bitexact_vs_pillow` (16 rows of every cached/reference JPEG), `png8_decode_bitexact`, `png16_round_trip_low_byte` and `tiff16_round_trip` (fixtures from `test_raster_highdepth.py` rebuilt by `make_goldens.py`), `load_raster_matches_python` (every real render: shape equal, SHA-256 of float64 bytes equal; on mismatch the test prints max |Δ|), `resize_f32_matches_pillow` (≤ 1 float32 ulp), `unsupported_suffix_message`, `uint16_low_max_not_257x` (constant 200 → 0.003052), `heic0601a_peak_memory_under_300mb` (getrusage delta; `[perf]`), then register Task 5's `extract_real_renders_match_golden` over `load_raster` output.
- [ ] **Step 2–4:** FAIL → implement → PASS. If SIMD-off decoding differs from Pillow, switch `WITH_SIMD=ON` with `nasm` added to the Containerfile (new image tag) and record why. **Step 5: Commit** `feat(ac-core): JPEG/PNG/TIFF decode + Pillow-exact resampling (bit parity on real renders)`.

### Task 11: WCS geometry, XMP/AVM (strict), tier logic (Opus)

**Files:** Create `src/core/io/Xmp.{h,cpp}`, `src/core/db/{Wcs,Avm,Skymath}.{h,cpp}`, `test/unit/test_wcs.cpp`, `test/unit/test_avm.cpp`.

**Interfaces (Produces):**
- `double separation_arcmin(double ra1, double dec1, double ra2, double dec2)` (haversine as Python); `bool cones_overlap(double sep, double ra, double rb)`.
- `struct TanWcs { double crpix[2], crval[2], cd[2][2]; }` (FITS 1-based crpix); `struct Geometry { double ra_deg, dec_deg, radius_arcmin, pixel_scale_arcsec; }`; `Geometry tan_geometry(const TanWcs&, int nx, int ny)` (astropy `pixel_to_world` at 0-based `(nx/2, ny/2)` and `(0,0)`; scale = mean column norm of CD in arcsec).
- `std::optional<std::string> extract_xmp(const std::string& path)` (JPEG APP1 XMP, PNG iTXt/`XML:com.adobe.xmp`, TIFF tag 700).
- `struct AvmResult { bool present = false; bool ok = false; std::string error; TanWcs wcs; }`; `AvmResult parse_avm(const std::string& xmp, int image_w, int image_h)` — reproduces pyavm's parsing **and** its strict validation (the same accept/reject on every golden file; the error text names the offending field), `Spatial.ReferenceDimension` absent → image size; CDELT/CROTA and CD forms.
- `struct ManualWcs { double ra_deg, dec_deg, fov_radius_arcmin; std::optional<double> pixel_scale_arcsec; }`; `struct WcsResult { std::optional<double> ra_deg, dec_deg, fov_radius_arcmin, pixel_scale_arcsec; std::string wcs_source; bool solved; std::string detail; }`; `using BlindSolverFn = std::function<std::optional<WcsResult>(const std::string& path)>`; `WcsResult acquire_wcs_raster(const std::string& path, const BlindSolverFn&, const std::optional<ManualWcs>&)` (Python tier order and detail strings, "avm" tier for non-FITS; FITS files are handled in the module, Task 16).

- [ ] **Step 1: Tests:** `separation_table_matches_python` (≤ 1e-9 arcmin), `tan_geometry_matches_astropy` (4 synthetic WCS, ≤ 1e-9 deg / rel), `avm_accept_reject_matches_pyavm` (all 40 real files: `present`, `ok` equal; `eso1103a` rejected), `avm_wcs_matches_pyavm` (accepted files: crpix/crval/cd ≤ 1e-12, geometry ≤ 1e-9), `manual_tier_used_only_when_avm_fails`, `unsolved_detail_lists_tiers`, `blind_tier_called_before_manual`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ac-core): TAN geometry, XMP/AVM with pyavm-strict validation, WCS tiers (parity)`.

### Task 12: Records, fingerprint store, gallery index, ingest, seed (Sonnet)

**Files:** Create `src/core/db/{Records,Store,Index,Ingest,Seed}.{h,cpp}`, `test/unit/test_store.cpp`, `test/unit/test_ingest.cpp`. Modify `cmake/Deps.cmake` (sqlite from `third_party/sqlite`, `SQLITE_OMIT_LOAD_EXTENSION=1`, `SQLITE_THREADSAFE=1`).

**Interfaces (Produces):**
- `extern const std::set<std::string> kSourceTypes, kConsensusEligible`; `struct ReferenceRecord { std::string id; double ra_deg, dec_deg, fov_radius_arcmin; std::string palette_class, source_type; FingerprintData fingerprint; Json provenance; Json to_json() const; static ReferenceRecord from_json(const Json&); }`.
- `struct ConeMatch { ReferenceRecord record; double separation_arcmin; bool palette_compatible; }`; `class FingerprintStore { explicit FingerprintStore(const std::string& db_path); void store(const ReferenceRecord&); std::optional<ReferenceRecord> get(const std::string&) const; std::vector<ConeMatch> cone_search(double ra, double dec, double radius_arcmin, std::optional<std::string> palette = {}, std::optional<std::set<std::string>> source_types = {}) const; std::vector<ReferenceRecord> all() const; }` (schema verbatim from `store.py`, `tool_output` and unknown types refused with Python's messages, record JSON via `dump_python`).
- `struct GalleryEntry` (all 20 fields of `gallery.py::GalleryEntry`, optionals for nullable) with `bool has_position() const`; `struct IndexMatch { GalleryEntry entry; double separation_arcmin; }`; `class GalleryIndex { explicit GalleryIndex(const std::string&); void upsert(const GalleryEntry&); std::optional<GalleryEntry> get(gallery, id) const; bool has(gallery, id) const; int count() const; std::vector<IndexMatch> cone_search(double ra, double dec, double radius) const; std::optional<std::string> get_sync_state(const std::string&) const; void set_sync_state(const std::string&, const std::string&); }` (schema verbatim from `index.py`).
- `ReferenceRecord ingest_reference(FingerprintStore&, const std::string& image_path, ...)` and `struct IngestOutcome { bool ingested; WcsResult wcs; std::optional<ReferenceRecord> record; std::string detail; }`, `IngestOutcome ingest_reference_auto(...)` (parameters exactly Python's, `max_dim` default 1600, `n_scales` 7, `id` default = random UUID4 string); `struct SeedOutcome { std::string id; bool ingested; std::string detail; std::optional<std::string> wcs_source; }`, `std::vector<Json> load_catalog(path)`, `std::vector<SeedOutcome> seed_catalog(FingerprintStore&, const std::vector<Json>&, const std::string& image_dir, int n_scales = 7, std::optional<int> max_dim = 1600)`.

- [ ] **Step 1: Tests:** `store_rejects_tool_output`, `store_rejects_unknown_source_type`, `cone_search_ra_wrap_and_order`, `cone_search_flags_palette_not_filters`, `store_opens_python_written_db` (copy of `tools/autocontrast/data/fingerprints.sqlite` into the build dir; both records load, fingerprints equal the Python JSON; skip-77 only when the file is absent), `python_reads_cpp_written_db` (ctest runs `.venv/bin/python -I` on a C++-written store; skip-77 without the venv), `store_corrupt_record_errors`, `store_readonly_errors`, `index_opens_python_written_db` (61 entries, `cone_search(83.82, -5.39, 30)` matches the Python golden list), `ingest_downsample_corrects_scale`, `seed_catalog_reports_missing_image`, `seed_avm_wins_over_declared_position` (`heic0601a` → `wcs_source == "avm"`).
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ac-core): SQLite fingerprint store and gallery index (Python-compatible), ingest, seed`.

### Task 13: Gallery parsing, polite fetching, crawl, discovery (Sonnet)

**Files:** Create `src/core/discover/{Gallery,Fetch,Crawl,Discover}.{h,cpp}`, `test/fixtures/gallery/*.html` (copied from `tools/autocontrast/tests/fixtures/gallery/`), `test/unit/test_gallery.cpp`, `test/unit/test_crawl.cpp`, `test/unit/test_discover.cpp`.

**Interfaces (Produces):**
- `struct GalleryConfig { std::string key, base_url, search_path, detail_path; bool results_paginated; std::string default_license; }`; `extern const std::map<std::string, GalleryConfig> kGalleries`; `search_url`, `detail_url`, `text_lines`, `labelled_value`, `parse_ra_sexagesimal`, `parse_dec_sexagesimal`, `parse_fov_arcmin`, `fov_radius_arcmin`, `parse_release_date`, `parse_credit`, `asserts_copyright`, `parse_filter_bands`, `parse_size_px`, `GalleryEntry parse_detail(const std::string& doc, const std::string& entry_id, const std::string& gallery, const std::string& detail_url, std::optional<int> w, std::optional<int> h)`, `struct ListingItem`, `parse_listing`, `parse_result_total` — Python semantics, `std::regex` ECMAScript, HTML entity decoding as `html.unescape` for the entities the fixtures contain plus numeric references.
- `struct HttpResponse { int status = 0; std::string body, content_type, location, error; std::optional<double> retry_after; }`; `class HttpTransport { public: virtual HttpResponse get(const std::string& url, size_t max_bytes) = 0; }`; `class Clock { public: virtual double now() = 0; virtual void sleep(double s) = 0; virtual double jitter() = 0; /* U(0,0.25) */ }`; `class Fetcher { public: virtual std::string get_text(const std::string&) = 0; virtual std::string get_bytes(const std::string&, size_t max_bytes) = 0; }`; `class PoliteFetcher final : public Fetcher { PoliteFetcher(HttpTransport&, Clock&, std::string user_agent, double min_interval = 1.0, int max_requests = 8000, int max_retries = 3); }` (throttle, 429/5xx/transport retry rules, `RequestBudgetExceeded`, size cap, redirect following within `kAllowedHosts` ≤ 3 hops); `extern const std::vector<std::string> kAllowedHosts`.
- `struct CrawlReport`, `class DriftError`, `CrawlReport crawl_gallery(GalleryIndex&, const GalleryConfig&, Fetcher&, const std::map<std::string,std::string>& criteria, std::optional<int> max_pages, bool refresh)`, `CrawlReport sync_gallery(GalleryIndex&, const GalleryConfig&, Fetcher&, const std::string& now_utc, int safety_days = 7, std::optional<int> max_pages = {})`.
- `struct RankedCandidate`, `rank_candidates`, `position_tolerance_arcmin`, `struct VerifyResult`, `verify_metadata`, `verify_candidate` (with `BlindSolverFn` and an injectable `WcsAcquirer`), `struct CandidateReport`, `struct DiscoveryOutcome`, `struct AcquisitionOutcome`, `discover_reference(...)`, `acquire_reference(...)` — Python signatures, cache file `<cache_dir>/<entry_id><suffix>`.

- [ ] **Step 1: Tests** (port the intent of `test_gallery_*.py`, `test_crawl.py`, `test_discover_*.py` case by case — every Python test function gets a C++ counterpart named the same, listed in the task report): parse results equal the golden JSON for the six fixtures; `dec_negative_zero`, `fov_unknown_unit_is_none`, `credit_copyright_withholds_license`, `trailing_malformed_tr_skipped`; scripted transport: `throttle_waits_min_interval_plus_jitter`, `retry_after_honoured_capped_120`, `5xx_backoff_2_pow_attempt`, `budget_exceeded_raises`, `bytes_cap_refuses`, `redirect_outside_allowlist_refused`, `drift_error_above_half_unparseable`, `crawl_resumable_skips_indexed`, `progress_guard_stops_on_no_fresh_ids`, `sync_window_minus_safety_days`; discovery: `rank_order_palette_framing_centering_id`, `gates_g5_g1_before_download` (no transport call), `g2_avm_drift_rejects`, `g3_solved_outside_cone_rejects`, `g4_no_scale_rejects`, `acquire_hits_store_without_network`, `discover_e2e_with_fixture_transport` (Python `test_discover_e2e.py` equivalent).
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(ac-core): gallery parsing, polite fetcher, crawl/sync, discovery gates (parity)`.

## Phase E: PixInsight integration

### Task 14: NetworkTransfer transport, consent, live smoke (Sonnet)

**Files:** Create `src/module/{PclTransport,NetworkConsent}.{h,cpp}`, probe cases in `src/module/probe/`, `test/run-probe-live.sh`.

**Interfaces (Produces):** `class PclTransport final : public ac::HttpTransport` (NetworkTransfer, `SetSSL(true,true,true,true)`, connection timeout 60 s, `User-Agent` header, `Control` receiver as GDG's `TapClient`, allow-list re-checked here, response code / content type / `Location` / `Retry-After` captured); `class SystemClock final : public ac::Clock`; `namespace pcl::NetworkConsent { constexpr int kVersion = 1; bool Granted(); String GrantedAt(); void Grant(); void Revoke(); String DisclosureHtml(); bool Ask(); /* GUI only */ void Require(bool allowNetwork); /* throws pcl::Error with the spec 7.2 text when allowNetwork is false or, interactively, consent is older than kVersion */ }`.

- [ ] **Step 1: Failing probes** (`run-probe-live.sh`, runs by default, **fails** — never skips — when a site is down, printing the HTTP status): `live_listing_esa_hubble` (search page 1 parses ≥ 1 item), `live_detail_eso1103a` (parses position within 0.01′ of the golden), `live_cdn_download_small` (`opo9545a`-sized ≤ 3 MB render → JPEG SOI, size equals Content-Length), `redirect_followed_within_allowlist` (records what NetworkTransfer actually does on a 301/302 from the galleries; updates spec section 7 [I] with the measured behaviour), `host_outside_allowlist_refused` (no network call), `network_refused_without_allow` (error text).
- [ ] **Step 2–4:** FAIL → implement → PASS (container probe build). **Step 5: Commit** `feat(autocontrast): NetworkTransfer transport (allow-listed, HTTPS), versioned network consent, live smoke`.

### Task 15: `PiExecutor` and `ImageBridge` (Opus)

**Files:** Create `src/module/{ImageBridge,PiExecutor}.{h,cpp}`, `test/unit/test_executor_params.cpp` (PCL-free parameter tables), probe cases.

**Interfaces (Produces):**
- `ac::Image ImageFromView(const View&)` (float64 [0,1], mono → 3 channels, throws on non-finite with the count); `ImageWindow NewHiddenWindow(const ac::Image&, const IsoString& id)` (32-bit float, not shown); `ac::Image ImageFromWindow(const ImageWindow&)`.
- `struct ProcessCall { IsoString processId; std::vector<std::pair<IsoString, Variant>> scalars; std::vector<TableValue> tables; }` built by PCL-free `ac::BuildProcessCall(const Action&, double pixel_scale_arcsec, const ImageFacts&)` in `test_executor_params` scope (pure data: process ids and every parameter value from `autocontrast_optimize.js` l.334–481 — MLT `layers` rows `[true,true,bias,false,3.0,1.0,1]` for `0..layer+1` with `bias = strength` at `layer`, `transform = StarletTransform`; LHE `radius = round(radius_arcsec / scale)`, `amount = strength`, `slopeLimit 2.0`, `histogramBins = Bit8`, `circularKernel true`; HDRMT `numberOfLayers = layers`, `numberOfIterations 1`, `medianTransform true`, `toLightness true`; Curves `K = [[0,0],[0.25,0.25−0.15s],[0.75,0.75+0.15s],[1,1]]`, `Kt = AkimaSubsplines`; HT `H` rows with RGB/K shadows = `clipLow1% · strength`; ColorSaturation `HS = [[0,s],[1,s]]`, `HSt = AkimaSubsplines`, `hueShift 0`; BN `backgroundReferenceViewId ""`, `backgroundLow 0`, `backgroundHigh 0.1`, `mode = RescaleAsNeeded`; SXT `stars false`, `unscreen false`, `overlap 0.20`).
- `class PiExecutor final : public ac::Executor` — opens a hidden window per candidate, runs `ProcessInstance::ExecuteOn` (false → throws "PixInsight reported that <process> did not execute"), reads pixels back, closes the window in every path; enum values and parameter ids confirmed by a probe introspection case (`executor_ids`), like GDG Correction C17.

- [ ] **Step 1: Tests:** `test_executor_params` — every action kind × level renders exactly the values above (cite JS line numbers in the test). Probes: `executor_ids` (process and parameter ids exist, enum numeric values recorded), `executor_each_action_changes_pixels` (400 px fixture, each kind except SXT changes pixels and keeps [0,1]), `probe_lhe_radius_below_16_logged` (PI raises → `Executor::apply` throws, message contains the radius), `executor_sxt_absent_or_failing_reported` (absent → "StarXTerminator is not installed"; present-but-false → the executeOn message), `black_point_histogram_matches_pjsr_definition` (pcl::Histogram channel/resolution defaults confirmed against `Histogram(view.image).normalizedClipLow` semantics from the PJSR docs; record the finding), `no_window_leak` (window count before == after, including exception paths).
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(autocontrast): PixInsight executor (eight stock-process builders) and view bridge`.

### Task 16: `AutoContrast` process — Analyze and Optimize (Opus)

**Files:** Create `src/module/{AutoContrastParameters,AutoContrastInstance,TargetWcs,RunReport}.{h,cpp}`; modify `AutoContrastProcess.cpp` (command line), probe cases, `test/run-probe.sh` cases, `test/fixtures/` (small solved FITS/XISF generated by `make_goldens.py`, ≤ 1 MB).

**Interfaces (Produces):** the parameters of spec §8 with those exact ids, types and defaults; `AutoContrastInstance::ExecuteOn(View&)` running spec 5.1 steps 1–9; command line `AutoContrast [--mode=analyze|optimize] [--view=<id>|<file>] [--store=] [--index=] [--cache=] [--work=] [--allow-network] [--reference=<id>] [--manual=ra,dec,radius,scale] [--palette=] [--max-dim=] [--no-replay]`; `pcl::TargetWcs { static ac::WcsResult Acquire(const ImageWindow&, const AutoContrastInstance&, const ac::BlindSolverFn&); }` (tier 1 `AstrometricMetadata`, tier 2 the source file's XMP/AVM, tier 3 injected, tier 4 manual); `RunReport` writes `report.txt`, `recipe.json` (PJSR keys), `session.json` (each iteration), console lines with the PJSR's wording for decline/improve.

- [ ] **Step 1: Failing probes:** `analyze_solved_fixture` (reference found in a fixture store, D printed and equal to `ac::fingerprint_distance` on the same proxy ≤ 1e-12), `analyze_no_wcs_errors_with_all_tiers`, `analyze_manual_requires_all_four`, `header_wcs_matches_astropy` (the NAS M42 stack: centre ≤ 1e-6 deg, radius/scale ≤ 1e-6 rel — or the measured SIP-vs-spline difference recorded and asserted; skip-77 without the NAS), `palette_from_filter_keyword` (HaO3 → HOO), `probe_linear_input_refused` (D3; message tells the user to stretch first), `optimize_declines_at_floor` (fixture vs its own fingerprint → `outImproved false`, no new window, report says untouched), `optimize_improves_flattened` (`flatten(eso1103a-crop, 0.6)` → `outImproved true`, new window `<id>_autocontrast` at full size, recipe steps = stock processes), `fullres_check_logged` (D4; divergence above `fullResTolerance` → a loud `fullres_divergence` noted entry, run flagged, nothing discarded), `probe_mono_view_runs`, `probe_nonfinite_refused`, `probe_abort_leaves_no_windows`, `store_miss_without_network_says_so`, `execute_global_refused`, `command_line_matches_parameters`.
- [ ] **Step 2–4:** FAIL → implement → PASS (container probe build). D3 thresholds measured and recorded in the task report before they are committed. **Step 5: Commit** `feat(autocontrast): AutoContrast process — analyze, guardrailed optimize, full-res validation and replay`.

### Task 17: `AutoContrastReferences` process (Sonnet)

**Files:** Create `src/module/AutoContrastReferences{Parameters,Instance}.{h,cpp}`; modify its process (command line); probe cases.

**Interfaces (Produces):** spec §8 operation enum and parameters; command line `AutoContrastReferences --sync[=esa_hubble,eso] [--max-pages=N] [--refresh] | --seed=<catalog.json> --images=<dir> | --ingest=<file> --palette=<class> --source-type=<type> [--license= --attribution= --source-url=] [--manual=…] | --list`, plus the shared `--store/--index/--cache/--allow-network`.

- [ ] **Step 1: Failing probes:** `seed_from_repo_catalog` (`tools/autocontrast/data/seed_catalog.json` + `data/references` → 5 outcomes equal to Python's `seed_catalog` golden; skip-77 without `data/references`), `ingest_rejects_tool_output`, `ingest_fits_file_min_max_normalised` (fixture FITS → fingerprint equals `ac::extract` on the Python-normalised array), `list_prints_store_and_index`, `sync_requires_allow_network`, `sync_live_one_page` (`--sync=eso --max-pages=1`, runs by default, fails when the site is down), `drift_is_loud_nonzero`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(autocontrast): AutoContrastReferences process — sync, seed, ingest, list`.

### Task 18: Interfaces and consent dialog (Sonnet)

**Files:** Create `src/module/AutoContrastInterface.{h,cpp}`, `AutoContrastReferencesInterface.{h,cpp}`, `test/gui-smoke.sh` (private Xvfb, XTest-driven, slot 97, PNGs into `<out-dir>`, modelled on `modules/planetary/test/gui-smoke.sh` but without PJSR: open through the `.scp` route).

**Interfaces (Produces):** every spec §8 parameter editable, grouped as in the table; "Allow network" checkbox opens `NetworkConsent::Ask()` when consent is missing/outdated and unticks on refusal; a "Withdraw network consent" button; reference attribution (licence, credit, URL) shown whenever a reference is displayed (design §5.5); progress through `StandardStatus` with Cancel.

- [ ] **Step 1: Failing smoke steps:** `interfaces_open`, `consent_dialog_names_four_hosts`, `consent_refused_unticks`, `analyze_from_interface_shows_attribution`, `references_list_from_interface` — screenshots saved, each step asserts on the console marker the module writes.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(autocontrast): process interfaces and network consent dialog`.

### Task 19: Shared plate solver and WCS tier 3 (Opus)

**Precondition (spec A11):** GDG's solver commits (`cddc1ba0` and its parents) are on `origin/main`. If they are not when this task comes up, it waits; every other task is independent of it (tier 3 is reached through `ac::BlindSolverFn`).

**Files:** Merge `origin/main` into `ac/pcl-port` — a merge commit on **this** branch only. Then create `modules/shared/platesolve/` (`CMakeLists.txt` defining static `astropi_platesolve`; `git mv` of `Solver*.{h,cpp}` and `core/SolveMath.{h,cpp}` plus the headers they need from `modules/gaia-depth-grade/src/`), re-point `modules/gaia-depth-grade` CMake at it (its tests must stay green), and add `src/module/SolverAdapter.{h,cpp}` to AutoContrast.

**Interfaces:** Consumes `pcl::SolveImage(ImageWindow&, const SolverParams&, const gdg::Log&) → SolveReport` and `BlindMode` (from `Solver.h` on `gdg/phaseE-solver`). Produces `ac::BlindSolverFn MakeBlindSolver(BlindMode, StringList gaiaDatabaseFiles)` — opens the raster/view in a hidden window, solves, reads `AstrometricMetadata` back into `ac::WcsResult{wcs_source = "blind"}`, returns `std::nullopt` with the solver's error in the run report when unsolvable or when no Gaia DR3 / index is configured.

- [ ] **Step 1: Failing tests:** GDG's full ctest + its solver probes unchanged after the move (`solve_spline_residuals`, `solve_fails_loudly_without_seed_or_index`); AutoContrast probes `tier3_solves_unsolved_copy` (strip the solution from the M42 fixture → tier 3 recovers centre within 1′ and scale within 1 %; skip-77 without Gaia DR3), `tier3_unavailable_falls_through_loudly`, `discovery_g2_uses_blind_for_avm_less_render` (`opo0205c`; records solved/unsolved — solve_spike's open question — in the task report).
- [ ] **Step 2–4:** FAIL → implement → PASS (container build of **both** modules). **Step 5: Commit** `refactor(platesolve): factor Gaia Depth Grade's native solver into modules/shared/platesolve; AutoContrast WCS tier 3`. Report the merge dependency (spec §9) in the commit body.

## Phase F: Acceptance and cut-over

### Task 20: `ac-tool` and Phase 0 real-data parity (Sonnet)

**Files:** Create `src/core/eval/{Ranking,Degrade}.{h,cpp}`, `src/tool/ac-tool.cpp`, `test/unit/test_eval.cpp`.

**Interfaces (Produces):** `ac-tool fingerprint <image> --scale= --psf= --palette= [--max-dim=] [--n-scales=]`, `ac-tool distance <ref.json> <target.json>`, `ac-tool rank --pros <dir> --amateurs <dir> [--reference <img>] [--palette --pixel-scale --psf]` (exit codes as `eval.ranking`), `ac-tool degrade --references <dir>` (as `eval.degrade`), no `sync` (network code lives only in the module; `AutoContrastReferences --sync` replaces `python -m autocontrast.db.discover sync`). Eval functions mirror `ranking.py`/`degrade.py` signatures.

- [ ] **Step 1: Tests:** `flatten_matches_python` etc. (≤ 1e-12), `leave_one_out_report_matches_python` and `local_minimum_report_matches_python` on the 10 real renders (every distance ≤ 1e-9; pass/fail verdicts identical; skip-77 without `data/`), `ac_tool_rank_exit_code`.
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `feat(autocontrast): Phase 0 evaluation harness and ac-tool dev CLI (parity on real renders)`.

### Task 21: §10 exit criterion and end-to-end real-data parity (Opus)

**Files:** Create `test/run-exit-criterion.sh` and the probe cases it drives. Read `tools/autocontrast/docs/history/superpowers-standalone/sdd/2026-07-27-phase2-optimizer/task-14-brief-v2.md` first (not v1).

- [ ] **Step 1: Failing acceptance cases** (real PixInsight, real data, run by default; skip-77 only when the NAS is absent): `exit_declines_at_floor` (`eso1103a` proxy vs its own fingerprint → declined, file untouched), `exit_improves_flat` (`flatten(eso1103a, 0.6)` → improved, D falls by > `epsilon_improve`), `exit_print_measured` (the finished HOO M42 print from `/mnt/qnap/astro_data/prints/` → result **recorded** as the Phase 3 calibration question, not asserted either way — memory `autocontrast-phase2-live-pixinsight-findings`), `exit_linear_stack_refused` (the M42 stack, linear → D3 refusal), `analyze_m42_stack_matches_python` (WCS, palette HOO, chosen reference, D vs the Python `analyze` golden ≤ 1e-9 when the same store is used), `fullres_divergence_measured` (records proxy vs full-res D on the improved run).
- [ ] **Step 2–4:** FAIL → fix root causes → PASS. Calibration observations (iteration_cap 20 binding, stacked `local_contrast` count, tonal/background dominance of top_k) are written into the task report — never tuned here. **Step 5: Commit** `test(autocontrast): §10 exit criterion and end-to-end real-data parity`.

### Task 22: Documentation, package description, gated release wiring (Sonnet)

**Files:** Create `doc/AutoContrast/AutoContrast.html`, `doc/AutoContrastReferences/AutoContrastReferences.html` (what it does; §2 theses in user terms; inputs, parameters, outputs, files written; **Network connections** — spec §7 table; licensing/attribution of references; PCL licence acknowledgement), `package-description.xml`, `THIRD_PARTY_NOTICES` (complete). Package the prebuilt gallery index (spec A10) as `share/AutoContrast/gallery_index.sqlite`, built by `AutoContrastReferences --sync` (full crawl, run in the background, resumable), and add the first-run copy to the user's data directory with a test (`first_run_copies_bundled_index`). Modify `release.sh` at every hand-listed module point, gated by `AC_MODULE_SHIPS=0` (the GDG `GDG_MODULE_SHIPS` pattern), with the package in a `1.9.6:2.0.0` platform block when enabled; `tools/test-release-manifest.sh` stays green.

- [ ] **Step 1: Failing checks:** `release.sh --package-test` (or the repo's dry-run mode) with `AC_MODULE_SHIPS=1` builds the AutoContrast package into a scratch repository and its manifest validates; with the gate at 0 nothing about AutoContrast appears in `repository/`; doc pages load in the probe (`doc_pages_resolve`).
- [ ] **Step 2–4:** FAIL → implement → PASS. **Step 5: Commit** `docs(autocontrast): module documentation incl. CPD 6.1 network section; gated release wiring`.

### Task 23: Retire the sidecar and the PJSR (Sonnet)

**Files:** Delete `tools/autocontrast/src/autocontrast/sidecar.py`, `tests/test_sidecar.py`, `tests/test_optimize_sidecar.py`, the batched-protocol functions in `optimize/loop.py` (`begin_batch`, `step_batch`, `_plan_batch`, `_ingest_batch`, `_open_iteration`, `_cursor_for`, `_instruction`, `_produced_index`, `_close_iteration_batched`, `resume`) and their tests, `tools/autocontrast/pixinsight/` (all four `.js`). Modify `tools/autocontrast/README.md` (the Python package is the dev-only golden reference for `modules/autocontrast`; how to regenerate goldens), root `CLAUDE.md` "What ships" (add `modules/autocontrast/` → `AutoContrast-pxm`, ids stable, gated until released).

- [ ] **Step 1: Failing check:** `grep -rn "sidecar\|ExternalProcess\|autocontrast_.*\.js" tools/autocontrast modules/autocontrast` returns only historical docs; the Python suite (`-m 'not live'`) is green after the deletions (count recorded); every golden still regenerates byte-identically (`make_goldens.py` + `diff -r`), proving the reference did not change.
- [ ] **Step 2–4:** FAIL → delete/update → PASS; full AutoContrast ctest, container build, `verify-portable.sh`, every probe script.
- [ ] **Step 5: Commit** `chore(autocontrast): retire the Python sidecar and the PJSR drivers from the shipped path`. Update memory (`autocontrast-pi-script-not-release-compliant` retracted as resolved by deletion; project status note) through `memory-graph`.

---

## Feature Parity Checklist

| Python / PJSR capability | Native home | Task |
|---|---|---|
| starlet, energy spectrum, band-limit (§4.4) | `ac_core/fingerprint` | 4 |
| CIELAB, tonal/saturation quantiles, chroma histogram, background block | `ac_core/fingerprint` | 5 |
| palette classes, gallery bands, chroma family gate (§2.3) | `Palette` | 5 |
| distance (option b), exact chroma EMD | `Distance`, `Emd` | 6 |
| action menu (band-limited), recipe, PI step rendering (§12 audit) | `optimize` | 7 |
| gap-ranked proposer, magnitude buckets | `Propose` | 7 |
| six guardrails (§7), angle-based hue invention | `Guardrails` | 8 |
| beam, bounded retry, convergence (§6.3), fail-safe decline | `Loop` | 9 |
| session JSON (inspectable) | `Session` | 9 |
| NumpyExecutor (offline loop tests) | `ApproxExecutor` (test/tool only) | 9 |
| FITS/TIFF/PNG/JPEG loading, allow-list, downsample factor | `io/Raster`, module FITS path | 10, 16 |
| WCS tiers: header / AVM (strict) / blind / manual | `Wcs`, `Avm`, `TargetWcs`, solver | 11, 16, 19 |
| fingerprint store (provenance guard), cone search | `Store` | 12 |
| ingest, curated seed | `Ingest`, `Seed`, References process | 12, 17 |
| gallery index, crawl/sync (politeness, drift), discovery gates G1–G6 | `discover/*`, `PclTransport` | 13, 14, 17 |
| sidecar `analyze` (solve → lookup → discover → measure) | `AutoContrast` Analyze | 16 |
| PJSR executor (eight processes) | `PiExecutor` | 15 |
| `optimize_begin/step` + PJSR Optimize driver, recipe/report files | `AutoContrast` Optimize, `RunReport` | 16 |
| §3.6 hybrid validation, linear-input refusal, full-res replay (spec'd, never built) | `AutoContrast` | 16 |
| `eval.ranking`, `eval.degrade` CLIs | `ac-tool` | 20 |
| `db.discover sync` CLI | `AutoContrastReferences --sync` | 17 |
| `solve_spike.js` question (can a render be solved?) | tier 3 probe | 19 |
| §10 exit criterion (Task 14 v2) | acceptance | 21 |
| live tests run by default | `run-probe-live.sh`, acceptance | 14, 17, 21 |

## Schedule

Phases A–D (Tasks 1–13) are PCL-free and mostly sequential by interface (3 → 4 → 5 → 6 → 7 → 8 → 9; 10–13 can run after 5 and in parallel with 7–9 when the same files are not touched). Phase E needs the container. Task 19 waits on `gdg/phaseE-solver` being mergeable. Estimated 3–5 weeks of agent time (memory `autocontrast-project-status-native-pcl`).
