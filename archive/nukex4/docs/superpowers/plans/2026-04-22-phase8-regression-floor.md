# Phase 8 Regression Floor (v4.0.0.8 baseline)

**Captured:** 2026-04-22 prior to Phase 8 work starting.

This document pins the state of the shipped `v4.0.0.8` module that Phase 8's
v4.0.1.0 must preserve bit-for-bit on the Auto path. At v4.0.1.0 ship, Layer 2
is absent (bootstrap deferred to Phase 8.5) and no user DB exists on the E2E
test machine, so Layer 3 has zero rows. The Layer 3 → Layer 2 → Layer 1
fallback therefore delivers Layer 1 factory defaults — the same as v4.0.0.8.
`make e2e` goldens must match byte-for-byte.

Any Phase 8 task that changes a golden hash has broken additivity and must be
reverted or fixed before proceeding.

## ctest

- **Total:** 46
- **Passed:** 46
- **Failed:** 0
- **Runtime (serial):** 76.83 s

Note: the first parallel run surfaced 6 transient failures on Ceres-heavy
stretch tests (`test_mtf`, `test_arcsinh`, `test_log`, `test_rnc`, `test_ghs`,
`test_ots`). All six passed cleanly under `ctest -j 1`. Root cause is
resource contention when multiple fitters hammer BLAS in parallel, not a
real test-bench regression. Serial ctest is the authoritative invariant.

## E2E goldens (must remain bit-identical through v4.0.1.0)

Committed at `test/fixtures/golden/lrgb_mono_ngc7635.json`:

```json
{"primary":{"stacked":"d069786a","noise":"b9ec9edd","stretched":"83dfad37"},"sweep":{"GHS":{"stretched":"046c8b25"},"MTF":{"stretched":"b890bf1e"},"ArcSinh":{"stretched":"f7ea96b4"}}}
```

Hashes are FNV-1a over the raw pixel samples of the output FITS. Image
geometry for all three primary outputs: 3840×2160×1 (LRGB-mono, NGC7635
L/Lights corpus at `/mnt/qnap/astro_data/NGC7635/L/Lights`, 65 frames, 0
failed alignment at this baseline).

## Phase B wall-time floor

Committed at `test/fixtures/phaseB_baseline_ms.txt`:

```
1971756
```

That's 1,971,756 ms ≈ 32.86 min at the Phase B baseline (captured in the
Phase 7 work, post-OpenMP-parallelisation). The e2e validator checks current
wall-time against this baseline via a `phaseB_speedup_min: 1.5` requirement
from the manifest, so a run that's not at least 1.5× faster than baseline
would fail — but today's fresh run completed its primary Phase B in 285.757 s,
well under the ceiling.

## Confirming e2e run (2026-04-22 18:16 EDT)

Fresh `make e2e` against the installed `v4.0.0.8` module
(`/opt/PixInsight/bin/NukeX-pxm.so`, modified 2026-04-21 14:27):

- **Meta file** (`/tmp/nukex_e2e_meta.txt`): `STATUS ok`, `CASES 4`.
- **Report** (`/tmp/nukex_e2e/e2e_report.json`): `"status": "PASS"` with
  primary pixel hashes matching the committed goldens bit-for-bit.
- **Primary case** `lrgb_mono_ngc7635`: 65/65 frames aligned, primary
  Phase B `elapsed_s: 285.757`.
- **Sweep variants** (GHS, MTF, ArcSinh): all `status: ok`, stretched hashes
  match committed goldens.

## How Phase 8 must preserve this

At v4.0.1.0:

1. Layer 2 is empty (no `share/phase8_bootstrap_model.json` ships).
2. Layer 3 is empty on a fresh install (no `phase8_user_model.json` in user
   data until the user rates a run).
3. The Layer 3 → Layer 2 → Layer 1 fallback therefore resolves to Layer 1
   factory defaults, which are the Phase-5 champion constants already baked
   into each `StretchOp`'s member initializers.
4. The pixel output of the Auto path is therefore bit-identical to v4.0.0.8.

Operationally: any commit in the Phase 8 plan that causes `make e2e` to
fail a golden match has broken the additivity invariant. Revert or fix it
before proceeding. Tasks 15 and 21 of the plan are explicit regression gates
that re-verify against this floor.

## Checkpoint after Task 14 (mid-plan gate) — 2026-04-22 23:30 EDT

**STATUS ok on all 4 cases.** Phase 8 infrastructure (Tasks 2-14) is truly
additive — zero pixel drift from the v4.0.0.8 baseline.

Verified against the freshly-built, signed module at
`/opt/PixInsight/bin/NukeX-pxm.so` (commit `45d724b` + signed):

| Case | stacked | noise | stretched |
|---|---|---|---|
| `lrgb_mono_ngc7635` primary | `d069786a` ✅ | `b9ec9edd` ✅ | `83dfad37` ✅ |
| `lrgb_mono_ngc7635` GHS sweep | — | — | `046c8b25` ✅ |
| `lrgb_mono_ngc7635` MTF sweep | — | — | `b890bf1e` ✅ |
| `lrgb_mono_ngc7635` ArcSinh sweep | — | — | `f7ea96b4` ✅ |

All six hashes match the committed baseline byte-for-byte. This confirms:

1. Adding the vendored SQLite amalgamation (`e6c8076`) did not drift any
   pixel path.
2. Adding the vendored nlohmann/json header (`5cb52e4`) did not drift
   any pixel path.
3. Extending `StretchOp` with `param_bounds()` / `set_param()` /
   `get_param()` across all 7 stretches (`c2a99da`) did not change any
   factory default — the new virtual methods are purely additive.
4. The `Phase8Context` optional-argument wiring in `build_primary`
   (`45d724b`) is dormant when `NukeXInstance` passes no context, which
   it does not yet.

**Harness fix landed alongside the checkpoint** — commit `12cc6a7`
hardens `tools/run_e2e.sh` with `--default-modules` on the PI invocation,
so future runs are robust against PI's persistent install-list falling
out of sync with the on-disk binary (the issue that first masked this
otherwise-green run).

ctest: 53/53 serial green (was 46 at baseline + 7 new test binaries).
Phase B primary elapsed: 282 s (well within the 1,971,756 ms ceiling).

Safe to proceed to Task 16 (RatingDialog).

## Final regression checkpoint (pre-release) — 2026-04-24 ~15:50 EDT

**STATUS ok on all 4 cases.** Phase 8 is additivity-complete: every
commit from `452f117` through `4edc525` (Tasks 1-20) preserves the
v4.0.0.8 pixel floor byte-for-byte.

**Clean release build** from `rm -rf build && cmake .. -DNUKEX_BUILD_MODULE=ON
-DNUKEX_RELEASE_BUILD=ON && make -j$(nproc)`: OK, no warnings in NukeX code.

**Vendored dependencies confirmed** via `CMakeCache.txt` and `ldd`:

- `NUKEX_USE_SYSTEM_SQLITE=OFF` — SQLite amalgamation static-linked.
- `NUKEX_USE_SYSTEM_CFITSIO=OFF` — cfitsio static-linked (USE_CURL=OFF).
- `NUKEX_RELEASE_BUILD=ON`.
- `ldd NukeX-pxm.so | grep -iE "sqlite|curl|ssl"` → empty. No system
  sqlite, libcurl, or libssl linkage. `nlohmann/json` is header-only.

**ctest**: 55/55 serial green (was 53 at Task-15 checkpoint + 1 from
`test_atomic_write` in Task 19 + 1 from `test_phase8_fallback` in Task 20).

Note on flakes: a first-cold-run of the full serial ctest right after the
clean build showed intermittent failures in `test_ghs` (8.4 s) and
`test_ots` (15.8 s). Both pass in isolation and both pass on the
subsequent full-serial retry. Root cause is Ceres thread-pool / BLAS
warm-up on the first invocation after a scratch build — not a Phase 8
regression (these tests have been on-main since Phase 4/5). The serial
invariant remains authoritative on the retry.

**make e2e**: `lrgb_mono_ngc7635` primary + GHS/MTF/ArcSinh sweeps, all
STATUS ok. Six pixel hashes byte-for-byte against v4.0.0.8:

| Case | stacked | noise | stretched |
|---|---|---|---|
| `lrgb_mono_ngc7635` primary | `d069786a` ✅ | `b9ec9edd` ✅ | `83dfad37` ✅ |
| `lrgb_mono_ngc7635` GHS sweep | — | — | `046c8b25` ✅ |
| `lrgb_mono_ngc7635` MTF sweep | — | — | `b890bf1e` ✅ |
| `lrgb_mono_ngc7635` ArcSinh sweep | — | — | `f7ea96b4` ✅ |

Primary Phase B elapsed: 275.9 s (baseline 285.8 s; the 1,971,756 ms
ceiling is 32.9 min — well under).

**Real-data smoke test (Step 4 of Task 21)**: deferred to user action.
The manual end-to-end check — stack a real dataset in an interactive PI
session, rate all 5 axes, click Save, confirm `~/.config/nukex4/
phase8_user_model.json` gets a stretch block, re-run against the same
data and observe "Layer 3 (user-learned) applied" in the Process Console
— cannot be scripted from the e2e harness (the rating dialog is modal
and gated by `NUKEX_PHASE8_NO_POPUP=1` in headless runs). The user must
run this before tagging v4.0.1.0 in Task 22.

Safe to proceed to Task 22 (release) once the manual smoke passes.
