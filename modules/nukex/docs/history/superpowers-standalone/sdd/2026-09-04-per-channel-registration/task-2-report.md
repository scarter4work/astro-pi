# Task 2 Report: Measure the per-channel transform

## What was implemented

Per the brief, verbatim:

- `src/lib/alignment/include/nukex/alignment/channel_registration.hpp` -- new header defining
  `ChannelTransform` (s, tx, ty, n_stars, residual, Fit enum, `is_identity()`, `max_displacement()`,
  `apply()`), `ChannelTransforms` (per_channel vector, cx/cy, reference_channel, `empty()`,
  `negligible()`), `ChannelRegistrationConfig`, and the two `measure_channel_transforms` overloads
  (4-arg and 3-arg-defaulting-config).
- `src/lib/alignment/src/channel_registration.cpp` -- the implementation: `median_of`,
  `centroid_at` (median-of-border-ring background, N-pass refinement, SNR gate), `Pair`,
  `fit_affine` (closed-form shared-scale least squares), `fit_translation` (present now per the
  brief's code block, unused until Task 3 wires it into the ladder), `residuals_of`, and
  `fit_channel` (Task 2's happy-path-only version: affine fit gated on `min_stars_affine`,
  returns default-identity below that). `measure_channel_transforms` builds the isolation mask,
  centroids every isolated star on the reference channel, then for each non-reference channel
  centroids the same stars and calls `fit_channel`.
- `src/lib/alignment/CMakeLists.txt` -- added `src/channel_registration.cpp` to
  `nukex4_alignment`'s sources, after `reference_selector.cpp`.
- `test/unit/alignment/test_channel_registration.cpp` -- the five test cases from the brief,
  transcribed verbatim.
- `test/CMakeLists.txt` -- registered `test_channel_registration`, immediately after
  `test_reference_selector`.

Both ordering hazards called out in the dispatch were honored:
- `fit_channel` (line 170 of the .cpp) is defined in the anonymous namespace above
  `measure_channel_transforms` (line 182), and is Task 2's simple affine-only version, not
  Task 3's ladder.
- `median_of` (line 9) is defined above `centroid_at` (line 44), which calls it twice (background,
  then MAD-based noise estimate).

The median-vs-minimum background choice in `centroid_at`'s doc comment, with its measured numbers
(0.217 px min vs 0.057 px median on the 1/5-flux case), was kept exactly as given -- not
simplified, not touched.

## What was tested and the results

Five Catch2 `TEST_CASE`s in `test_channel_registration.cpp`, exactly as specified in the brief:

1. `measure_channel_transforms recovers a known scale and shift` -- 800x800 synthetic 3-channel
   frame, red displaced by (s=1.0005, tx=0.33, ty=-0.21) about the frame centre, 25-star grid.
   Asserts `Fit::Affine`, `n_stars >= 20`, and positional accuracy at the field corner within
   0.02 px.
2. `the reference channel is exactly identity` -- green's transform is `Fit::Identity`,
   `is_identity()` true, s/tx/ty exactly 1.0/0.0/0.0.
3. `a channel that already agrees measures as near-identity` -- blue, drawn at the same positions
   as green, measures small (not exactly zero) displacement.
4. `a single-channel image registers nothing` -- mono image returns `ct.empty()`.
5. `negligible()` -- pure unit-level test of the radius-dependent threshold logic, no image
   involved.

All five pass; 21 assertions, 0 failures. On this noiseless synthetic field the recovered errors
are far inside the 0.02 px budget (e.g. corner scale error 0.0000225 px, tx error 0.0000839 px,
residual 0.0000467 px) -- no tolerance was loosened to get there.

## TDD evidence

RED:

```
$ cmake --build build -j$(nproc) --target test_channel_registration 2>&1 | tail -20
...
/home/scarter4work/projects/nukex5/test/unit/alignment/test_channel_registration.cpp:2:10: fatal error: nukex/alignment/channel_registration.hpp: No such file or directory
    2 | #include "nukex/alignment/channel_registration.hpp"
      |          ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
compilation terminated.
```

Expected: the test file was written and registered in `test/CMakeLists.txt` before the header or
implementation existed, so the only possible failure at this point is a missing header. Confirmed.

GREEN:

```
$ cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
...
[100%] Built target test_channel_registration
===============================================================================
All tests passed (21 assertions in 5 test cases)
```

(One warning surfaced during the library build: `fit_translation` defined but not used. Expected
and harmless -- it's part of the brief's verbatim code, present now because Task 3 will wire it
into the fallback ladder in place of the current `fit_channel`. No `-Werror` anywhere in the
build, confirmed by grep, so it does not fail the build.)

Full suite, run once after the focused test was green:

```
$ ctest --test-dir build --output-on-failure
...
100% tests passed, 0 tests failed out of 73
Total Test time (real) =  61.43 sec
```

`test_channel_registration` is test #21 in that run; no other test regressed.

## Files changed

- `src/lib/alignment/include/nukex/alignment/channel_registration.hpp` (new)
- `src/lib/alignment/src/channel_registration.cpp` (new)
- `src/lib/alignment/CMakeLists.txt` (added one source line)
- `test/unit/alignment/test_channel_registration.cpp` (new)
- `test/CMakeLists.txt` (added one registration line)

Commit: `44645ef` -- "feat(alignment): measure per-channel scale and shift"

## Self-review findings

- Both ordering hazards verified by grepping line numbers in the final file (see above) rather
  than trusting the paste order -- confirmed correct.
- ASCII-only check (`grep -nP '[^\x00-\x7F]'` over all three new/touched source files) found
  nothing -- no mojibake risk from the em dashes used throughout the doc comments and brief text
  (they were written as plain `--` in code, matching the brief's own convention, not a Unicode
  em dash).
- Interfaces consumed (`default_reference_channel`, `StarDetector::Config::channel`, `Image`,
  `StarCatalog`, `Star`) were checked against Task 1's actual header content before writing
  anything, not assumed from the brief's prose -- all matched exactly.
- CMakeLists edits are single-line, minimal, and placed exactly where the brief specified
  (`src/channel_registration.cpp` after `reference_selector.cpp`; test registration immediately
  after `test_reference_selector`).
- No YAGNI additions -- every function in the .cpp is either used now (`median_of`, `centroid_at`,
  `Pair`, `fit_affine`, `residuals_of`, `fit_channel`, `measure_channel_transforms`) or is
  deliberately staged for Task 3 per the brief's own instructions (`fit_translation`). Nothing was
  added beyond what the brief specified.
- Tests exercise real behavior, not the mock/stub kind: they render actual Gaussian PSFs into a
  synthetic multi-channel `Image`, run the real centroiding and fitting code path, and assert on
  the numeric result, including a corner-case positional bound that ties directly to the acceptance
  criterion.

## Concerns

None. No tolerance was loosened; no numeric comment was altered; both ordering hazards were
respected; the median-background design point was left untouched as instructed. The one build
warning (`fit_translation` unused) is expected and resolves itself when Task 3 lands.
