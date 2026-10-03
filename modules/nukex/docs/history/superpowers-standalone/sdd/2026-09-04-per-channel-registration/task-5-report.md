# Task 5 Report: Wire it into FrameAligner

## Status: DONE

## What I implemented

Per the brief, in `src/lib/alignment/include/nukex/alignment/frame_aligner.hpp`:
- Added `#include "nukex/alignment/channel_registration.hpp"`.
- Added `ChannelRegistrationConfig channel_config` and `bool register_channels = true` to `Config`.
- Added `ChannelTransforms channels` to `AlignedFrame`, after `stars`.
- Corrected the stale doc comment on `align()` (Task 1 review finding): it used to say the input
  "should be single-channel" and that channel 0 is used for detection. Both are now wrong --
  colour is the normal case and detection runs on green. Replaced with: "The input image may be
  mono or colour. Star detection runs on the green channel for a colour frame (see
  default_reference_channel())."

In `src/lib/alignment/src/frame_aligner.cpp`, rewrote `align()` verbatim per the brief:
- Added `#include <cmath>` and an anonymous-namespace `kNegligibleChannelShiftPx = 0.01` constant.
- After star detection (and after the first-frame-becomes-reference fallback), measure channel
  transforms on the unwarped frame via `measure_channel_transforms`, reference channel from
  `default_reference_channel(frame.n_channels())`, gated on `register_channels`, `n_channels() >= 2`,
  and a non-empty catalog.
- Added the near-identity skip: compute `corner_radius` from the frame's own half-width/half-height,
  and clear `result.channels` back to an empty `ChannelTransforms{}` whenever
  `negligible(corner_radius, kNegligibleChannelShiftPx)` is true, so downstream code sees the plain
  no-op path.
- Reference-frame path: now warps with the identity homography and `result.channels` when
  `channels_matter`, and clones only when there is nothing to correct.
- Normal warped path: passes `result.channels` into the five-argument `HomographyComputer::warp`.
- Failed-alignment path: when `channels_matter`, warps with the identity homography at the frame's
  own dimensions instead of cloning; clones only when there is also nothing to correct.

## What I tested and results

### TDD evidence

**RED** -- appended the brief's test block to `test/unit/alignment/test_frame_aligner.cpp` first,
then built before touching the header/cpp:

```
$ cmake --build build -j$(nproc) --target test_frame_aligner 2>&1 | tail -20
...
error: 'struct nukex::FrameAligner::Config' has no member named 'register_channels'
error: 'struct nukex::FrameAligner::AlignedFrame' has no member named 'channels'
```

Expected failure, exactly as the brief predicted: the new tests reference `AlignedFrame::channels`
and `Config::register_channels`, neither of which existed yet.

**GREEN (first pass, after implementing verbatim)**:

```
$ cmake --build build -j$(nproc) --target test_frame_aligner && ./build/test/test_frame_aligner
...
test cases:     11 |     10 passed | 1 failed
assertions: 570032 | 570031 passed | 1 failed

FrameAligner registers channels on a warped frame
test_frame_aligner.cpp:245: FAILED:
  REQUIRE_FALSE( out.alignment.alignment_failed )
with expansion:
  !true
```

One of the five new tests failed at runtime (not compile). I root-caused this before touching
anything further: wrote a standalone diagnostic (`/tmp/.../scratchpad/diag.cpp`, not part of the
repo) that calls only `StarDetector::detect` + `StarMatcher::match` + `HomographyComputer::compute`
directly on the test's own `ref`/`moved` images -- no `FrameAligner`, no `measure_channel_transforms`,
nothing from Task 5 at all. It reproduced the identical failure: of 13 star-matcher correspondences,
only 6 had the correct (3,2) translation; the other 7 were matches to a different grid cell entirely
(e.g. diff=(163,-78), diff=(-317,322)). 6 correct correspondences is below
`HomographyComputer::Config::min_matches` (8), so `alignment_failed` was set regardless of anything
channel-registration-related.

Cause: the brief's `make_colour_frame` placed all 25 stars on a perfectly periodic 5x5 grid.
Triangle-similarity matching is degenerate on a lattice -- many triangles are congruent to a
triangle in a different grid cell, so the matcher pairs stars across cells. This is a property of
the test fixture, not of `StarMatcher`, `HomographyComputer`, or my `align()` rewrite (I did not
modify either of the first two, and channel measurement runs on a `const StarCatalog&` with no
side effects on the matcher's RNG, which is separately seeded at 42 in `homography.cpp` regardless).

I reported this to the controller before changing anything (per "use the test code verbatim" /
"ask if something looks wrong"), including the diagnostic evidence and a ranked list of options.
Ruling: apply option 1 (break periodicity) in the exact form specified -- move the grid origin to
50 (from 40) and pitch to `(w-100)/4`, add a small deterministic per-star sinusoidal offset
(`ox`/`oy`, amplitude 9 px) to `x`/`y` before `jitter_x`/`jitter_y`, so the two fixture position
lines in `make_colour_frame` are replaced. I applied that replacement verbatim as given (see
`test/unit/alignment/test_frame_aligner.cpp` in the `make_colour_frame` grid loop) and did not
tune or second-guess the constants.

**GREEN (final, after the fixture fix)**:

```
$ cmake --build build -j$(nproc) --target test_frame_aligner && ./build/test/test_frame_aligner
Randomness seeded to: 1967850059
===============================================================================
All tests passed (570033 assertions in 11 test cases)
```

All 11 cases pass: the 6 pre-existing `[aligner]` cases and the 5 new `[frame_aligner]` cases
(reference-frame registration, warped-frame registration, mono no-op, near-identity skip, and the
`register_channels = false` switch).

### Full suite

```
$ cmake --build build -j$(nproc)     # clean, no warnings
$ ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 73
Total Test time (real) =  62.18 sec
```

`test_frame_aligner`, `test_alignment_diag`, and `test_channel_registration` all pass; the
`golden_frozen` / integration suites (`test_lrgb_mono`, `test_phase_a_router`, `test_phase_b_qsolve`,
etc.) are unaffected.

## Confirmation: all three image-producing paths apply the correction

1. **Reference frame** (`frame_index == ref_index_`): warps with `HomographyMatrix::identity()`
   plus `result.channels` when `channels_matter`; clones only when there is nothing to correct.
   Covered by "FrameAligner registers channels on the reference frame itself" (channel offset drops
   from >1.0 px raw to <0.06 px after correction) and by "a frame whose channels already agree is
   not resampled for it" (bit-identical output when there is truly nothing to fix).
2. **Failed alignment**: warps with `HomographyMatrix::identity()` at the frame's own
   width/height plus `result.channels` when `channels_matter`; clones only when there is also
   nothing to correct. Not exercised by a dedicated new test (none of the five test the failed-match
   branch directly), but the code path is symmetric with the reference-frame path and shares the
   same `channels_matter` gate computed once, above the branch.
3. **Normal warped path**: passes `result.channels` straight into the five-argument
   `HomographyComputer::warp`. Covered by "FrameAligner registers channels on a warped frame"
   (channel offset <0.06 px after warp-plus-correction).

## Files changed

- `src/lib/alignment/include/nukex/alignment/frame_aligner.hpp`
- `src/lib/alignment/src/frame_aligner.cpp`
- `test/unit/alignment/test_frame_aligner.cpp`

Commit: `be9c546` -- "feat(alignment): register channels through FrameAligner"

## Self-review findings

- Diff matches the brief's code blocks verbatim for the header and cpp changes; no extra
  refactoring or scope creep.
- The doc-comment fix (Task 1's Minor finding) is applied and is factually accurate against
  current behaviour.
- Ran a non-ASCII grep over all three edited files. The only hits are pre-existing lines
  (an em dash and a multiplication sign in the class-level doc comment, and a box-drawing
  separator comment already in the test file) that I did not author or touch -- no new
  non-ASCII content was introduced.
- Build output is pristine: no new warnings from either the library or the test target.
- The `channels_matter` gate is computed once, before the `frame_index == ref_index_` branch,
  so both the reference-frame and failed-alignment paths reuse the identical decision the normal
  path implicitly makes by just always passing `result.channels` (empty or not) into `warp`. This
  avoids duplicating the negligibility logic three times.
- One YAGNI check: I did not add a dedicated test for the failed-alignment path warping with
  channel correction, since the brief's test list didn't include one and the branch is a direct,
  narrow mirror of the already-tested reference-frame branch (same ternary shape, same
  `channels_matter` flag, only the homography's target dimensions differ in a way unrelated to
  channel correction). Flagging this as a coverage gap rather than silently calling it fully proven.

## Concerns

- The fixture deviation is the only departure from "use the code blocks verbatim." It affects only
  the test file, was surfaced before being applied, ruled on explicitly by the controller with a
  fully specified replacement, and is documented in both this report and the commit message. I did
  not modify the interface contract (`ChannelTransforms`, `Config::register_channels`,
  `AlignedFrame::channels`) or the `align()` rewrite from the brief.
- As noted above, the failed-alignment-with-channel-correction branch has no direct test coverage.
  It's a narrow, symmetric mirror of the already-tested reference-frame branch, but if the
  controller wants it covered explicitly, that would be a small follow-up test rather than a code
  change.

---

# Fix Round 1

## Status: DONE

Two Important findings from the Task 5 review, both ruled on by the controller before I touched
code.

## Finding 1: warp() silently zeroed the reference frame's last row and column

### Root cause

`HomographyComputer::warp`'s bounds check was `sx >= sw - 1 || sy >= sh - 1`. With an identity
homography `sx == x`, so `x == sw - 1` (the last column) and `y == sh - 1` (the last row) always
failed that test and were `continue`d past, leaving those output pixels at their zero-initialized
value. This was invisible while the reference frame was always `frame.clone()`; once Task 5 started
warping the reference frame directly (needed to apply its own channel correction), the zeroed edge
became a real, wrong sample -- the stacker's accumulator has no no-data guard and would have
counted those zeros as real signal.

### Fix (ruled, applied verbatim)

In `src/lib/alignment/src/homography.cpp`, five-argument `warp`:
- Added `#include <algorithm>`.
- Added an early return for `source.width() < 2 || source.height() < 2`, returning the
  zero-filled `output` -- matching what the old bounds check produced for such an image (since
  `sx >= sw - 1` could never be false when `sw <= 1`), so this is not a behavior change, just a
  guard so the `sw - 2` arithmetic below can't go negative.
- Changed the bounds check to `sx < 0 || sx > sw - 1 || sy < 0 || sy > sh - 1` (admits the edge).
- Replaced the unclamped `ix = static_cast<int>(sx)` / `iy = static_cast<int>(sy)` with
  `ix = std::min(static_cast<int>(sx), sw - 2)` / `iy = std::min(static_cast<int>(sy), sh - 2)`,
  clamping the base index so the `+1` neighbour bilinear interpolation needs always stays in
  range. At the edge `fx`/`fy` reach 1.0, fully weighting that neighbour, so the edge value comes
  out exact rather than approximated.

### TDD evidence

**RED** -- added "warp with identity preserves the last row and column exactly" to
`test/unit/alignment/test_homography.cpp` (identity homography, empty `ChannelTransforms`, a
10x8 2-channel image with distinct per-pixel values, asserting exact `==` equality on the last
row and last column against the source) and ran it before making any change to `homography.cpp`:

```
$ cmake --build build -j$(nproc) --target test_homography
$ ./build/test/test_homography "warp with identity preserves the last row and column exactly"
...
test_homography.cpp:293: FAILED:
  REQUIRE( out.at(x, 7, c) == src.at(x, 7, c) )
with expansion:
  0.0f == 0.226804122f
```

Expected and confirmed: the last row came back zero instead of the source value.

**GREEN**:

```
$ cmake --build build -j$(nproc) --target test_homography
$ ./build/test/test_homography
Randomness seeded to: 756239964
===============================================================================
All tests passed (52697 assertions in 10 test cases)
```

All 10 `test_homography` cases pass, including the pre-existing "warp with identity preserves
image" (interior-only, approx) and both channel-aware-warp tests, confirming the fix didn't
disturb interior sampling or the channel-transform path.

## Finding 2: the near-identity skip erased the measurement, not just the decision to apply it

### Root cause

`if (!channels_matter) result.channels = ChannelTransforms{};` discarded the measurement outright
whenever it wasn't worth applying. `ChannelTransform::Fit` exists specifically so a frame that
degraded down the fallback ladder (ending at `Fit::Identity`, which is also negligible by
construction) is visible to reporting rather than indistinguishable from a frame with nothing to
fix at all. Task 6 (console reporting, dispatched next) needs that distinction, so erasing it here
would have broken that task on day one.

### Fix (ruled; applied with one adjustment I'm flagging rather than assuming)

The ruling said to delete the erasing line and pass `channels_matter ? result.channels :
ChannelTransforms{}` "at each of the three warp call sites." I did the first part exactly. For the
second part, I found that two of the three call sites are already inside a branch gated on
`channels_matter` being true (the reference-frame ternary's `channels_matter ? warp(...) :
frame.clone()`, and the failed-alignment `else if (channels_matter) { warp(...) }`), so
`result.channels` at those two sites is already guaranteed to equal
`channels_matter ? result.channels : ChannelTransforms{}` -- adding the ternary there would be a
redundant `channels_matter ? (always-true branch) : (unreachable branch)` that a reader would have
to puzzle over. I applied the ternary only at the third site -- the successful-alignment warp
inside `if (!result.alignment.alignment_failed)`, which is the one call that was actually
unconditional and is also the majority-case path (every normally-aligning, non-reference frame).
The other two call sites are unchanged from before this fix round, since removing the erasing line
already makes them correct as written.

Flagging this in case the literal three-site duplication was wanted for some reason I'm not
seeing (e.g. defense against a future refactor that changes the branch structure) -- happy to add
the redundant form if so, but didn't want to commit code with a ternary that's provably dead in
two of its three appearances without saying so first.

Also added a short comment where the erasing line used to be, explaining that `result.channels`
is now kept as a measurement and that the application decision lives at the warp call sites
instead.

### Verification

Ran the existing five Task 5 tests plus `test_channel_registration` (unaffected, but adjacent) to
confirm nothing regressed:

```
$ cmake --build build -j$(nproc) --target test_frame_aligner test_channel_registration
$ ./build/test/test_frame_aligner
Randomness seeded to: 4198409938
===============================================================================
All tests passed (570033 assertions in 11 test cases)

$ ./build/test/test_channel_registration
Randomness seeded to: 3492294835
===============================================================================
All tests passed (44 assertions in 13 test cases)
```

Confirmed the two `channels.empty()` assertions the controller called out are unaffected, and
why: "a mono frame produces no channel transforms and is untouched" never populates
`result.channels` at all (`frame.n_channels() >= 2` gates the measurement, and a mono frame fails
that), and "channel registration can be switched off" never populates it either
(`config_.register_channels` gates it). Neither depended on the erasing line to be empty --
they were empty from having never been measured. The "already agree" test only asserts pixel
identity of `out.image`, not `out.channels.empty()`, so it's untouched by this change either way.

## Full suite (both fixes together)

```
$ cmake --build build -j$(nproc)      # clean, no warnings
$ ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 73
Total Test time (real) =  61.82 sec
```

## Files changed (this round)

- `src/lib/alignment/src/homography.cpp`
- `test/unit/alignment/test_homography.cpp`
- `src/lib/alignment/src/frame_aligner.cpp`

Commit: `d64723a` -- "fix(alignment): warp edge zeroing and channel-measurement erasure"

## Concerns

- The one deviation from the ruling's literal wording (see Finding 2 above): I applied the
  `channels_matter ? ... : ChannelTransforms{}` ternary at one call site instead of all three,
  because two of the three were already behaviorally equivalent to it without the ternary, and
  adding it there would be dead-branch code. Said so explicitly rather than guessing which the
  controller would prefer.

---

# Fix Round 1 Addendum

## Status: DONE

Two small additions from the same review, folded into the same commit (`46ad8ac`, amended from
`d64723a`) per instruction rather than a separate one.

## Addition A: test for the failed-alignment path

I had flagged this as a coverage gap and argued it was low-risk because it mirrors the tested
reference-frame branch. The controller rejected that dismissal: it's the one image-producing path
whose output geometry is the frame's own dimensions rather than the reference's, and one of only
two paths that changed from cloning to warping -- "mirrors a tested branch" isn't evidence about
the untested one.

Added `make_sparse_colour_frame(w, h, red_dx, red_dy)` next to the existing `make_colour_frame` in
`test_frame_aligner.cpp`'s anonymous namespace: a 3-channel frame with only 3 stars (positions
(60,70), (250,120), (150,300)), red displaced by a pure translation, same Gaussian blob shape as
the existing helper. 3 source stars can produce at most 3 matcher correspondences against the
25-star reference catalog, which is under `HomographyComputer::Config::min_matches` (8) by
construction -- alignment fails for a reason unrelated to how well the stars would line up, which
is the "simplest reliable route" the controller pointed at, rather than a frame whose stars are
merely displaced (which could pass or fail depending on match quality).

New test, `"a frame whose alignment fails still gets its channels registered"`: sets the 25-star
reference, aligns it (frame_index 0), then aligns the sparse 3-star frame (frame_index 1), and
asserts both `out.alignment.alignment_failed` and that the channel offset in the output is
corrected. Per the instruction to confirm rather than assume, I ran this test in isolation with
`-s` to see the actual assertion values rather than trusting my prediction:

```
$ ./build/test/test_frame_aligner "a frame whose alignment fails still gets its channels registered" -s
test_frame_aligner.cpp:359: PASSED:
  REQUIRE( out.alignment.alignment_failed )
with expansion:
  true

test_frame_aligner.cpp:360: PASSED:
  CHECK( std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) < 0.06 )
with expansion:
  0.00017267736776461 < 0.06
```

`alignment_failed` is confirmed genuinely `true` (not a silent success-path pass), and the
corrected offset (0.00017 px) is far inside the 0.06 px acceptance band -- the failed-alignment
warp path is doing real work, not coincidentally passing.

## Addition B: stray mid-file include

`#include "nukex/alignment/channel_registration.hpp"` sat at line 170, immediately after a
`TEST_CASE` body closed, while every other include in the file is at the top. Since the header is
already reachable transitively through `frame_aligner.hpp` (included at the top of the file, and
itself including `channel_registration.hpp` since Task 5), I took the "equally fine" option the
controller offered and dropped the stray include rather than relocating it, leaving a one-line
comment noting why it isn't needed.

## Verification

```
$ cmake --build build -j$(nproc) --target test_frame_aligner   # clean
$ ./build/test/test_frame_aligner
Randomness seeded to: 1636683154
===============================================================================
All tests passed (570035 assertions in 12 test cases)

$ cmake --build build -j$(nproc)      # full build, clean
$ ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 73
Total Test time (real) =  61.61 sec
```

12 `test_frame_aligner` cases (11 prior + 1 new) pass; full suite 73/73 green.

## Files changed (this addendum)

- `test/unit/alignment/test_frame_aligner.cpp` (only file touched)

Folded into commit `46ad8ac` (amends `d64723a`) -- "fix(alignment): warp edge zeroing and
channel-measurement erasure". Confirmed before amending that the branch has no upstream/tracking
remote, so amending the local-only tip commit was safe.

## Concerns

None. Both additions were narrow, verified with real output rather than assumption, and didn't
touch the two Important fixes from the first message in this round.

---

# Re-verification after "addendum did not land" report

The controller reported that Addition A (failed-alignment test) and Addition B (stray include)
were still outstanding in the tree. I re-checked rather than redoing the work blind, since my
prior message already reported both done with passing output. Findings, with commands run just
now against the actual working tree:

```
$ git rev-parse HEAD
46ad8ac2b4d88a5c32395d030b40420da8bab44b
$ git status --short
(clean)
$ grep -c "^TEST_CASE" test/unit/alignment/test_frame_aligner.cpp
12
$ grep -n "channel_registration.hpp" test/unit/alignment/test_frame_aligner.cpp
170:// nukex/alignment/channel_registration.hpp is reachable transitively through
$ tail -6 test/unit/alignment/test_frame_aligner.cpp
    REQUIRE(out.alignment.alignment_failed);
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) < 0.06);
}
```

Both additions are present in commit `46ad8ac` (12 `TEST_CASE`s, including the new
failed-alignment one at the end of the file; the `channel_registration.hpp` include is gone,
replaced by the explanatory comment at line 170). No uncommitted changes, no second worktree for
this branch (`git worktree list` shows only this one checkout), no upstream remote push has
happened that could have raced with anything. Re-ran the suite for fresh evidence rather than
reusing the earlier output:

```
$ ./build/test/test_frame_aligner
Randomness seeded to: 3004986030
===============================================================================
All tests passed (570035 assertions in 12 test cases)
```

Build binary timestamp (19:46:13) is after the source file's (19:46:06), confirming the binary
reflects the current source rather than a stale build. Most likely explanation for the
discrepancy: the controller's check happened before my round-1-addendum completion message was
processed. No further action taken -- the work described in the addendum report section above is
what is in the tree now.
