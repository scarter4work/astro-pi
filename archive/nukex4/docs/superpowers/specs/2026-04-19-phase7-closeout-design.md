# Phase 7 Close-Out — v4.0.0.4 Release Design

**Date:** 2026-04-19
**Target release:** v4.0.0.4
**Status:** Draft — awaiting user review
**Preceding memory:** `project_phase7_closeout.md`, `project_phase6_progress.md`

## Context

Phases 1–6 have landed: lib IO, alignment, stacking engine (Phase B Ceres fitting), voxel cube, stretch pipeline (10 stretches, optimized defaults), progress observer, GPU executor with CPU fallback. v4.0.0.3 is the most recent release (cfitsio vendored fix).

Current gap in the shipped module: the stacking pipeline runs end-to-end but the stretch pipeline is never applied to the output. `NukeXInstance::ExecuteGlobal()` (`src/module/NukeXInstance.cpp:141`) memcpy's the stacked `Image` into an `ImageWindow` directly, ignoring `stretchType` and `autoStretch` parameters entirely. This is Phase 7's correctness fix.

Alongside the fix, Phase 7 closes out two deferred items: profiling Phase B for one justified optimization, and building automated end-to-end validation in real PixInsight.

## Scope

Three tightly-scoped work items:

1. **Wire stretch into output path, including real auto-selection (C.1).** Introduce a `primaryStretch` enum (replaces the flat `stretchType`) and a `finishingStretch` enum (new, only `None` enrolled for this release). `Auto` in `primaryStretch` selects a curve based on FITS metadata (filter class). Output adds a third window `NukeX_stretched` alongside stacked and noise.
2. **Phase B Ceres fitting: profile with `perf`, land one optimization with ≥ 1.5× measured speedup.**
3. **End-to-end validation harness** driven by PJSR + `PixInsight.sh --automation-mode`, gating all 18 pass/fail checks per stack across a 4-stack corpus. Bitwise regression included.

**Non-goals (deferred):**
- macOS / Windows builds
- Stretch-parameter UI for manual tuning
- Stats-driven parameter tuning (→ Phase 8)
- Adaptive learning DB (→ Phase 9)
- SAS blend, OTS custom targets, Photometric catalog (those enum entries stay hidden from the `finishingStretch` dropdown until their respective phases land)

## Design

### 1. Stretch wiring + Auto-selection (C.1)

**Parameter schema change** in `NukeXParameters.cpp`:
- **Remove:** the flat `stretchType` enum (10 entries), the unused `autoStretch` bool.
- **Add:** `primaryStretch` enum with values `Auto`, `VeraLux`, `GHS`, `MTF`, `ArcSinh`, `Log`, `Lupton`, `CLAHE`. Default `Auto`.
- **Add:** `finishingStretch` enum with value `None`. Default `None`. (Future phases add `SAS`, `OTS`, `Photometric` entries.)

`primaryStretch` has no `None`/`Linear` entry intentionally: the `NukeX_stacked` output window is already the linear output. Users who want linear data use that window; users who want stretched data use the new `NukeX_stretched` window. A "no-stretch stretched window" would be redundant and invite confusion.

This is a schema break, acceptable because v4 has no shipped saved projects referencing the old names.

**Auto-selection module** — new file `src/module/stretch_auto_selector.cpp` (+ header):

```
struct AutoSelection {
    std::unique_ptr<StretchOp> op;
    std::string log_line;  // e.g. "Auto: classified as LRGB-mono → VeraLux"
};

AutoSelection select_auto(const FITSMetadata& meta);
```

Implementation:
1. Classify the image into one of three classes by reading FITS keywords: `FILTER`, `BAYERPAT`, `NAXIS3`.
   - `NAXIS3 == 3` and no `BAYERPAT` → `LRGB-color`
   - `BAYERPAT` present → `Bayer-RGB`
   - `FILTER` matches Ha / H-alpha / OIII / O3 / SII / S2 / narrowband → `Narrowband`
   - Otherwise → `LRGB-mono`
2. Look up the champion `StretchOp` constructor for that class from a compile-time dispatch table. Today all four map to `VeraluxStretch{}` (Phase 5 champion); the table structure means future phases can change one entry without touching the selector.
3. Construct the op and return both the unique_ptr and a formatted log line.

**Stretch factory** — new file `src/module/stretch_factory.cpp` (+ header):

```
std::unique_ptr<StretchOp> build_primary(PrimaryStretchEnum e,
                                         const FITSMetadata& meta,
                                         std::string& out_log_line);

std::unique_ptr<StretchOp> build_finishing(FinishingStretchEnum e);
```

`build_primary(Auto, meta, log)` delegates to `select_auto(meta)` and fills `out_log_line`. Other enum values construct the named op directly and return an empty log line. `build_finishing(None)` returns `nullptr`.

**Integration point** in `NukeXInstance::ExecuteGlobal()`:

After the existing `result.stacked` handling and before creating the stacked `ImageWindow`, insert:

```
FITSMetadata meta = extract_metadata(light_paths.front());
std::string auto_log;
auto primary_op    = build_primary(params.primaryStretch, meta, auto_log);
auto finishing_op  = build_finishing(params.finishingStretch);

if (!auto_log.empty()) progress.message(auto_log);

Image stretched = result.stacked.clone();
StretchPipeline pipeline;
pipeline.add(std::move(primary_op));
if (finishing_op) pipeline.add(std::move(finishing_op));
pipeline.execute(stretched);
```

Then create a third `ImageWindow` named `NukeX_stretched` and memcpy stretched pixels into it, matching the existing pattern for stacked and noise.

**FITS metadata extraction** — single helper reading FILTER/BAYERPAT/NAXIS3 from the first light frame. We read the first light only: all frames in a stack share the filter; this is a near-universal astrophotography convention and the current pipeline already assumes it.

### 2. Perf profiling approach

**Tool:** Linux `perf record` + FlameGraph scripts.

**Build:** profiling build adds `-fno-omit-frame-pointer` to the release flags via a CMake option `NUKEX_PROFILING=ON`. Release build stays unchanged.

**Prereqs:** `kernel.perf_event_paranoid ≤ 1` (one-time sysctl). Ceres symbols require the system Ceres to have frame pointers; if flamegraph stacks terminate at `ceres::Solve()` without descending, contingency is a one-off rebuild of Ceres from source with `-fno-omit-frame-pointer`.

**Measurement:** single `std::chrono::steady_clock` timer around `execute_phase_b()` in `stacking_engine.cpp`, logged via `progress.message()` as `"Phase B: <n> ms"`. Not permanent section-timer infrastructure — one log line.

**Bottleneck identification bar** (pre-committed; prevents confirmation bias):
- One section accounts for ≥ 50% of Phase B wall time, OR
- One section has per-call cost ≥ 3× its siblings, OR
- A clearly-redundant operation (object-per-call, repeated allocation) is visible.

If none of the above: we report honestly that Phase B is uniformly slow, and **do not ship a micro-optimization for appearance**. The right response would be a structural change in a later phase, not a fake win here.

**Optimization ship bar** (pre-committed):
- ≥ 1.5× speedup on Phase B wall time measured on a real workload (test stack #1 from the validation corpus).
- Bitwise regression test still passes (Section 3 check #7).
- No new external library dependencies (in-tree code changes only).

### 3. End-to-end validation

**Invocation:**
```
/opt/PixInsight/bin/PixInsight.sh \
    --automation-mode --force-exit \
    -r=tools/validate_e2e.js
```

Wrapped as a CMake target `make e2e`. Gating — v4.0.0.4 does not release without `make e2e` returning 0.

**Harness:** new file `tools/validate_e2e.js` (PJSR). Responsibilities:
1. Load fixture list from `test/fixtures/e2e_manifest.json`, verify all input FITS paths exist (fail fast with clear message if not).
2. For each test case: construct a `NukeXInstance`, configure parameters, `.launch()`, inspect output windows via `ImageWindow.windowById()`.
3. Run all pass/fail checks for that case.
4. On any failure: `Console.criticalln()` with the specific check that failed, write JSON report to `build/e2e_report.json`, `throw` (propagates to non-zero exit via `--force-exit`).
5. On full pass: write JSON report with `status: "PASS"`, normal exit.

**Xvfb contingency:** if PI's headless mode requires a display, wrap the invocation with `xvfb-run`. Confirmed in the plan's first implementation task.

**Corpus — all gating:**
1. **LRGB mono, medium.** 30–50 frames, ~4K × 4K, from `~/projects/processing/`.
2. **Bayer RGB, medium.** 30–50 frames from OSC data in `/mnt/qnap/astro_data/`. If no real OSC session exists in recent folders, fallback is a synthetic Bayer-packed test — flagged as honest shortcut in the test manifest.
3. **Narrowband, medium.** 30–50 frames (Ha, O3, or S2).
4. **Stress — LRGB mono, large.** Largest realistic dataset ≥ 100 frames at ≥ 4K. Exercises progress observer under sustained load.

**Pass/fail checks per stack (all gating):**

Module & execution:
1. Module loads without error (no missing symbols, no SIGSEGV, no console `[ERROR]`-severity messages).
2. Stack completes with non-zero exit from the PJSR harness.
3. Stack completes within a per-case wall-time budget (tunable via manifest).

Stacked output correctness:
4. `NukeX_stacked` window exists with correct dimensions and channel count.
5. All pixel values in [0, 1] across every channel.
6. No NaN / Inf.
7. **Bitwise regression:** stacked-image bytes hash to the SHA-256 stored in `test/fixtures/golden/<name>.sha256`. Regenerable only via `NUKEX_REGENERATE_GOLDEN=1`.

Noise map:
8. `NukeX_noise` window exists, same dimensions, non-zero content.
9. Noise map pixel values in [0, 1].
10. Bitwise regression against `test/fixtures/golden/<name>_noise.sha256`.

Stretched output (new in v4.0.0.4):
11. `NukeX_stretched` window exists, same dimensions.
12. Pixel values in [0, 1].
13. Histogram sanity: stretched-image median > stacked-image median.
14. Auto-selection log line matches expected pattern (`"Auto: classified as <class> → <curve>"`).
15. Dropdown-drives-pipeline: test stack #1 repeated with `primaryStretch` = GHS, MTF, ArcSinh produces three stretched images with pairwise-distinct SHA-256 hashes.

Progress & cancellation:
16. Progress callbacks fire with monotonically non-decreasing percentage.
17. Cancellation during Phase B returns cancelled-status within 10 seconds; no output windows are created.

Performance:
18. Phase B wall-time on test stack #1 ≤ `test/fixtures/phaseB_baseline_ms.txt` × (1/1.5). Baseline captured pre-optimization and committed.

**Golden file management:** `test/fixtures/golden/` is in git (tiny text files). Input FITS is not (size); the harness fails fast with a clear `"missing fixture: <path>"` message when data is absent.

**Determinism prerequisite (already verified):**
- No thread-shared atomics in `src/lib/`.
- Fixed-seed RNG in homography RANSAC and reservoir.
- Ceres is single-threaded per solve (default `num_threads = 1`).
- Per-voxel independence — no cross-pixel reductions.

**Cross-machine GPU fallback (risk):** if golden SHA-256s diverge between machines with different OpenCL drivers, we make GPU-path hashes machine-local and fall back to statistical regression (mean/stddev/MAD within 0.01%) only for the GPU path, keeping bitwise regression for the CPU fallback. Pulled only if a real cross-machine failure occurs; bar is not weakened preemptively.

### 4. Release process

Per the **PixInsight Release Workflow** in global CLAUDE.md.

Pre-build:
1. Bump `MODULE_VERSION_BUILD` from 3 to 4.
2. Update `MODULE_RELEASE_YEAR/MONTH/DAY` to ship date.
3. Write release notes to CHANGELOG: new Auto-selection, stretch wiring, Phase B perf number, bugfixes.

Build & test gate (all required):
4. `make clean && make -j$(nproc)` (or `make release`).
5. `ctest --output-on-failure` — all unit tests pass.
6. `make e2e` — all 18 checks per stack across 4-stack corpus pass.

Signing:
7. Sign module: `PixInsight.sh --sign-module-file=build/bin/NukeX-pxm.so --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk --xssk-password="…"`

Packaging:
8. `make package` → creates dated tarball, updates `updates.xri` with name + SHA1, verifies SHA1 matches tarball.
9. Sign XRI: `PixInsight.sh --sign-xml-file=repository/updates.xri --xssk-file=... --xssk-password=...`

Commit & push:
10. Single commit: version bump + CHANGELOG + tarball + signed XRI. Message format matches recent releases.
11. `git tag v4.0.0.4 && git push origin main --tags`.

Hard rules (restated from global CLAUDE.md):
- **NEVER `sudo make install`.**
- **NEVER skip a step.**
- **NEVER push with failing tests.**

## Files created

- `src/module/stretch_auto_selector.hpp` + `.cpp`
- `src/module/stretch_factory.hpp` + `.cpp`
- `src/module/fits_metadata.hpp` + `.cpp` (metadata extraction helper)
- `tools/validate_e2e.js` (PJSR test harness)
- `test/fixtures/e2e_manifest.json` (test case declarations)
- `test/fixtures/golden/<name>.sha256` × N (golden bitwise hashes)
- `test/fixtures/phaseB_baseline_ms.txt` (perf baseline)

## Files modified

- `src/module/NukeXInstance.cpp` — insert stretch pipeline call, add third output window.
- `src/module/NukeXParameters.cpp` + `.hpp` — replace flat `stretchType` + `autoStretch` with `primaryStretch` + `finishingStretch`.
- `src/module/NukeXInterface.cpp` — update UI dropdown bindings.
- `CMakeLists.txt` — add `make e2e` target, `NUKEX_PROFILING` option.
- `CHANGELOG.md` — v4.0.0.4 notes.

## Open risks

1. **PI headless display requirement.** Resolved by first plan task — `xvfb-run` fallback if needed.
2. **Ceres symbol depth in perf.** Contingency: rebuild Ceres from source with frame pointers.
3. **GPU bitwise cross-machine.** Contingency: statistical regression for GPU path only.
4. **Corpus data availability.** If four distinct real stacks aren't available in user's data, specific cases may need OSC-synthetic fallback (for Bayer) or scaled-down stress (for large-LRGB).

Each risk has a concrete contingency. No risk blocks starting the plan.

## Success definition

v4.0.0.4 ships when:
- All four corpus stacks pass all 18 E2E checks.
- Phase B achieved ≥ 1.5× speedup, bitwise regression intact.
- Signed module and signed XRI committed and pushed.
- Release notes published.

No partial releases. No "informational" escapes. The gate is the gate.
