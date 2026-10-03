# Task 1 Report: Star detection reads green, not channel 0

## What I implemented

Exactly the brief's Steps 1-7:

1. `nukex::default_reference_channel(int n_channels)` — a free `inline` function in
   `star_detector.hpp`, declared before `class StarDetector`. Returns `1` (green) for
   `n_channels >= 3`, `0` otherwise.
2. `StarDetector::Config::channel` — `int`, default `-1` (auto), added after
   `saturation_reject_fraction`, with the doc comment given verbatim in the brief.
3. The four private helpers (`compute_background_noise`, `find_local_maxima`,
   `refine_centroid`, `compute_flux`) now take an explicit `int ch` parameter (last
   parameter for `compute_flux`, ahead of the defaulted `aperture_radius`, matching
   the brief's given signatures) and use it in place of every literal `0` channel
   index inside them.
4. `StarDetector::detect()` resolves `ch` once, immediately after the empty/size
   guard, using the exact expression given in the brief, and threads it through all
   four call sites plus the inline `image.at(px, py, ch)` in the FWHM second-moment
   block.
5. `StarDetector::saturation_fraction()` is untouched in behavior — still reads
   channel 0 — with the required comment added explaining why it stays on 0.
6. Updated two doc comments that the change made stale/wrong (not explicitly in the
   brief's text, but they described the old channel-0-only behavior and would have
   been actively misleading left as-is): the class-level comment on `StarDetector`
   ("Input should be a single-channel (luminance) image...") and the one-line
   comment above `detect()` ("If image is multi-channel, only channel 0 is used").
   Both now describe the actual (colour-aware) behavior. No production logic changed
   beyond what the brief specified.

## What I tested and the results

### TDD evidence

**RED** — appended the test block from the brief to
`test/unit/alignment/test_star_detector.cpp`, then:

```
cmake --build build -j$(nproc) --target test_star_detector 2>&1 | tail -40
```

Failed to compile, as expected:

```
test/unit/alignment/test_star_detector.cpp:247:20: error: 'default_reference_channel' is not a member of 'nukex'
...
test/unit/alignment/test_star_detector.cpp:248:20: error: 'default_reference_channel' is not a member of 'nukex'
...
test/unit/alignment/test_star_detector.cpp:280:9: error: 'struct nukex::StarDetector::Config' has no member named 'channel'
```

This is the correct first failure per the brief: the free function doesn't exist yet
and `Config` has no `channel` member. `<cmath>` was already included in the test
file, so no include change was needed there.

**GREEN** — after implementing Steps 3-4:

```
cmake --build build -j$(nproc) --target test_star_detector && ./build/test/test_star_detector
```

```
Randomness seeded to: 3823092383
===============================================================================
All tests passed (1038 assertions in 14 test cases)
```

Both new cases pass, including the assertion that detecting on green finds 3 stars
while detecting on channel 0 (via the override) finds 1 — the actual behavioral
change under test, not just a signature check.

### Full suite

```
ctest --test-dir build --output-on-failure
```

```
100% tests passed, 0 tests failed out of 72
Total Test time (real) =  61.73 sec
```

Ran the alignment-specific subset again by name to confirm directly (all were
already included in the full 72):

```
ctest --test-dir build -R "frame_aligner|homography|star_detector|alignment_diag" --output-on-failure
```

```
100% tests passed, 0 tests failed out of 4
```

No existing test broke. `test_frame_aligner` and `test_homography` — the ones the
brief flagged as using synthetic mono images — were unaffected, so no fixture
changes were needed anywhere in the suite.

## Files changed

- `src/lib/alignment/include/nukex/alignment/star_detector.hpp`
- `src/lib/alignment/src/star_detector.cpp`
- `test/unit/alignment/test_star_detector.cpp`

## Self-review findings

- Diff matches the brief's given code verbatim for the test, the `Config::channel`
  field + doc comment, the free function, the four helper signatures, the `ch`
  resolution line in `detect()`, the `saturation_fraction` comment, and the commit
  message.
- Confirmed via `grep -n "\.at(.*, 0)"` that the only remaining literal `0` channel
  read in the file is the one inside `saturation_fraction` — every other read now
  goes through `ch`.
- Naming (`ch`) matches the brief exactly rather than something I'd have picked
  independently (e.g. `channel`), for consistency with the brief's stated signatures
  and with Task 5's expected reuse of `default_reference_channel`.
- No stubs, no TODOs, no dead code. No new third-party dependencies. No non-ASCII
  characters introduced (checked visually; all new comments/strings are plain
  ASCII).
- The two doc-comment edits beyond the brief's literal text are the only scope
  addition. They're corrections of now-false statements the diff itself invalidated,
  not new behavior, and I've called them out explicitly above rather than folding
  them in silently.
- The new multi-channel test is a real behavioral check (red-only blob vs.
  green-only blobs, distinguished by count), not a vacuous assertion — it would
  fail if `ch` were wired to the wrong helper argument or if the auto-selection
  logic picked channel 0 or 2 instead of 1.

## Concerns

None. Task is complete and self-contained; Tasks 2 and 5 can call
`nukex::default_reference_channel` directly as planned.
