# Task 20b Report: Fitting — data-derived estimate for n < 3

## What I implemented

`KDEFitter::fit` (`src/lib/fitting/src/kde_fitter.cpp`) previously returned a
default-constructed `FitResult` for `n < 3`, which zeroed every field
including `distribution.true_signal_estimate`. `ModelSelector::select` copied
that zeroed distribution into the voxel and only set `FIT_FAILED`, so any
channel fed by fewer than three frames stacked to exactly 0.0.

The `n < 3` branch now returns:
- `converged = false` (unchanged — this is still not a real fit)
- `distribution.shape = DistributionShape::UNKNOWN`
- `distribution.used_nonparametric = true`
- `distribution.true_signal_estimate = robust_location` (the sample itself
  for n=1, the biweight location of the pair for n=2 — this is exactly what
  the caller already passed in as `robust_location`)
- `distribution.kde_mode = robust_location`
- `distribution.kde_bandwidth = 0.0f`
- `distribution.signal_uncertainty = robust_scale`
- `distribution.confidence = 0.0f`

Added a doc comment above `KDEFitter::fit` in
`src/lib/fitting/include/nukex/fitting/kde_fitter.hpp` describing this
contract.

## What I tested and results

Three new KDE unit tests appended to `test/unit/fitting/test_kde_fitter.cpp`:
- n=1 returns the sample as the estimate, not zero
- n=2 estimate is the robust location of the pair
- n=3 still runs the real KDE (regression guard — this case passed
  before and after)

One new selector unit test appended to
`test/unit/fitting/test_model_selector.cpp`, built the same way as the
file's existing "writes robust stats to voxel" case (`SubcubeVoxel voxel{};`,
`selector.select(...)`, `voxel.distribution[0]`, `voxel.has_flag(...)`):
- a single sample sets `FIT_FAILED` but keeps the sample as the estimate

### TDD evidence — RED

```
=== test_kde ===
-------------------------------------------------------------------------------
KDEFitter: n=1 returns the sample as the estimate, not zero
-------------------------------------------------------------------------------
.../test_kde_fitter.cpp:81: FAILED:
  REQUIRE( r.distribution.true_signal_estimate == Catch::Approx(0.37f) )
with expansion:
  0.0f == Approx( 0.37000000476837158 )

-------------------------------------------------------------------------------
KDEFitter: n=2 estimate is the robust location of the pair
-------------------------------------------------------------------------------
.../test_kde_fitter.cpp:98: FAILED:
  REQUIRE( r.distribution.true_signal_estimate == Catch::Approx(rl) )
with expansion:
  0.0f == Approx( 0.32000002264976501 )

test cases:  7 |  5 passed | 2 failed
assertions: 19 | 17 passed | 2 failed

=== test_model_selector ===
-------------------------------------------------------------------------------
ModelSelector::select: a single sample sets FIT_FAILED but keeps the sample as
the estimate
-------------------------------------------------------------------------------
.../test_model_selector.cpp:126: FAILED:
  REQUIRE( voxel.distribution[0].true_signal_estimate == Catch::Approx(0.42f) )
with expansion:
  0.0f == Approx( 0.41999998688697815 )

test cases:  6 |  5 passed | 1 failed
assertions: 16 | 15 passed | 1 failed
```

The n=3 KDE case passed already in this RED run, exactly as the brief
predicted.

### TDD evidence — GREEN (after implementing Step 3)

```
=== test_kde ===
Randomness seeded to: 486388006
===============================================================================
All tests passed (27 assertions in 7 test cases)

=== test_model_selector ===
Randomness seeded to: 3425566089
===============================================================================
All tests passed (17 assertions in 6 test cases)
```

### Full verification (Step 5)

```
$ ctest
100% tests passed, 0 tests failed out of 66
Total Test time (real) =  57.92 sec
```

```
=== phase_a_router [integration] ===
Filters: [integration]
Randomness seeded to: 2231860607
===============================================================================
All tests passed (21 assertions in 5 test cases)
```

```
=== phase_b_qsolve [integration] ===
...............................................................................

.../test_phase_b_qsolve.cpp:140: SKIPPED:
explicitly with message:
  no synthetic writer for an engineered-negative-emission frame yet

================================================================================
test cases:  5 |  4 passed | 1 skipped
assertions: 23 | 23 passed
```

All match the brief's expected outputs exactly (66/66; phase_a all 5 cases
passed; phase_b 4 passed / 1 skipped / 0 failed, the skip being the
still-gated negative-emission case owned by Task 20).

## GPU parity check

```
$ grep -rn "< 3\|<3\|n_samples\b" src/lib/gpu/src/*.cpp src/lib/gpu/kernels/* 2>/dev/null | head
(no output)
```

No small-sample guard exists anywhere in the GPU sources or kernels. I
confirmed why: the GPU path never runs its own distribution fit at all.
`src/lib/gpu/kernels/select_pixels.cl` reads `dist_true_signal` (a field
whose comment in the kernel reads "Reads the fitted distribution's
true_signal_estimate (from CPU fitting)") out of the shadow buffers and only
performs pixel *selection* over already-CPU-computed values.
`gpu_shadow_buffers.hpp` documents the same field as "Distribution input
(host → device, after CPU fitting)". `gpu_executor.cpp:445` has a comment
noting the parametric/KDE fit families are handled as CPU stack locals ahead
of GPU dispatch. There is no GPU-side re-implementation of KDE or any other
fitter to patch — the CPU fix in `kde_fitter.cpp` is upstream of the
shadow-buffer transfer, so the corrected `true_signal_estimate` for n<3
voxels flows into the GPU selection kernel unchanged, with no separate fix
needed there.

`ctest -R gpu_agreement` passes:

```
Test #60: test_gpu_agreement ...............   Passed    0.93 sec
100% tests passed, 0 tests failed out of 1
```

No GPU files were touched.

## Files changed

- `src/lib/fitting/src/kde_fitter.cpp` — n<3 branch now returns
  `robust_location`/`robust_scale` instead of a zeroed default.
- `src/lib/fitting/include/nukex/fitting/kde_fitter.hpp` — doc comment on
  `fit()` describing the n<3 contract.
- `test/unit/fitting/test_kde_fitter.cpp` — 3 new test cases (n=1, n=2, n=3
  regression guard), plus `#include "nukex/core/distribution.hpp"`.
- `test/unit/fitting/test_model_selector.cpp` — 1 new test case (single
  sample sets FIT_FAILED, keeps sample as estimate).

No changes outside `src/lib/fitting` and `test/unit/fitting`. No CMake
changes (both test binaries were already registered). No GPU changes (parity
check found no small-n guard to mirror; see above).

## Self-review

- **Completeness**: three KDE cases present (n=1, n=2, n=3-regression) and
  one selector case present, matching the brief verbatim. Header doc comment
  added verbatim. GPU check performed and reported explicitly with grep
  output and reasoning.
- **Quality**: implementation matches the brief's Step 3 code exactly,
  including the inline comment explaining why converged stays false. No
  dead code, no stubs.
- **Discipline**: `git diff --stat` after the commit shows only the four
  files listed above touched; nothing under `test/integration/` was touched
  (Task 20 still owns those, including the currently-skipped negative-
  emission case, which was left exactly as found).
- **Testing**: RED captured before the fix (2 KDE failures + 1 selector
  failure, all on `true_signal_estimate == 0`, n=3 already green, matching
  the brief's predicted failure mode exactly); GREEN captured after. Full
  ctest run 66/66. Both opt-in integration binaries run and match expected
  counts verbatim.

## Issues or concerns

None. The fix was a narrow, well-scoped root-cause change exactly matching
the brief's contract, and the GPU path required no mirrored change because
it consumes CPU-fitted distributions rather than computing its own.
