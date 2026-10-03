# Task 6 Report: Report channel registration in the Process Console

## Summary

Implemented the brief's formatter (`describe_channel_transforms`) verbatim, and
implemented the ruled amendment in full: promoted `kNegligibleChannelShiftPx`
to a single shared definition in `channel_registration.hpp`, and replaced the
brief's `if (!aligned.channels.empty())` engine guard with the two-condition
gate the amendment specifies (applied OR gave-up-on-a-channel), plus the
additional gave-up test case (Step C).

## What I implemented

**Step A (constant promotion).**
`kNegligibleChannelShiftPx` moved from the anonymous namespace in
`frame_aligner.cpp` to namespace scope in `nukex`, in
`channel_registration.hpp`, with its original explanatory comment plus one
sentence noting it is now shared between the aligner and the console report
so the two cannot disagree about what "applied" means.
`frame_aligner.cpp`'s local copy and its now-empty anonymous namespace block
were deleted. `frame_aligner.cpp` picks the header's definition up
transitively through `frame_aligner.hpp`, which already includes
`channel_registration.hpp` (verified before relying on it).

**Step B (engine gate).**
`stacking_engine.cpp` gained an explicit
`#include "nukex/alignment/channel_registration.hpp"` and, right after the
existing `aligned:` status block, a new block that:
- computes `corner_radius` from `image.width()/height()` — the same `image`
  variable passed into `aligner.align(image, f)` two lines above, i.e. the
  same input `frame_aligner.cpp` used for its own corner_radius, so the two
  can't disagree about what counts as negligible;
- sets `applied = !aligned.channels.negligible(corner_radius, kNegligibleChannelShiftPx)`;
- sets `gave_up = true` if any non-reference channel's `fit ==
  ChannelTransform::Fit::Identity`;
- emits `"  channel reg: " + desc` only when `applied || gave_up`, and only
  when `desc` is non-empty (kept as a defensive no-op guard from the brief;
  it can't actually be empty when either condition holds, since a non-empty
  `ChannelTransforms` always has at least one non-reference channel to
  describe).

**Step C (gave-up test).**
Added a third test case, `describe_channel_transforms names channels the fit
gave up on`, with all non-reference channels at `Fit::Identity`, asserting
the result is non-empty and names `ch0`/`ch2` with `"identity"`, and omits
the reference (`ch1`). Kept the brief's two original tests (rung+size, and
the empty-`ChannelTransforms` case) unchanged.

## TDD evidence

**RED** — ran before any implementation, right after appending all three
test cases (including the Step C amendment test) to
`test/unit/alignment/test_channel_registration.cpp`:

```
$ cmake --build build -j$(nproc) --target test_channel_registration 2>&1 | tail -30
...
test_channel_registration.cpp:380:11: error: 'describe_channel_transforms' was not declared in this scope
...
test_channel_registration.cpp:396:27: error: 'describe_channel_transforms' was not declared in this scope
gmake: *** [Makefile:699: test_channel_registration] Error 2
```

Expected and correct: the formatter didn't exist yet, so every new test case
referencing it failed to compile. This was the only failure mode possible at
this point since nothing else in the new tests used unimplemented behavior.

**Intermediate build error (expected, not a bug)** — after declaring and
implementing `describe_channel_transforms` (Step 3) but before deleting
`frame_aligner.cpp`'s local copy of the constant (Step A), the build failed
with:

```
frame_aligner.cpp:49:40: error: reference to 'kNegligibleChannelShiftPx' is ambiguous
  - candidate 1: nukex::{anonymous}::kNegligibleChannelShiftPx (frame_aligner.cpp:11)
  - candidate 2: nukex::kNegligibleChannelShiftPx (channel_registration.hpp:20)
```

This is exactly the collision the amendment predicted — two definitions in
the same translation unit's visible scope — and confirmed the promoted
constant was reachable from `frame_aligner.cpp` before I deleted the local
one, i.e. that deleting it was safe rather than losing the include path.

**GREEN** — after deleting the local copy in `frame_aligner.cpp`:

```
$ cmake --build build -j$(nproc) --target test_channel_registration
[100%] Built target test_channel_registration

$ ./build/test/test_channel_registration
Randomness seeded to: 2396027307
===============================================================================
All tests passed (173 assertions in 16 test cases)
```

16 test cases = 13 pre-existing + 3 new (confirmed by
`grep -c '^TEST_CASE'` = 16 and `git diff | grep -c '^+TEST_CASE'` = 3).

**Full build + suite**, run once before committing:

```
$ cmake --build build -j$(nproc)
... [100%] Built target NukeX-pxm

$ ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 73
Total Test time (real) =  61.49 sec
```

## Confirmation: exactly one definition of the negligibility constant

```
$ grep -rn "kNegligibleChannelShiftPx" --include="*.hpp" --include="*.cpp" src test
src/lib/alignment/src/frame_aligner.cpp:42:                    kNegligibleChannelShiftPx);        (use)
src/lib/stacker/src/stacking_engine.cpp:697:    corner_radius, kNegligibleChannelShiftPx);          (use)
src/lib/alignment/include/nukex/alignment/channel_registration.hpp:20:constexpr double kNegligibleChannelShiftPx = 0.01;   (the one definition)
```

One `constexpr` definition, two use sites (the aligner's apply-decision and
the engine's report-decision), which was the whole point of the amendment.

## Files changed

- `src/lib/alignment/include/nukex/alignment/channel_registration.hpp` —
  added `kNegligibleChannelShiftPx` at namespace scope (`#include <string>`
  added for the formatter's return type); declared
  `describe_channel_transforms`.
- `src/lib/alignment/src/channel_registration.cpp` — implemented
  `describe_channel_transforms` verbatim from the brief (`#include <cstdio>`,
  `#include <string>` added).
- `src/lib/alignment/src/frame_aligner.cpp` — deleted the local
  `kNegligibleChannelShiftPx` and its now-empty anonymous namespace; no other
  change.
- `src/lib/stacker/src/stacking_engine.cpp` — added the
  `channel_registration.hpp` include and the two-condition report gate
  (amendment Step B) in place of the brief's `!aligned.channels.empty()`
  guard.
- `test/unit/alignment/test_channel_registration.cpp` — appended the brief's
  two tests plus the Step C gave-up test.

## Self-review findings

- Checked every added line across all four production/test files for bytes
  >= 0x80 with `grep -P '[^\x00-\x7F]'` on the diff's `+` lines: none found.
  All my new comments use `--` for parenthetical dashes, matching the
  existing codebase convention (the brief's own commit-message style), never
  an em-dash.
- Verified `image` (the variable passed to `aligner.align(image, f)`) is not
  mutated between the align call and my new block, so `image.width()` /
  `image.height()` at the report site are the identical values
  `frame_aligner.cpp` used internally to compute its own `corner_radius` —
  this was the amendment's explicit requirement ("use the same corner_radius
  the aligner uses").
- Verified the `gave_up` loop can't spuriously fire on a mono frame or an
  empty `ChannelTransforms`: it's gated behind `!aligned.channels.empty()`,
  and `measure_channel_transforms` only ever returns non-empty when
  `n_channels() >= 2`, so there is always at least one non-reference channel
  to examine.
- Verified `desc` cannot be empty when `applied || gave_up` is true: a
  non-empty `ChannelTransforms` always has >=1 non-reference channel, and
  `describe_channel_transforms` emits a line for every non-reference channel
  regardless of its `Fit` value (including `Identity`, via the "nothing
  measurable" line). The `if (!desc.empty())` in my engine code is therefore
  dead-but-harmless defensive code inherited from the brief's original
  version; I left it rather than removing it, since removing it wasn't asked
  for and it costs nothing.
- Confirmed the brief's original two tests are byte-for-byte as specified
  (I did not alter their expected strings or assertions).
- Re-read `frame_aligner.hpp`'s include list before relying on transitive
  visibility of the promoted constant, rather than assuming it.

## Post-review fix: pre-existing em-dashes in stacking_engine.cpp

The team lead reviewed the concern above and correctly overruled deferring
it: the file is already in this task's diff, the fix is mechanical, and it's
the exact defect class Task 6's own ASCII test guards against. Fixed in this
task, amended into the same commit rather than filed as a follow-up.

Three em-dashes in `src/lib/stacker/src/stacking_engine.cpp`, each replaced
with `" -- "` (matching the branch's existing convention, e.g. in this
task's own new comments):

- line 563 (live console string) — `"  skipped — unknown FILTER='..."` to
  `"  skipped -- unknown FILTER='..."`
- line 643 (comment only, but fixed so nobody copies it into a string later)
  — `// ("SKIPPED — blown out, X%") ...` to `// ("SKIPPED -- blown out, X%") ...`
- line 664 (live console string) — `"  aligned: SKIPPED (blown out — ")` to
  `"  aligned: SKIPPED (blown out -- ")`

Verified each replacement with a Python script asserting exactly one
occurrence of each old string before substituting (no accidental double
replacement, no missed occurrence), then re-read the surrounding context to
confirm sensible spacing (`grep`/`sed -n` shown in the commands below).

### Confirmation grep: no other non-ASCII inside a string/char literal under `src/`

A plain `grep -rP '[^\x00-\x7F]'` over `src/` is not sufficient here: this
codebase's comments legitimately use em-dashes and Greek letters throughout
(statistics/optics documentation), so a byte-level grep alone returns dozens
of files and can't distinguish a harmless comment from a live string reaching
PCL. It would also miss a non-ASCII character injected via `\xHH` hex
escapes, octal escapes, or `\uXXXX`/`\UXXXXXXXX` universal character names —
exactly the case the team lead warned about, and exactly the case this
search actually found (below).

I wrote a small state-machine scanner
(`/tmp/claude-1000/-home-scarter4work-projects-nukex5/b4fa40e8-a54f-4e20-8924-28702af5e54f/scratchpad/scan_string_literals.py`)
that walks each `.cpp`/`.hpp`/`.h`/`.cc` file tracking line comments, block
comments, char literals, normal string literals, and raw string literals
(`R"delim(...)delim("`), and flags non-ASCII only when it falls *inside* a
literal — plus flags `\xHH`, octal, and `\u`/`\U` escapes that assemble to a
byte >= 0x80 or inject a wide character.

Command and full output, run from repo root after the fix above:

```
$ python3 /tmp/.../scan_string_literals.py src
src/lib/calibration/src/channel_decomposer.cpp:35: [string] raw-byte  literal=') is singular — QE values must form a non-degenerate basis. '
src/module/NukeXProgress.cpp:117: [string] hex-escape(\xe2) ...  (18 findings total, all the same line)
src/module/NukeXProgress.cpp:117: [string] hex-escape(\x95) ...
src/module/NukeXProgress.cpp:117: [string] hex-escape(\x90) ...
  [... repeated escape hits, same single line, 6 repetitions of the 3-byte sequence ...]

TOTAL: 19 finding(s)
```

Also confirmed `stacking_engine.cpp` specifically is now clean at the
literal level:

```
$ python3 /tmp/.../scan_string_literals.py src/lib/stacker
No non-ASCII content found inside any string/char literal under src/lib/stacker
```

### Two additional findings — NOT fixed, outside this task's scope

Both are real, live bugs, not comments:

1. **`src/lib/calibration/src/channel_decomposer.cpp:35`** — a raw em-dash
   byte directly in a `SingularQError` exception message: `"... ) is
   singular — QE values must form a non-degenerate basis. ..."`. This
   exception's `what()` text can surface to the user/log.

2. **`src/module/NukeXProgress.cpp:117`** — `console_.WriteLn(String().Format(
   "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90 %s (%d steps)
   \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90", name.c_str(), total_steps))`. The
   `\xe2\x95\x90` hex escapes are the UTF-8 encoding of U+2550 (a
   box-drawing double horizontal line, "═"), six of them bracketing every
   phase-start banner. This is a **currently shipping** mojibake bug in the
   module's own Process Console output — the exact case a naive "grep for
   literal characters" would miss, since there is no visible non-ASCII
   character in the source, only its hex-escaped bytes.

I did not fix either: both are outside Task 6's declared file list
(`channel_registration.hpp/.cpp`, `frame_aligner.cpp`, `stacking_engine.cpp`,
the test file), and the team lead's instruction to amend into this commit was
specifically about the stacking_engine.cpp em-dashes already found. Per the
project's "no stub tickets" rule, this needs an explicit dispatch/promote/drop
decision rather than a filed note — flagging both for that decision now
rather than fixing them unasked or letting them sit undecided.

## Concerns

- The two additional non-ASCII findings above
  (`channel_decomposer.cpp:35`, `NukeXProgress.cpp:117`) are real, live,
  currently-shipping bugs and need a dispatch decision.
