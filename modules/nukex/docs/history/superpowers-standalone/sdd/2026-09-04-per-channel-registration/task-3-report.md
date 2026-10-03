# Task 3 Report: The Fallback Ladder

## What was implemented

Replaced Task 2's placeholder `fit_channel` (affine-only, no guards) in
`src/lib/alignment/src/channel_registration.cpp` with the full fallback
ladder, in place, using the brief's code verbatim:

- `n < min_stars_translation` (3) -> identity immediately.
- `n >= min_stars_affine` (8) -> affine fit, one sigma-clip pass on
  residuals (1.4826*MAD estimator, `clip_sigma` = 3.0), refit on survivors
  if the clip actually dropped anyone and enough remain; result checked for
  plausibility before being returned.
- Between those thresholds -> `fit_translation`, also checked for
  plausibility.
- Any implausible result (`|s-1| > max_scale_deviation`, `|tx|`/`|ty| >
  max_translation_px`, or non-finite) collapses to identity rather than
  retrying a lower rung -- an implausible affine result does not get a
  translation-only retry, since the same bad centroids feed both.

No new symbols were produced. `fit_channel`'s signature is unchanged. The
`Fit` enum already had all three values from Task 2; this task is what
makes `TranslationOnly` and post-hoc `Identity` actually reachable.

## What was tested and the results

Appended the eight fallback-ladder test cases from the brief verbatim to
`test/unit/alignment/test_channel_registration.cpp` (translation-only
fallback, too-sparse-for-translation fallback, no-stars-at-all, implausible
scale rejected, implausible translation rejected, a star invisible in one
channel dropped from that channel only, a star with a close neighbour
excluded, an outlier centroid clipped).

### RED

Command:
```
cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
```

Result: 13 test cases, 9 passed, 4 failed. Relevant failing output:

```
too few stars for four parameters falls back to translation only
  REQUIRE( red.fit == ChannelTransform::Fit::TranslationOnly )
with expansion:
  0 == 1

an implausible scale is rejected rather than applied
  REQUIRE( red.fit == ChannelTransform::Fit::Identity )
with expansion:
  2 == 0

an implausible translation is rejected rather than applied
  REQUIRE( red.fit == ChannelTransform::Fit::Identity )
with expansion:
  2 == 0

an outlier centroid is clipped out of the fit
  CHECK( std::abs(red.s - s) * R < 0.02 )      -> 0.0554 < 0.02  FAILED
  CHECK( std::abs(red.tx - tx) < 0.02 )        -> 0.1040 < 0.02  FAILED
  CHECK( red.n_stars < int(f.catalog.stars.size()) ) -> 25 < 25  FAILED
```

(`Fit` enum order is `Identity`=0, `TranslationOnly`=1, `Affine`=2, so
"0 == 1" means the actual value was `Identity` where `TranslationOnly` was
expected, and "2 == 0" means the actual value was `Affine` where `Identity`
was expected.)

This matches exactly what the brief predicted: the translation-only case
reported `Identity` because Task 2's `fit_channel` returns identity below
8 stars with no translation rung; both implausible-fit cases reported
`Affine` with the absurd value applied because Task 2 had no plausibility
check. The outlier-clip failure is the expected consequence of there being
no sigma-clip yet -- Task 2 fits every point unconditionally. The other
four new cases (too-few-for-translation, no-stars-at-all, invisible-in-one-
channel, close-neighbour-excluded) already passed under Task 2's code,
which is expected: they don't require anything the ladder specifically
adds (identity-by-default, empty-catalog short-circuit, and the
neighbour-isolation/SNR-gate mechanics Task 2 already built).

### GREEN

Command:
```
cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
```

Result:
```
All tests passed (44 assertions in 13 test cases)
```

### Full suite

Command:
```
ctest --test-dir build --output-on-failure
```

Result:
```
100% tests passed, 0 tests failed out of 73
Total Test time (real) =  61.41 sec
```

## `fit_translation` warning

Confirmed gone. Forced a clean recompile of just
`channel_registration.cpp` (`rm` the object file, rebuild the
`nukex4_alignment` target) and grepped the compiler output for
"warning"/"unused"/the filename: no matches. `fit_translation` is now
called from the ladder's middle rung (`n >= min_stars_translation && n <
min_stars_affine`), so it's genuinely used, not suppressed.

## Files changed

- `src/lib/alignment/src/channel_registration.cpp` -- `fit_channel`
  replaced in place with the ladder (same location, above its caller,
  single definition).
- `test/unit/alignment/test_channel_registration.cpp` -- eight new test
  cases appended.

## Self-review

- Diffed both files against the brief's code blocks; both are verbatim
  matches (confirmed via `git diff`), including all comments carrying
  measured numbers and rationale (MAD-sigma factor, why no
  translation-only retry after a failed affine, why one clip pass and not
  two).
- `fit_channel` stays a single definition in the same anonymous namespace,
  above `measure_channel_transforms`, not forward-declared -- no
  duplication.
- No new dependencies, no new public symbols. `ChannelTransform::fit` now
  actually reaches all three enumerator values at runtime (Task 2 only
  reached `Identity`/`Affine`); out-of-range/implausible fits fall back to
  `Identity`, matching the interface note in the brief.
- Namespace `nukex` and library target `nukex4_alignment` untouched.
- `grep -nP '[^\x00-\x7F]'` over both changed files found zero non-ASCII
  bytes.
- Checked the plausibility gate applies uniformly to both the affine and
  translation rungs, and that a plausibility failure on affine does not
  fall through to attempt translation -- it returns identity directly, per
  the brief's explicit comment on why (same bad centroids feed both).
  Confirmed by the two implausible-fit tests: both frames have `n=25`
  stars (well above `min_stars_affine`), so an incorrect fallback to
  translation-only would still have produced a non-identity `tx`/`ty` that
  the tests would have caught as failing `is_identity()`.
- Re-read the sigma-clip branch for an edge case not covered by name in
  the brief: if the clip cutoff keeps every point (`kept.size() ==
  pairs.size()`), the code intentionally does not refit, falling through
  to use the original unclipped `a` -- avoids a redundant identical refit.
  If the clip is aggressive enough to drop below `min_stars_affine`, it
  also falls through to the original unclipped `a` rather than dropping to
  translation-only. Both are exactly what the brief's code does; I did not
  alter this behavior, just confirmed I understood why it's shaped this
  way before committing to it.
- No tolerances were loosened. All numeric assertions in the new tests are
  the brief's own figures and all passed as given.

## Concerns

None. The numeric tolerances in the seven quantitative tests all passed as
specified without needing any investigation of near-misses.
