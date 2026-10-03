# Task 4 Report: Compose the transform into the warp

## What I implemented

Added a five-argument overload of `HomographyComputer::warp` that folds the
per-channel colour-registration correction into the existing back-mapping
resample, at zero extra cost:

- `src/lib/alignment/include/nukex/alignment/homography.hpp`: added
  `#include "nukex/alignment/channel_registration.hpp"` and declared
  `static Image warp(const Image& source, const HomographyMatrix& H, int output_width, int output_height, const ChannelTransforms& channels)`
  with the doc comment from the brief, verbatim.
- `src/lib/alignment/src/homography.cpp`: the old four-argument `warp` body
  was renamed onto the new five-argument signature; the old signature now
  delegates with `ChannelTransforms{}`. Inside the channel loop, the
  per-channel decision (`has_ct`, `ct`, `apply_ct`) is computed once per
  channel, hoisted above the `y`/`x` pixel loops. Inside the pixel body,
  `if (apply_ct) ct.apply(channels.cx, channels.cy, sx, sy);` is inserted
  immediately after `sx`/`sy` are computed from `H_inv`, and before the
  `std::isfinite` / bounds check.
- `test/unit/alignment/test_homography.cpp`: appended the two test cases
  from the brief verbatim.

## Where the channel transform sits relative to the bounds check

Confirmed placed **after** computing `sx`, `sy` from `H_inv` and **before**
both the `std::isfinite` check and the `sx < 0 || sx >= sw - 1 || ...` bounds
check (`src/lib/alignment/src/homography.cpp`, in the five-argument `warp`).
This means a coordinate that is inside the source frame before the channel
shift but outside it after gets correctly rejected by the bounds check
instead of read out of bounds.

## TDD evidence

**RED** — `cmake --build build -j$(nproc) --target test_homography 2>&1 | tail -20`, run before any implementation edits (only the test file had been appended):

```
error: no matching function for call to 'nukex::HomographyComputer::warp(nukex::Image&, nukex::HomographyMatrix, int, int, nukex::ChannelTransforms&)'
    • candidate 1: 'static nukex::Image nukex::HomographyComputer::warp(const nukex::Image&, const nukex::HomographyMatrix&, int, int)'
      candidate expects 4 arguments, 5 provided
```

(Same error repeated for the second new test case.) This is exactly the
expected failure: the five-argument overload does not exist yet, so both new
`TEST_CASE`s fail to compile.

**GREEN** — after Steps 3-4 (header + cpp edits):

```
$ cmake --build build -j$(nproc) --target test_homography && ./build/test/test_homography
...
[100%] Built target test_homography
Randomness seeded to: 4161119785
===============================================================================
All tests passed (52661 assertions in 9 test cases)
```

Re-ran with a touched source (fresh compile, different random seed) to check
for build-cache flukes and compiler warnings:

```
$ touch test/unit/alignment/test_homography.cpp
$ cmake --build build -j$(nproc) --target test_homography 2>&1 | grep -i "warning\|error"
no warnings/errors
$ ./build/test/test_homography
Randomness seeded to: 3362826852
All tests passed (52661 assertions in 9 test cases)
```

**Full suite** — `ctest --test-dir build --output-on-failure`:

```
100% tests passed, 0 tests failed out of 73
Total Test time (real) =  61.43 sec
```

## What the two new tests actually verify

1. `warp with channel transforms brings a displaced channel into register`:
   builds a 200x200 3-channel synthetic image with a green blob at (100,100)
   and a red blob at (101.5,100) -- 1.5 px displaced. A `ChannelTransforms`
   with `tx=1.5` on channel 0 (red) is applied via the new overload. Before:
   the two channels' centroids differ by >1.4 px. After: they agree to
   <0.05 px. The reference channel (green, channel 1) is required to be
   bit-identical (`Catch::Approx`, effectively exact since it's a straight
   float compare of untouched values) to the un-warped source everywhere in
   the frame, since `HomographyMatrix::identity()` plus an untouched
   reference channel means no resampling occurs for it at all.
2. `warp with empty channel transforms matches the old warp exactly`: calls
   the four-argument `warp` and the five-argument `warp` with
   `ChannelTransforms{}` on the same non-trivial `H` (translation-only, not
   identity) and 64x64x3 deterministic pseudo-random data, and requires
   every one of the `data_size()` floats to compare exactly equal
   (`==`, not `Approx`). This is the delegation path and it is bit-identical
   as required -- unsurprising since the four-argument overload now *is* a
   one-line call into the five-argument one with an empty struct, so there
   is no room for the two to diverge on any input.

## Files changed

- `src/lib/alignment/include/nukex/alignment/homography.hpp`
- `src/lib/alignment/src/homography.cpp`
- `test/unit/alignment/test_homography.cpp`

Commit: `6a08f01` "feat(alignment): channel-aware warp overload"

## Self-review

- **Completeness against brief**: header include, doc comment, and
  declaration match the brief verbatim. The cpp delegation, the hoisted
  `has_ct`/`ct`/`apply_ct` triple, and the `ct.apply(...)` call site all
  match the brief's code blocks verbatim, in the exact position specified.
- **Naming**: `has_ct`, `ct`, `apply_ct` match the brief and read fine next
  to the existing short names in this file (`sx`, `sy`, `ch`, `He`, `H_inv`).
- **YAGNI**: no scope beyond the brief. I did not touch
  `correct_meridian_flip`, `dlt_4point`/`dlt_npoint`, or anything else in the
  file.
- **Do the tests verify real behaviour?** Yes: test 1 exercises the actual
  numerical correction (centroid recovery to <0.05 px from a >1.4 px offset)
  and separately pins that the reference channel is untouched byte-for-byte;
  test 2 pins bit-for-bit equivalence of the two call paths on non-trivial
  input, so a future edit that lets the two diverge (e.g. an accidental
  extra copy, or a changed floating-point evaluation order between the two
  functions) would be caught immediately.
- **Build output pristine?** Yes -- confirmed with a full clean-target
  rebuild of `nukex4_alignment` and of `test_homography` (`touch`-forced
  recompilation of the modified `.cpp` files), grepping for
  "warning|error": none found in either. The full project build
  (`cmake --build build -j$(nproc)`, no target filter) also completed clean,
  including linking `NukeX-pxm.so`.
- **Full suite**: 73/73 ctest cases pass, no regressions.

## Concerns

None. One incidental observation, out of scope for this task and not
something I touched: `src/lib/alignment/src/homography.cpp` already
contained pre-existing non-ASCII characters (`×`, `→`) in comments at lines
79, 80, 101, and 318, predating this change (part of the DLT normalization
comments and the "H maps source→ref" comment). This violates the project's
ASCII-only-in-PCL-strings rule but is pre-existing technical debt untouched
by this diff -- flagging it here rather than silently fixing unrelated code
outside the task's scope. Worth a follow-up cleanup pass if the team wants
one.
