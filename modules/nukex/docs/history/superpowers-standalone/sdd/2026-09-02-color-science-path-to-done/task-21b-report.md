# Task 21b Report: Engine — route mono frames by the registered slot name, not the raw filter name

## What I implemented

In `src/lib/stacker/src/stacking_engine.cpp`, the mono routing branch
(`case FilterClass::BROADBAND_L / NARROWBAND_SINGLE / BROADBAND_RGB`) looked
up the voxel slot with `frame_filter.name` and hit `std::abort()` whenever
that raw filter name wasn't the slot `merge()` actually registered. Replaced
the lookup key with `per_frame_cfg.channel_names[0]` — the same
`ChannelConfig::from_filter(frame_filter)` result already computed earlier
in the per-frame loop (confirmed at `stacking_engine.cpp:368`, in scope
because the routing switch lives in the same per-frame loop body — no
fallback declaration was needed). Updated the branch's leading comment and
added an inline comment on the new line explaining why `frame_filter.name`
was unsafe to use directly (BROADBAND_L always maps to `"L"` regardless of
`Filter.name`, which may be `"L_unnamed"` or a raw unknown FILTER value like
a wheel-slot number).

Verified against `channel_config.cpp` that `channel_names[0]` is exactly the
mapping `merge()` registers for all three cases: `"L"` for BROADBAND_L,
`f.name` for NARROWBAND_SINGLE, `f.name` for BROADBAND_RGB — so lookup and
registration can no longer diverge.

Added two new `[.integration][phase_a]` test cases to
`test/integration/test_phase_a_router.cpp` (Step 1's text, verbatim):
missing FILTER on mono (`L_unnamed`, ASI2600MM) and unknown FILTER on mono
(wheel-slot `"1"`, ATR585M) — both must route into the `"L"` slot.

## What I tested and results

### TDD RED (Step 2)

Built `test_phase_a_router` clean (no compiler errors), then ran it before
touching the engine code:

```
$ ./test/integration/test_phase_a_router "[integration]" > red_output.txt 2>&1; echo "real_exit=$?"
/bin/bash: line 1: 1033614 Aborted                    (core dumped) ./test/integration/test_phase_a_router "[integration]" > ... 2>&1
real_exit=134
```

Tail of captured output — Catch2 printed the failing case name before the
process aborted:

```
-------------------------------------------------------------------------------
Phase A: missing FILTER on mono (L_unnamed) routes into the L slot
-------------------------------------------------------------------------------
/home/scarter4work/projects/nukex5/test/integration/test_phase_a_router.cpp:109
...............................................................................

/home/scarter4work/projects/nukex5/test/integration/test_phase_a_router.cpp:109: FAILED:
due to a fatal error condition:
  SIGABRT - Abort (abnormal termination) signal

===============================================================================
test cases:  6 |  5 passed | 1 failed
assertions: 22 | 21 passed | 1 failed
```

Exit code 134 = 128 + SIGABRT(6), confirmed via direct (non-piped) exit-code
capture. That crash is the RED, exactly as the brief predicted (a process
abort, not a Catch2 assertion failure line).

### TDD GREEN (Step 4)

Full rebuild after the fix — no errors:

```
$ make -j$(nproc) 2>&1 | grep -E "error"
(no output)
```

```
$ ctest 2>&1 | tail -5
66/66 Test #66: test_phase_b_qsolve ...............   Passed    0.01 sec

100% tests passed, 0 tests failed out of 66

Total Test time (real) =  57.91 sec
```

```
$ ./test/integration/test_phase_a_router "[integration]" 2>&1 | tail -6
Filters: [integration]
Randomness seeded to: 784622143
===============================================================================
All tests passed (27 assertions in 7 test cases)
```
(exit=0)

```
$ ./test/integration/test_phase_b_qsolve "[integration]" 2>&1 | tail -6
Filters: [integration]
Randomness seeded to: 1225549441
===============================================================================
All tests passed (537 assertions in 5 test cases)
```
(exit=0)

All four expectations from Step 4 matched exactly: 66/66 ctest, phase_a
7/7, phase_b 5/5, pristine output (the "pci id for fd 6" line is an
unrelated stderr message from the GPU driver probe, present on all runs
in this environment, not something this change introduced).

## Files changed

- `src/lib/stacker/src/stacking_engine.cpp` — mono routing branch only: one
  line changed (`frame_filter.name` → `per_frame_cfg.channel_names[0]`),
  comments updated/added. No other branch (BROADBAND_OSC, DUAL_NB_OSC,
  UNKNOWN) touched.
- `test/integration/test_phase_a_router.cpp` — two new test cases appended
  verbatim from the brief's Step 1.

## Self-review

- **Completeness**: both cases present and passing; the branch's leading
  comment updated to say the slot name comes from `from_filter()`; grepped
  the diff — confirmed no other `case FilterClass::` branch was touched.
- **Discipline**: diff is `2 files changed, 41 insertions(+), 2 deletions(-)`,
  scoped exactly to the brief's two files; no unrelated changes.
- **Testing**: RED captured as a process abort with exit code 134 and the
  Catch2 case-name tail, per the brief's Step 2 expectation (not a normal
  assertion failure line). GREEN captured verbatim for all four checks in
  Step 4.

## Issues or concerns

None. `per_frame_cfg` was already in scope in the per-frame loop, so the
brief's fallback instruction (declare it fresh before the switch) was not
needed.
