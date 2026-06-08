# NukeX v4 — Changelog

## v4.0.1.0 — 2026-04-25

Phase 8: adaptive stretch tuning.  The first NukeX release where the
auto-stretch can *learn from your taste*: rate a stack 1-5 with optional
nudges on four axes (brightness, saturation, color, star bloat), and the
next stack on similar imagery uses your accumulated ratings to nudge its
own parameters.

The learner is a three-layer fallback: a closed-form ridge regression
trained from your own ratings (Layer 3) sits on top of an optional
community-bootstrap model (Layer 2, deferred to v4.0.1.x), which sits on
top of the Phase-5 hard-coded constants this project has shipped since
v4.0.0.4 (Layer 1).  At fresh install the user model is empty and Layer
2 is absent, so every stack falls through to Layer 1 — pixel output is
**bit-identical to v4.0.0.8** (verified end-to-end, 6/6 golden hashes).
The learning kicks in only after you actively click Save on a rating.

### Added
- **Rating popup after every Auto-stretch run.**  At the end of an
  `executeGlobal()` that produced a `NukeX_stretched` window, a modal
  asks "How did the stretch look?" with four signed sliders (-2..+2,
  centred at 0 = "fine") plus an Overall (1..5) slider.  The Color
  axis is hidden for mono / narrowband filter classes.  An "Rate last
  run" button on the NukeX Interface re-opens the dialog for the most
  recent run; a "Don't show after Execute" checkbox in the dialog
  persists the opt-out via PCL Settings under
  `NukeX/Phase8/RatingPopupSuppressed`.
- **Closed-form ridge regression (Layer 3).**  Saving a rating triggers
  an in-process retrain: the user's per-stretch run rows are loaded
  from SQLite, ridge-regressed via Eigen LDLT (no iterative solver, no
  external dependency), and the resulting per-parameter coefficients
  are atomically written to `~/.nukex4/phase8_user_model.json`.
  Atomic write = `tmp + fsync + rename` on POSIX; partial-write or
  power-loss leaves the previous good model in place.  Cross-validated
  R² is recorded per parameter so a future "Explain" UI can surface it.
- **Image-statistics feature extractor (29 columns).**  Each stack
  records per-channel mean / median / MAD / shadow-quartile / highlight
  / dynamic-range / saturated-fraction plus global SNR / star-density /
  noise-floor features as a single `runs` row keyed by a stable
  `run_id`.  Layer 3 trains a separate ridge regression per parameter
  per stretch, so a single user accumulates per-curve, per-axis taste
  data without one curve's behavior leaking into another's prediction.
- **SQLite ratings DB at `~/.nukex4/phase8_user.sqlite`.**  Schema v1
  with WAL journal mode + on-open integrity check.  CRUD lives in
  `src/lib/learning/rating_db.cpp`; an `ATTACH DATABASE` hook is in
  place to layer Phase 8.5's bootstrap rows on top of the user's own
  rows for the per-stretch query.
- **Layer-fallback wiring through `stretch_factory`.**  The factory
  consults `LayerLoader` (Layer 3 → Layer 2 → Layer 1 cascade) for the
  parameters it ships to each `StretchOp::set_param`, with hard
  per-parameter clamps from the new `StretchOp::param_bounds()` so the
  learner can never drive a stretch outside its safe range.  All seven
  Phase-5 curves participate (VeraLux, GHS, MTF, ArcSinh, Log, Lupton,
  CLAHE).

### Changed
- **NukeXInstance now carries a Phase 8 last-run context** (`run_id`,
  filter class, `lastRun` aggregate of the input stats + chosen curve
  + final params).  This is what the rating dialog reads back when
  the user clicks Save, and what the "Rate last run" button needs to
  re-open the dialog without re-stacking.  `run_id` is seeded from
  `std::random_device` so re-launches of PI never collide on a primary
  key.
- **RatingDialog UX polish.**  Sliders shrunk 180 → 120 px, axis
  labels compacted to `Axis  (low <-> high)` form, so the modal is
  narrow enough to sit beside the stretched image rather than
  covering it.

### Deferred — explicitly out of v4.0.1.0 scope
- **Phase 8.5 (community bootstrap):**  ~350 of Scott's labeled
  sessions exported to `share/phase8_bootstrap.sqlite` +
  `share/phase8_bootstrap_model.json`, plus a new E2E golden
  (`lrgb_mono_ngc7635_phase8_bootstrap`) covering the Layer 2
  activation path.  Until this ships, Layer 2 is absent and every
  Layer 3 fallback lands on Layer 1 — verified safe, see "Testing".
- **Phase 8 polish release:**  Reset-to-factory, Reset-to-bootstrap,
  and Explain UI escape hatches; non-modal rating dialog with live
  preview.  Punted to a v4.0.1.x polish release; the v4.0.1.0 modal
  is good enough to ship and the user has a "Don't show after
  Execute" opt-out for users who don't want to rate.

### Implementation notes
- The user-trained model is per-(stretch, parameter) — one ridge
  regression per cell of a 7-curves × ~2-params grid — so coefficients
  can't bleed across curves.  A bug here would corrupt only the cell
  being retrained, not the whole user model.
- `read_param_models_json` clears its output map on any parse failure
  to avoid silently mixing old + new data.  `SaveRatingFromLastRun`
  guards against the corrupt-file case (file exists but won't parse)
  so it doesn't atomically replace a bad-but-readable file with an
  empty one — see `e7709a1`.

### Testing
- ctest serial: 55/55 pass (was 53 at v4.0.0.8 ship; +1
  `test_atomic_write` for the JSON write path, +1
  `test_phase8_fallback` for the Layer 3 → 2 → 1 cascade including
  the missing-Layer-2-and-empty-user-model case that is in fact the
  fresh-install state).
- E2E goldens (6 hashes across primary `lrgb_mono_ngc7635` + GHS /
  MTF / ArcSinh sweep variants) byte-for-byte against v4.0.0.8.  No
  new goldens needed: existing goldens validate the no-rating path,
  Layer 3 activation lives in integration tests with seeded user DB.
- Two subagent-driven code-review catches landed during
  implementation: `7cf0ac3` (unseeded `std::rand()` collision risk on
  `run_id`) and `e7709a1` (corrupt `user_model_json` data-loss
  scenario on retrain).

### Build / plumbing
- **SQLite amalgamation vendored via FetchContent** (same pattern
  as cfitsio in v4.0.0.3) so the released `.so` doesn't depend on
  whatever sqlite the user's PI happens to load.  `ldd NukeX-pxm.so |
  grep -iE "sqlite|curl|ssl"` is empty by design.  The system path
  remains opt-in via `-DNUKEX_USE_SYSTEM_SQLITE=ON` for distro
  packagers.
- **nlohmann/json v3.11.3 vendored** for the user-model JSON
  round-trip.
- New library `src/lib/learning/` housing the rating DB, ridge
  regression, image stats, and per-stretch parameter model.

## v4.0.0.8 — 2026-04-21

Robustness + observability release driven by the first real-data stack on
shipped v4.0.0.7 (M27 Bayer-RGB, 2026-04-20).  Two concrete user-reported
gaps closed; pixel output on well-exposed frames is bit-identical to
v4.0.0.7 (E2E goldens unchanged).

### Added
- **Saturation guard in StarDetector.**  Before running
  `find_local_maxima`, the detector measures the fraction of pixels at
  or above `saturation_level` on a 4× decimated sample of the frame.
  If that fraction exceeds `saturation_reject_fraction` (default 0.5),
  the frame is rejected up front with an empty catalog.  Previously, a
  dawn-twilight frame where a majority of pixels were clipped sent the
  O(n²) exclusion-radius filter chasing a plateau of "local maxima" and
  hung `StarDetector::find_local_maxima` for 5+ minutes per frame.
  New unit tests exercise the hang on 1200×1200 (10.4 s → < 50 ms) and
  verify normal frames with a handful of saturated-core stars still
  detect correctly.
- **Distinctive log line for blown-out frames.**  In the stacking
  engine, a pre-flight saturation check replaces the generic
  `aligned: FAILED (stars=0)` line with
  `aligned: SKIPPED (blown out — X.X% pixels at saturation)` so a user
  watching the Process Console can tell a truly unusable frame from a
  detection misfire.
- **Fit-loop heartbeat in Phase B.**  The OpenMP-parallelised Ceres
  distribution-fitting loop (`gpu_executor.cpp:437`) now emits a
  `fitted K/N voxels (Ts)` line every 2 s from thread 0.  Previously
  the Process Console was silent for the entire 3–4 min single-batch
  fit between kernel 2 and kernel 3, which made long stacks look
  frozen.  The emission is rate-limited via an atomic compare-exchange
  on a shared "last report time" so the observer's mutex never lands
  on the hot compute path; unit tests (`test_fit_heartbeat`) lock in
  the thread-0 gate, the interval gate, and the concurrent-done count.

### Implementation notes
- New public `StarDetector::saturation_fraction(image, level)` helper
  backs both the detector's own guard and the stacking-engine log line
  so the fraction is computed from a single definition.
- New `FitHeartbeat` utility class in `src/lib/gpu` keeps the
  rate-limit / emission logic testable without a full Phase B harness.

## v4.0.0.7 — 2026-04-20

Polish pass on top of v4.0.0.6.  No compute-path changes — pixel output
is bit-identical to v4.0.0.6 (E2E golden hashes unchanged).  The
delta is entirely user-facing clarity, scripting ergonomics, and test
hygiene.

### Added
- **WARNING / CRITICAL** messages in the Process Console on low
  alignment success rate:  > 50 % failed fires `** CRITICAL **` with
  a remediation hint; 10–50 % fires `** WARNING **`; ≤ 10 % stays
  quiet (some drift / clouded frames is normal).
- **Auto-selector rationale** now names the FITS values that drove
  the classification:  `Auto: classified as LRGB-mono (FITS
  FILTER='L', BAYERPAT='', NAXIS3=1) -> VeraLux`.
- **Tooltips** on every NukeX interface control: Primary / Finishing
  Stretch dropdowns + labels, Enable GPU checkbox, frame-list tree
  boxes, and all Add / Remove / Clear / Toggle All buttons.
- **Validate()** now refuses to enable the Execute button when
  `cacheDirectory` is unset-but-non-default-and-unwritable, and warns
  at ExecuteGlobal time when fewer than 5 frames are enabled (Phase B's
  Ceres fitters need n ≥ 5 to converge).
- **FITS header provenance** on all output windows (`NukeX_stacked`,
  `NukeX_noise`, `NukeX_stretched`):  `CREATOR`, `NUKEXVER`,
  `NUKEXIMG`, `NFRAMES`, `NFAILALN`, `HISTORY` entries.  The stretched
  window additionally carries `PRIMSTR` / `FINSTR` with the applied
  curve names.

### Changed
- **Filter classifier** now case-folds the `FILTER` keyword before the
  narrowband-name lookup.  Previously depended on the FITS reader's
  uppercase normalisation; now robust to any casing upstream might
  produce.
- **ctest** runs all 45 tests in 94 s with no `-E` exclusion list.
  Heavy exploratory / integration / visual / sweep cases are re-tagged
  as opt-in Catch2 dot-prefixed tags (`[.integration]` etc.) so they
  skip by default but are still callable via the corresponding filter.

### Fixed
- **`tools/run_e2e.sh` and `tools/release.sh`** now use `set -o
  pipefail` so `PixInsight.sh … | tee file` surfaces a PI crash
  instead of silently reporting success.  `run_e2e.sh` also wraps PI
  in `timeout --kill-after=30s ${NUKEX_E2E_TIMEOUT:-3600}`.

### Testing
- Golden-hash E2E regression now covers the dropdown-sweep variants
  (`GHS`, `MTF`, `ArcSinh`) as well as the primary path, so a
  per-curve regression in (say) MTF while VeraLux stays stable is
  caught bitwise.
- New regression tests for the `isfinite` guard in `StudentTNLL::
  Evaluate` and `ContaminationNLL::Evaluate`:  extreme-outlier and
  all-identical-samples inputs that used to CHECK-abort the process
  at `ceres/line_search.cc:705` now exit the fitter cleanly.
- Thirteen mixed-case narrowband filter names (`Ha`, `ha`, `hA`,
  `h-alpha`, `Narrowband`, `nb`, …) lock in the defensive case-fold.

### Plumbing
- Module version macros centralised in `src/module/NukeXVersion.h`
  (previously only in `NukeXModule.cpp`, so any other translation unit
  that wanted them had to hardcode).  Version bumps now require
  editing one place.

## v4.0.0.6 — 2026-04-20

### Added
- **Alignment diagnostics exposed on PJSR.** Two new read-only process parameters — `nFramesProcessed` and `nFramesFailedAlignment` — are populated from the stacking result after `executeGlobal()`. Scripted callers can read them directly (`P.nFramesFailedAlignment`) instead of regex-parsing the Process Console log. The E2E harness (`tools/validate_e2e.js`) now fails a case if any frame failed alignment, rather than relying on wall-time and execute-ok alone.
- **Informative per-frame alignment log.** The Process Console used to emit `aligned (200 stars)` for every frame whether alignment succeeded or failed — a real UX trap that contributed to the NGC7635 alignment-failure regression going unnoticed before the v4.0.0.5 investigation. It now emits the actual outcome, inlier count, and sub-pixel RMS, e.g. `aligned: ok (stars=200, inliers=83, rms=0.088 px)` or `aligned: FAILED (stars=200, inliers=0, rms=0.000 px)`.

### Build / tooling
- **`make sign` and `make package`** now work out of the box: `tools/release.sh` drives the full mechanical post-build pipeline (sign `.so`, stage into `repository/bin/`, create date-stamped tarball, compute SHA-1, patch `updates.xri` with new `fileName`/`sha1`/`releaseDate`, re-sign the XRI). The previous `cmake/PackageAndSign.cmake` was never included and had three latent bugs (wrong target name, wrong tarball filename, no SHA-1 update) that would have surfaced the first time anyone typed `make package`.
- **`make e2e` / `make e2e-regen` hardened.** Invocation moved to `tools/run_e2e.sh` after the initial CMake inlining hit a `$$`-escape bug where `$NUKEX_E2E_REGEN` expanded to `$$` (shell PID) at configure time, corrupting the manifest path. The driver now also wipes the manifest's `output_root` before invoking PI as a belt-and-braces against any stale FITS triggering an overwrite prompt.
- **Silent-overwrite of harness FITS outputs.** `ImageWindow.saveAs` was being called with `verifyOverwrite=true`, which pops a modal "replace?" dialog in headless `--automation-mode`. Changed to `false`.

## v4.0.0.5 — 2026-04-20

### Fixed
- **Alignment: replaced the positional nearest-neighbour star matcher** with Groth (1986) / Valdes (1995) triangle similarity matching (K=100, canonical vertex ordering, sorted-r1 window index, vote-based correspondences). On real multi-hour imaging sessions with cumulative tracking drift >> 5 px, the old matcher reported "aligned (N stars)" but produced 0 usable matches in nearly all frames — the E2E baseline on NGC7635 L/Lights (65 frames, 3.5 hr) dropped from 61/65 failed alignments to **0/65**.
- **Phase B: Ceres solver crash on pathological voxels.** `StudentTNLL::Evaluate` and `ContaminationNLL::Evaluate` now return `false` instead of letting a non-finite cost/gradient reach `ceres/line_search.cc:705`'s CHECK-fail (which aborted the process after ~26 min on a full stack).

### Performance
- **Phase B per-voxel Ceres fitting now parallelised across CPU cores** (OpenMP `parallel for` with dynamic-256 scheduling). Each voxel's fit is independent: `ModelSelector::select` allocates its fitters as stack locals, so there is no shared mutable state to synchronise.
  - **Measured: 9.35× Phase B speedup** on NGC7635 L/Lights (32.86 min → 3.51 min). Total stack wall-time 34 min → 4.6 min. Ship bar was 1.5×.

### Observability
- **`NukeXProgress` emits three sideband progress channels** so headless harnesses (and curious users watching a long stack) can see liveness even during silent Ceres compute:
  1. `std::cerr` line per progress event (reaches shell via `2>&1 | tee`).
  2. Append log at `/tmp/nukex_progress.log` (`tail -f` from any terminal).
  3. 10-second heartbeat watchdog thread writes `/tmp/nukex_heartbeat.txt` with elapsed seconds + last phase/detail, proving liveness even when no callbacks fire.

### Testing
- **E2E validation harness** (`tools/validate_e2e.js` + `make e2e`). Runs the full pipeline against a manifest of test stacks, verifies execute-ok, wall-time budgets, 0-alignment-failure, and bitwise regression via FNV-1a hash of raw pixel samples (not file SHA — avoids the FITS write-time-stamp problem). Per-case golden hashes in `test/fixtures/golden/`. Dropdown sweep (`GHS`, `MTF`, `ArcSinh`) verified to produce distinct stretched outputs.
- **Baseline fixture:** `test/fixtures/phaseB_baseline_ms.txt` captures the authoritative Phase B floor (**1,971,756 ms** on NGC7635, pre-OpenMP) that B6 measures the post-optimisation run against.
- New `test_alignment_diag.cpp` (tag `[.diag]`, opt-in) loads real NGC7635 frames and sweeps matcher parameters for future debugging. Existing homography tests updated: 5×4 grid and collinear-point layouts replaced with deterministic-random positions (grids had too many congruent triangles for triangle-matching descriptors to discriminate).

### Build / plumbing
- `tools/capture_baseline.js` rewritten for PI `--automation-mode` quirks discovered while getting headless harnesses to work: `File.environmentVariable` does not see shell env (use `jsArguments`), `Console.writeln` does not reach shell stdout (wrap in `Console.beginLog()`/`endLog()`), enum values sharing an integer value collide on the prototype (use integer literals). See memory `reference_pjsr_automation_quirks.md`.
- `docs/superpowers/plans/2026-04-19-phase7-perf-findings.md` documents the flamegraph + selected optimisation + actual 9.35× speedup.
- `.gitignore` covers `Testing/`, `repository/bin/`, `build_profile/`, `.claude/`.

## v4.0.0.4 — 2026-04-20

### Added
- **Stretch pipeline wired to output.** New `NukeX_stretched` window opens alongside `NukeX_stacked` and `NukeX_noise`. Previously the module stacked but never applied the configured stretch.
- **Metadata-driven Auto selection.** `primaryStretch` defaults to `Auto`; the module reads FITS `FILTER`/`BAYERPAT`/`NAXIS3` from the first light, classifies into one of LRGB-mono / LRGB-color / Bayer-RGB / Narrowband, and picks the Phase-5 champion curve (VeraLux today across all classes). The classification is logged to the Console.
- **Finishing stretch slot.** New `finishingStretch` parameter; only `None` is enrolled this release. SAS / OTS / Photometric are deferred to later phases per their own dedicated brainstorms.

### Changed
- **Schema break:** the flat 10-entry `stretchType` enum and `autoStretch` bool are replaced by `primaryStretch` (Auto + 7 curves: VeraLux, GHS, MTF, ArcSinh, Log, Lupton, CLAHE) and `finishingStretch` (None only). Saved projects referencing the old IDs revert to defaults.

### Deferred to v4.0.0.5
- Phase B Ceres-fitting perf optimization (>=1.5x target on real workload).
- PJSR E2E validation harness (`make e2e`) with bitwise regression across a 4-stack corpus.

### Build
- Added optional `NUKEX_PROFILING=ON` CMake flag that adds `-fno-omit-frame-pointer` to support perf flamegraph capture for the v4.0.0.5 Phase B work.
- Added `#include <chrono>` Phase-B wall-time instrumentation in the stacker; logs `Phase B: <n> ms` via the progress observer.
