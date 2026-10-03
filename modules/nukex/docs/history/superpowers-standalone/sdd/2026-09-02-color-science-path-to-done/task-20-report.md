# Task 20 Report: Synthetic-FITS writer; un-gate the integration tests; add the generic-camera case

## Status: BLOCKED (Task 20's own deliverables are DONE; 3 downstream assertions are blocked on a pre-existing bug outside this task's files)

## What I implemented

1. **`test/util/synthetic_fits.hpp` / `.cpp`** — exactly as specified in the brief, in `namespace nukex::test_util`:
   - `write_synthetic_bayer` — uniform-value Bayer frame with BAYERPAT/INSTRUME/FILTER headers.
   - `write_synthetic_mono` — uniform-value mono frame, no BAYERPAT.
   - `write_synthetic_q_solved_hao3` / `write_synthetic_q_solved_s2o3` — build the (camera, filter) Q matrix from the fixture QE DB via `ChannelDecomposer::build_q`, multiply by the target `(line1, line2)` vector, and lay the resulting (R,G,B) onto an RGGB mosaic so a Q-solve exactly recovers the engineered lines. `instrume` empty → INSTRUME = camera; non-empty → written verbatim (drives the Task 15b generic-camera fallback test while the Q matrix still comes from the real `camera` argument).

2. **`test/unit/io/test_synthetic_fits.cpp`** — the writer's own unit test, 4 cases, verbatim from the brief.

3. **`test/CMakeLists.txt`**:
   - `test_util` static library now also builds `util/synthetic_fits.cpp` and links `nukex4_calibration PUBLIC` + `cfitsio PRIVATE` (in addition to the existing `nukex4_io`, `nukex4_stretch`).
   - Registered `nukex_add_test(test_synthetic_fits unit/io/test_synthetic_fits.cpp test_util nukex4_calibration nukex4_io)` immediately after the `test_util` block.

4. **`test/integration/test_phase_a_router.cpp`** — fully un-gated. Removed the gate header comment, the `WIRED_BY_TASK_20` macro, and the per-case `#if/#else SKIP/#endif` wrapping from all 5 `TEST_CASE`s, replacing `test_data_loader.hpp` with `synthetic_fits.hpp`. All 5 cases now run their real bodies.

5. **`test/integration/test_phase_b_qsolve.cpp`** — 3 of its 4 pre-existing cases un-gated the same way (recovers-HaO3, pure-broadband-produces-none, HaO3+S2O3-mixed-merge). Appended the new generic-camera-fallback `TEST_CASE` from the brief verbatim.

## Deliberate deviation from the literal self-review checklist: one `SKIP(` remains

The brief's "Produces" interface for Task 20 lists exactly 4 writer functions: `write_synthetic_bayer`, `write_synthetic_mono`, `write_synthetic_q_solved_hao3`, `write_synthetic_q_solved_s2o3`. The 4th pre-existing gated case in `test_phase_b_qsolve.cpp` ("negative emission clamped, counter incremented") calls `test_util::write_synthetic_negative_emission_hao3` — a function **not** in that interface and not shown anywhere in the brief's implementation code.

I did not invent a 5th writer function beyond the specified interface (that would be scope creep — "nothing beyond the brief" per my instructions). Instead I left that one case's `#if/#else/SKIP/#endif` structure in place (now `#if 0` since the `WIRED_BY_TASK_20` macro is gone from the rest of the file and wasn't reintroduced), with a comment explaining why. This reconciles exactly with the task title's arithmetic: "un-gate the **8** integration tests" (5 in phase_a + 3 real ones in phase_b) + "add the generic-camera case" (+1) = 9 real bodies, and the team lead's stated expectation of "5 test cases" in the phase_b binary (4 pre-existing TEST_CASE blocks, one of which stays SKIPped, + 1 new) matches what I built and what the binary reports.

If a `write_synthetic_negative_emission_hao3` writer is wanted, that's a scope decision for a follow-up task — I did not add it silently.

## Tests and results

### TDD evidence for the writer (RED → GREEN)

**RED** — before `synthetic_fits.hpp` existed:
```
cd build && cmake .. 2>&1 | tail -1 && make test_synthetic_fits 2>&1 | grep -E "error" | head -2
```
```
-- Build files have been written to: /home/scarter4work/projects/nukex5/build
/home/scarter4work/projects/nukex5/test/unit/io/test_synthetic_fits.cpp:2:10: fatal error: synthetic_fits.hpp: No such file or directory
```

**GREEN** — after implementing the writer:
```
./test/test_synthetic_fits
```
```
Randomness seeded to: 3900555616
===============================================================================
All tests passed (16 assertions in 4 test cases)
```

### Full ctest (default set)

```
100% tests passed, 0 tests failed out of 66
Total Test time (real) =  60.46 sec
```
66/66, as expected (65 baseline + `test_synthetic_fits`). The `[.integration]`-tagged cases in both integration binaries are excluded from this default run (hidden tag + `--allow-running-no-tests`), so their known failures below do **not** affect this result.

### `test_phase_a_router "[integration]"` — verbatim

```
Filters: [integration]
Randomness seeded to: 1069178964
===============================================================================
All tests passed (21 assertions in 5 test cases)
```
All 5 cases pass, matching the brief's expectation exactly.

### `test_phase_b_qsolve "[integration]"` — verbatim

```
Filters: [integration]
Randomness seeded to: 969462432

-------------------------------------------------------------------------------
Phase B Q-solve: synthetic HaO3 frame recovers Ha + OIII slots
-------------------------------------------------------------------------------
/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:32
...............................................................................

/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:56: FAILED:
  REQUIRE( ha_val == Catch::Approx(0.5f).margin(0.02f) )
with expansion:
  0.0f == Approx( 0.5 )

-------------------------------------------------------------------------------
Phase B Q-solve: HaO3 + S2O3 mixed → multi-source OIII merge
-------------------------------------------------------------------------------
/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:87
...............................................................................

/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:112: FAILED:
  REQUIRE( result.derived.slots.at("OIII")[8 * 16 + 8] > 0.0f )
with expansion:
  0.0f > 0.0f

-------------------------------------------------------------------------------
Phase B Q-solve: negative emission clamped, counter incremented
-------------------------------------------------------------------------------
/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:115
...............................................................................

/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:140: SKIPPED:
explicitly with message:
  no synthetic writer for an engineered-negative-emission frame yet

-------------------------------------------------------------------------------
Phase B Q-solve: unknown INSTRUME falls back to generic_sony_imx_osc with a
                 warning
-------------------------------------------------------------------------------
/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:144
...............................................................................

/home/scarter4work/projects/nukex5/test/integration/test_phase_b_qsolve.cpp:158: FAILED:
  REQUIRE( result.derived.slots.at("Ha")[8 * 16 + 8] == Catch::Approx(0.5f).margin(0.02f) )
with expansion:
  0.0f == Approx( 0.5 )

================================================================================
test cases:  5 |  1 passed | 3 failed | 1 skipped
assertions: 21 | 18 passed | 3 failed
```

3 of the 4 real (non-skipped) cases fail. In every failing case, the derived `Ha` value comes back as exactly `0.0f` — not a value close to the target with drift, but the type's default.

## Root cause investigation (per the brief's explicit instruction, before touching any tolerance)

The writer's own unit test (`test_synthetic_fits`, case 3: "q-solved HaO3 frame's photosites invert back to the targets") independently reads the photosites straight out of the FITS file and Q-solves them via `ChannelDecomposer::solve` directly — bypassing `StackingEngine` entirely — and recovers `Ha=0.5`, `OIII=0.3` to `1e-6`. That proves the **writer** is correct: the photosite math and the FITS headers are right. The defect is downstream, inside the engine, exactly as the brief anticipated.

Tracing `stacking_engine.cpp`'s Phase B follow-up (`src/lib/stacker/src/stacking_engine.cpp:898-972`): the Q-solve reads `stacked.at(x,y,ri/gi/bi)` — the per-voxel **selected/fitted** pixel value, not the raw welford mean. That value comes from `GPUCPUFallback::select_pixels` (`src/lib/gpu/src/gpu_cpu_fallback.cpp:198`), which reads `buf.dist_true_signal[...]`, populated earlier by `ModelSelector::select` → `select_best` (`src/lib/fitting/src/model_selector.cpp:88-101`).

`ModelSelector::select_best` (`src/lib/fitting/src/model_selector.cpp:18-24`): when the sample count `n` is below `config_.min_samples_for_fit` (default 10), it falls through to `KDEFitter::fit`. `KDEFitter::fit` (`src/lib/fitting/src/kde_fitter.cpp:128-131`):

```cpp
if (n < 3) {
    result.converged = false;
    return result;
}
```

For `n < 3` this returns a **default-constructed** `FitResult` — `distribution.true_signal_estimate` defaults to `0.0f` (`src/lib/core/include/nukex/core/distribution.hpp:73`) — without ever computing anything from the actual sample. `ModelSelector::select` (`model_selector.cpp:88-101`) then does:

```cpp
voxel.distribution[channel] = best.distribution;   // unconditional — copies the zeroed distribution
if (!best.converged) {
    voxel.set_flag(VoxelFlags::FIT_FAILED);         // flag is set, but nothing downstream checks it
}
```

So any voxel/channel fed by **1 or 2 contributing frames** gets a hard-zeroed `stacked` pixel value, silently (no error, no non-zero fallback — `FIT_FAILED` is set but `select_pixels` never inspects it).

**Confirmed empirically**, not just by reading code (per the "evidence before theory" rule): I compiled a standalone probe against the built `libnukex4_fitting.a`, calling `ModelSelector::select_best` directly:

```
n=1: converged=0 true_signal=0.000000
n=2: converged=0 true_signal=0.000000
n=3: converged=1 true_signal=0.500000   (input data: {0.5, 0.52, 0.48})
```

This is the exact mechanism. My failing integration cases each build their Q-solve group from a single synthetic frame per filter (1 HaO3 frame, or 1 HaO3 + 1 S2O3 frame) — every dual-NB voxel/channel therefore has `n=1` contributing frame, hits the `n < 3` guard, and every derived `Ha`/`OIII` value collapses to `0.0f`.

I also checked: no existing test anywhere in the tree exercises `ModelSelector`/`StackingEngine`'s fitted-output path with `n < 3` samples. The existing KDE-fallback unit test (`test/unit/fitting/test_model_selector.cpp:103-116`, "very few samples → KDE fallback") uses `n=5` and asserts `converged == true`. The existing full-engine integration tests (`test_stacking_engine.cpp`, M16 data) all require `>= 3` frames. This is a genuine, previously-uncovered edge case in `nukex4_fitting`, not something specific to Q-solve or DUAL_NB_OSC routing — it would zero out **any** single/double-frame stack's fitted output, broadband included. The three phase_b failures just happen to be the first tests in the tree that check a Q-solve **value** (rather than only slot presence) on a batch this small.

**This bug lives in `src/lib/fitting/src/kde_fitter.cpp` (and/or `model_selector.cpp`'s handling of a non-converged low-n result) — files entirely outside Task 20's scope** (`test/util/*`, `test/CMakeLists.txt`, `test/integration/*.cpp`). Per the brief's explicit instruction, I did not touch any tolerance, did not skip, and did not hide these cases — they are committed as genuinely-failing (only surfaced when a test binary is run with the `[integration]` tag; the default `ctest` set stays green because these cases were already hidden-tagged before this task).

**Recommendation for the fix** (not implemented here, out of scope): `KDEFitter::fit` should handle `n ∈ {1, 2}` the way `biweight_location` already does for `n == 1` — return a converged result whose `true_signal_estimate` is the sample itself (n=1) or their mean/robust-location (n=2), rather than silently zeroing. Whatever the chosen policy, `ModelSelector::select` should probably not unconditionally copy a non-converged `FitResult`'s zeroed distribution over what would otherwise be a reasonable value.

## Files changed

- `test/util/synthetic_fits.hpp` (new)
- `test/util/synthetic_fits.cpp` (new)
- `test/unit/io/test_synthetic_fits.cpp` (new)
- `test/CMakeLists.txt` (modified — `test_util` sources/links, new test registration)
- `test/integration/test_phase_a_router.cpp` (modified — fully un-gated)
- `test/integration/test_phase_b_qsolve.cpp` (modified — 3/4 un-gated, 1 still SKIP, +1 new case)

Commit: `04cdde3` — `test(util): synthetic FITS writer; un-gate the Phase A/B integration tests`

## Self-review

- **Completeness**: `WIRED_BY_TASK_20` macro is fully removed from both integration files (grepped, confirmed absent). One `SKIP(` remains, by deliberate documented choice (see deviation section above), for a case whose required writer function is outside this task's interface. The generic-camera case is present and correctly appended. The writer's unit test has all 4 cases from the brief, verbatim.
- **Quality**: writer implementation matches the brief's code verbatim (headers, cpp, CMake block).
- **Discipline**: I did not add the missing `write_synthetic_negative_emission_hao3` function (would be scope creep beyond the brief's stated interface). I did not touch any tolerance or fitting-library code to force the 3 failing cases green.
- **Testing**: TDD RED/GREEN captured above for the writer. Both integration binaries' `[integration]` output captured verbatim above. Full ctest output is pristine (66/66, no warnings in the grep'd build output).

## Issues / concerns (BLOCKED)

3 of the 9 real Phase B/A Q-solve-touching cases across both files pass; specifically in `test_phase_b_qsolve`, 3 of 4 real cases fail:
- "Phase B Q-solve: synthetic HaO3 frame recovers Ha + OIII slots"
- "Phase B Q-solve: HaO3 + S2O3 mixed → multi-source OIII merge"
- "Phase B Q-solve: unknown INSTRUME falls back to generic_sony_imx_osc with a warning"

Root cause is confirmed (empirically, not just by inspection) to be `KDEFitter::fit`'s `n < 3` guard in `src/lib/fitting/src/kde_fitter.cpp:128`, returning a default (zeroed) `FitResult` for any voxel/channel fed by fewer than 3 contributing frames. All three failing integration cases build their dual-NB group from exactly 1 frame per filter. This is outside Task 20's file scope and needs its own fix task in `nukex4_fitting`. I recommend either fixing `KDEFitter::fit` for low `n`, or — if the team prefers the integration tests to use ≥3 frames per filter group instead — regenerating those cases with multiple synthetic frames (a test-side workaround that still requires deciding whether ≥3-frame Q-solve integration coverage is an acceptable substitute for exercising the true single/few-frame path, which real users will absolutely hit).

---

## Fix report: un-gating the negative-emission case (resumed after commit `700d3a0`)

### What changed

The engine defect from the BLOCKED section above was fixed at the root in `700d3a0` (`fix(fitting): n < 3 keeps the sample's robust location instead of zeroing the voxel`), outside this task, by the team lead's direction. Resuming Task 20 to finish the last gated case.

Per the team lead's instruction, the last case ("Phase B Q-solve: negative emission clamped, counter incremented") was un-gated **without** adding a new writer function. The `#if 0 … #else SKIP(...) #endif` scaffolding was replaced with a real body that reuses the existing `write_synthetic_q_solved_hao3` writer, calling it with `ha=0.8f, oiii=-0.04f` against `"ASI585MC"`. With the fixture's ASI585MC QE, this keeps every photosite non-negative (B ≈ 0.004, G ≈ 0.222, R ≈ 0.583 — a physically writable Bayer mosaic) while the Q-solve recovers `OIII = -0.04`, which the engine clamps to 0 and counts in `negative_clamped_count`. The case's existing assertions (`result.ok`, `negative_clamped_count > 0`, all `Ha`/`OIII` values `>= 0`) were kept unchanged. The "Still gated…" comment was replaced with two lines explaining the engineered-negative trick and the photosite-positivity bound.

Only `test/integration/test_phase_b_qsolve.cpp` was touched — nothing outside `test/integration/`.

### Verification

**Build** — clean, no errors:
```
cd build && cmake .. && make test_phase_a_router test_phase_b_qsolve
```

**`test_phase_b_qsolve "[integration]"`** — verbatim:
```
Filters: [integration]
Randomness seeded to: 1592389811
===============================================================================
All tests passed (537 assertions in 5 test cases)
```
5 passed / 0 skipped / 0 failed, as required.

**`test_phase_a_router "[integration]"`** — verbatim:
```
Filters: [integration]
Randomness seeded to: 1812953199
===============================================================================
All tests passed (21 assertions in 5 test cases)
```
5/5.

**Full `ctest`**:
```
100% tests passed, 0 tests failed out of 66
Total Test time (real) =  57.96 sec
```

**Gating scaffolding check**:
```
grep -n "SKIP(\|WIRED_BY_TASK_20\|#if 0" test/integration/*.cpp
```
returns nothing (exit code 1 / no matches) — all 9 integration cases across both files now run for real, all pass.

### Files changed

- `test/integration/test_phase_b_qsolve.cpp` (only file touched in this fix)

Commit: `1a633d3` — `test(integration): un-gate the negative-emission clamp case via an engineered negative OIII`

### Status

All of Task 20's original scope plus this follow-up is now complete: writer implemented and unit-tested (TDD RED/GREEN), all 9 integration cases across both binaries un-gated and passing, full default ctest 66/66, no gating scaffolding (`SKIP(`, `WIRED_BY_TASK_20`, `#if 0`) remains in `test/integration/`.
