# Task 11 Report: Disposition, quarantine, and historical backfill

## Summary

Implemented `src/astrometa/disposition.py` with the five functions specified
in the task brief, plus the correction on `mark_missing`'s empty-`seen_hashes`
guard. All required brief tests plus additional coverage pass; full suite
(146 tests) passes.

## What I implemented

- `mark_missing(conn, seen_hashes)` — flips every currently-`present` frame
  not in `seen_hashes` to `missing`. Never touches a frame already `missing`
  or `quarantined`. Refuses to run (raises `ValueError`, touches nothing) if
  `seen_hashes` is empty while the database still holds `present` frames.
- `mark_culled(conn, content_hash, reason, source)` — records a frame as
  `quarantined` with a reason/source, verbatim per the brief.
- `quarantine(conn, content_hash, reason, dry_run=True)` — the only
  archive-mutating function. Computes `dest = src.parent / "rejected" /
  src.name`, moves (never deletes) when `dry_run=False`, updates the DB row's
  `path`/`disposition`/`disposition_reason`/`disposition_source='auto'`.
  Returns `None` for an unknown `content_hash` (see "Design decisions"
  below); raises `FileNotFoundError` if the DB's recorded file isn't
  actually on disk, in both dry-run and real mode.
- `session_thresholds(hfds)` — `(median + 2·pstdev, ABSOLUTE_HFD_FLOOR)`
  when ≥3 samples, else `(ABSOLUTE_HFD_FLOOR, ABSOLUTE_HFD_FLOOR)`. Kept the
  brief's constants (`HFD_SIGMA=2.0`, `ABSOLUTE_HFD_FLOOR=10.0`) verbatim, as
  instructed.
- `backfill_known_culls(conn, culls)` — generic, data-free; iterates
  `{"filename_like", "reason"}` records and applies `mark_culled(...,
  source="manual")` to every matching frame.

## How I guarded `mark_missing` (the correction)

Implemented exactly as specified: if `seen_hashes` is empty (`not
seen_hashes`, covering `None` too) **and** the database has at least one
`present` frame, raise `ValueError` before touching anything. If the
database has zero `present` frames, an empty `seen_hashes` is allowed and
returns `0` — there's nothing to protect.

**I did not add a threshold beyond strict emptiness** (e.g. "seen_hashes
covers <5% of present frames"). Reasoning, per the correction's instruction
to justify explicitly rather than pick a number silently:

1. No evidence for any specific fraction — picking one now would be exactly
   the kind of ungrounded fixed limit `session_thresholds` exists to *avoid*
   for HFD (a global cutoff can't tell a legitimately huge deliberate cull
   from a partial scan failure any better than a global HFD ceiling can
   tell a mediocre night from a junk night).
2. Task 5's `inventory.scan()` already raises loudly on the one concrete way
   this happens today (an unmounted/missing scan root) — the empty-set case
   this guard closes is the residual "some future caller bypasses `scan()`
   or a bug returns nothing" class, which strict emptiness already fully
   covers.
3. If a real small-but-nonempty failure mode is later observed, it should
   get its own named, evidence-backed guard at that point, not a number
   guessed today. This reasoning is written into the module and function
   docstrings.

Covering test: `test_mark_missing_refuses_empty_seen_hashes_when_frames_are_present`
(RED/GREEN below), plus `test_mark_missing_allows_empty_seen_hashes_when_no_frames_are_present`
proving the guard doesn't over-fire on a genuinely empty database.

## Design decisions beyond the brief's minimal reference

- **`quarantine` on an unknown `content_hash` returns `None`** rather than
  raising. The brief's own interface signature is `-> Path | None`, and
  since `frames` rows are never deleted (hard constraint), a genuine
  content_hash can never legitimately go missing from the table — so a
  lookup miss here is a caller error (typo/wrong hash), and `None` is the
  documented, intentional return for it per the given signature.
- **`quarantine` raises `FileNotFoundError` if the frame's recorded `path`
  isn't on disk** — in *both* dry-run and real mode. This isn't in the
  brief's reference implementation, but it's directly in the spirit of "no
  silent fallbacks that hide real errors": a dry run that returns a
  plausible `dest` for a move that could never actually succeed would be
  lying about what a real run would do, on the one function in this system
  explicitly called out as needing "corresponding care." Covered by
  `test_quarantine_raises_loudly_when_source_file_already_gone`.
- **`quarantine`'s real move always records `disposition_source='auto'`**
  (no `source` parameter in the interface). This matches the brief and
  makes sense given the split of responsibilities: `quarantine()` is the
  automated-move path (e.g. driven by `session_thresholds`), while
  historical/manual culls that don't involve a physical move go through
  `mark_culled(..., source="manual")` directly — which is exactly what
  `backfill_known_culls` does (39 real IC 59 frames were already physically
  deleted by the operator; there's no file left to move).

## Backfill pattern-discrimination trap (per your correction)

Did not hardcode the real 39-frame IC 59 list — `backfill_known_culls` stays
fully generic and data-free, matching the brief. Added
`test_backfill_known_culls_pattern_discriminates_by_filter`: two frames with
the same index (`0038`) but different filters (`Lqef` vs `HaO3`) — a pattern
targeting only the `HaO3` one (`%IC 59%HaO3%0038%`) matches exactly that
frame and leaves the `Lqef` one untouched. This proves the `filename LIKE`
approach discriminates correctly as long as callers include the filter
token in their pattern (which the real backfill call, supplied externally,
will need to do for the 0038/0039/0040 overlap).

## TDD evidence

**RED** — `.venv/bin/python -m pytest tests/test_disposition.py -v`, before
`src/astrometa/disposition.py` existed:

```
ImportError while importing test module '.../tests/test_disposition.py'.
E   ImportError: cannot import name 'disposition' from 'astrometa'
=========================== short test summary info ============================
ERROR tests/test_disposition.py
```

Expected failure reason confirmed: module doesn't exist yet.

**GREEN** — `.venv/bin/python -m pytest tests/test_disposition.py -v`, after
implementation:

```
collected 13 items
tests/test_disposition.py::test_vanished_frame_becomes_missing_not_deleted PASSED
tests/test_disposition.py::test_mark_missing_refuses_empty_seen_hashes_when_frames_are_present PASSED
tests/test_disposition.py::test_mark_missing_allows_empty_seen_hashes_when_no_frames_are_present PASSED
tests/test_disposition.py::test_mark_culled_records_reason_and_source PASSED
tests/test_disposition.py::test_quarantine_dry_run_does_not_touch_disk PASSED
tests/test_disposition.py::test_quarantine_moves_and_never_deletes PASSED
tests/test_disposition.py::test_quarantine_unknown_hash_returns_none PASSED
tests/test_disposition.py::test_quarantine_raises_loudly_when_source_file_already_gone PASSED
tests/test_disposition.py::test_session_thresholds_scale_with_the_night PASSED
tests/test_disposition.py::test_session_thresholds_falls_back_to_absolute_floor_with_too_few_samples PASSED
tests/test_disposition.py::test_backfill_known_culls PASSED
tests/test_disposition.py::test_backfill_known_culls_pattern_discriminates_by_filter PASSED
tests/test_disposition.py::test_backfill_known_culls_multiple_patterns_sum PASSED
============================== 13 passed in 0.02s ==============================
```

**Full suite** — `.venv/bin/python -m pytest -q`:

```
146 passed in 1.93s
```

(133 pre-existing + 13 new; nothing else broke.)

## No test touches a real archive path

Verified: `grep -n "/mnt/qnap\|/archive\|Config(" tests/test_disposition.py`
returns nothing. Every test that creates a frame file does so under
pytest's `tmp_path` fixture (25 occurrences in the file); the only "real"
paths used anywhere are the unresolved literal `"/a/b.fit"` /
`"/a/Light_..."` strings used purely as DB `path` column values in tests
that never touch disk (`mark_missing`, `mark_culled`, `backfill_known_culls`
tests) — none of those call `quarantine`, so no filesystem operation is ever
attempted against them.

## Files changed (as of the initial commit — see "Files changed (final)" below for the full picture after the addendum)

- `src/astrometa/disposition.py` (new)
- `tests/test_disposition.py` (new)

Commit: `ca1809a` — "feat: disposition tracking, quarantine, and historical
cull backfill"

## Self-review (fresh eyes)

- **Completeness**: all 5 interface functions implemented with the exact
  signatures specified. Brief's 7 example tests are all present (verbatim
  assertions), plus 6 more covering the correction and edge cases I judged
  worth defending given "no silent fallbacks" and "quarantine deserves
  corresponding care."
- **YAGNI check**: did not add a `source` parameter to `quarantine` (not in
  interface), did not add configurable quarantine directory naming, did not
  add fraction-based guard to `mark_missing` (justified above), did not
  hardcode the real cull list. Stayed inside the specified interface.
- **Test honesty**: every assertion checks a real behavior (disk state via
  `.exists()`/`.read_bytes()`, DB state via direct `SELECT`), no tests
  assert against mocks or trivially-true conditions. The RED run was
  captured before any implementation existed, not synthesized.
- **Style consistency**: module-level docstring with the "why" (matching
  `grouping.py`/`quality.py`/`inventory.py`'s density), `import sqlite3` at
  module top used only for type hints, no `row_factory` mutation (module
  never sets it, so no save/restore needed), stdlib imports alphabetized
  the same way as `quality.py`.
- **Python version**: no 3.14-only syntax; `set[str]`, `list[dict]`,
  `tuple[float, float]`, `Path | None` are all fine on 3.13. Dev venv runs
  3.14.7 but nothing here depends on it.

## Addendum: rescan-clobbers-quarantine, resolved per team-lead ruling

Raised as an open question above; team-lead's ruling was neither of the two
options I proposed. **Ruling: `EXCLUDED_PATH_MARKERS` stays untouched.**
Excluding `rejected/` from scans would make the store blind to whether
culled files still exist on disk — wrong for this project specifically,
since culled frames are deliberately *kept*, not deleted, and the whole
point of this task is to record that a frame is absent from the working
set *on purpose*, with the file still findable. The real invariant:
**inventory owns the `present`/`missing` transition; the operator (via
`mark_culled`/`quarantine`) owns `quarantined`.** The defect was the
unconditional clobber in inventory's `ON CONFLICT`, not the directory walk.

This directed a surgical, out-of-scope change to `src/astrometa/inventory.py`
(reviewed under Task 5) — **directed by team-lead as controller, not scope
creep initiated by me.** Confirmed minimal: only the `ON CONFLICT` clause's
`disposition` assignment changed, from unconditional `disposition='present'`
to `disposition=CASE WHEN disposition='quarantined' THEN disposition ELSE
'present' END` (SQLite UPSERT semantics: an unqualified column name in the
`SET` clause reads the pre-conflict row's existing value). `path`,
`last_seen`, and `read_error` still update unconditionally — relocating a
quarantined frame's recorded path to where it now actually lives (inside
`rejected/`) is useful, not a disposition change.

**RED** — added `test_quarantine_survives_a_rescan` to
`tests/test_disposition.py` (inventory a real frame written with astropy →
`quarantine(..., dry_run=False)` → re-run `inventory.scan()` on the same
root → assert disposition/reason/path) and ran it against the unfixed
clause:

```
.venv/bin/python -m pytest tests/test_disposition.py -v -k "quarantined_frame or survives_a_rescan"
...
>       assert row[0] == "quarantined"
E       AssertionError: assert 'present' == 'quarantined'
E         - quarantined
E         + present
FAILED tests/test_disposition.py::test_quarantine_survives_a_rescan
1 failed, 1 passed, 13 deselected
```

(The other test in that run, `test_mark_missing_never_touches_a_quarantined_frame`,
already passed — see "mark_missing invariant" below.)

**GREEN** — after the `inventory.py` fix:

```
.venv/bin/python -m pytest tests/test_disposition.py -v
...
15 passed in 0.14s
```

**Full suite after the fix**: `.venv/bin/python -m pytest -q` → `148 passed
in 1.93s` (146 prior + these 2 new tests).

### `mark_missing` invariant, verified

Team-lead asked me to confirm `mark_missing` only ever transitions rows
whose disposition is `present`. Re-checked the implementation: its
candidate query is `SELECT content_hash FROM frames WHERE
disposition='present'` — a quarantined (or already-missing) row is never
even a candidate, so it can't be flipped. Added an explicit test,
`test_mark_missing_never_touches_a_quarantined_frame`, to make this a
guarded invariant rather than an implicit property: marks a frame culled,
then calls `mark_missing(conn, set())` (empty seen_hashes, legal here since
the only `present` frames were already culled — 0 present frames) and
asserts the frame is still `quarantined`, not `missing`. Passed on first
run (no code change needed for this part, just coverage).

## Files changed (final)

- `src/astrometa/disposition.py` (new)
- `tests/test_disposition.py` (new, later extended with the two tests above)
- `src/astrometa/inventory.py` (surgical `ON CONFLICT` fix, team-lead directed)

Commits:
- `ca1809a` — "feat: disposition tracking, quarantine, and historical cull backfill"
- `396450c` — "fix: inventory rescan must never overwrite a quarantined disposition"

**Status: DONE** — the cross-task integration gap I flagged has been
resolved per team-lead's ruling, with RED/GREEN evidence and an explicit
covering test for the `mark_missing` invariant they asked me to verify.

## Addendum 2: review fix round — quarantine idempotency + overwrite refusal

Task 11 review came back Approved with one Important finding and one Minor
that team-lead upgraded. Both fixed; `inventory.py`, `mark_missing`,
`session_thresholds`, and the backfill filter discrimination were
explicitly left untouched per team-lead's instruction (all independently
re-verified correct by them, including the SQLite UPSERT semantic).

### Fix 1 (Important): repeat quarantine nested `rejected/rejected/`

Once a frame is quarantined its DB `path` points into `rejected/`, so a
second `quarantine(..., dry_run=False)` call on the same `content_hash`
recomputed `dest = src.parent / "rejected" / src.name` from the
already-relocated path and moved it again — unbounded nesting on further
repeats. Realistic trigger: a quality-gate re-run, or an operator
re-applying `mark_culled` then `quarantine`.

**Fix**: `quarantine()` is now idempotent. The row lookup now also selects
`disposition`; if the frame is already `disposition='quarantined'`, **or**
its recorded path's parent directory is already named `rejected` (checked
independently as a structural fallback, in case disposition ever
disagrees), the function returns the current location unchanged and makes
**no** database write — so a differently-worded `reason` on a repeat call
does not silently overwrite the originally recorded one.

### Fix 2 (upgraded from Minor — real data-loss path)

`shutil.move` falls back to `os.rename` on the same filesystem, which
silently overwrites an existing destination file. The operator's own
manual culling method is exactly "move the bad sub into `rejected/` by
hand," so that directory routinely already holds files he put there
himself, and filenames in this archive are not unique (1,989 collide) — a
machine silently clobbering a hand-culled frame via an automated
`quarantine()` call would itself be a deletion, on the one function in
this system whose entire promise is "moves, never deletes."

**Fix**: before moving, `quarantine()` now checks `dest.exists()` and, if
so, raises `FileExistsError` naming both `src` and `dest`, leaving the
source untouched — no uniquifying suffix is invented; a collision is for
the operator to see and resolve by hand. This check runs in **both**
`dry_run` and real mode, consistent with the existing `FileNotFoundError`
check: a dry run that reports a move as feasible when it would actually
clobber something would itself be a silent-fallback bug.

Left alone per team-lead's instruction: `backfill_known_culls` counting
matches (not distinct newly-culled frames) — documented, correct behavior.

### TDD evidence

**RED** — 4 new tests added to `tests/test_disposition.py`, run against
the unfixed `quarantine()`:

```
.venv/bin/python -m pytest tests/test_disposition.py -v -k "idempotent or overwrite or collision"
...
>       assert dest2 == dest1
E       AssertionError: assert PosixPath('.../M 42/rejected/rejected/Light_M 42_0001.fit')
                            == PosixPath('.../M 42/rejected/Light_M 42_0001.fit')
FAILED tests/test_disposition.py::test_quarantine_is_idempotent_on_repeat_calls
...
>       assert dest == f
E       AssertionError: assert PosixPath('.../rejected/rejected/Light_M 42_0001.fit')
                            == PosixPath('.../rejected/Light_M 42_0001.fit')
FAILED tests/test_disposition.py::test_quarantine_treats_a_path_already_under_rejected_as_idempotent
...
>       with pytest.raises(FileExistsError):
E       Failed: DID NOT RAISE FileExistsError
FAILED tests/test_disposition.py::test_quarantine_refuses_to_overwrite_an_existing_destination
FAILED tests/test_disposition.py::test_quarantine_dry_run_also_refuses_a_destination_collision
4 failed, 15 deselected in 0.14s
```

Failures confirmed for the expected reasons: real nesting into
`rejected/rejected/`, and no error raised on a destination collision in
either mode.

**GREEN** — after the fix, `tests/test_disposition.py` full run:

```
.venv/bin/python -m pytest tests/test_disposition.py -v
...
19 passed in 0.13s
```

**`tests/test_inventory.py`** (re-run in full per team-lead's request, even
though this round touched no inventory code):

```
.venv/bin/python -m pytest tests/test_inventory.py -v
...
16 passed in 0.15s
```

**Full suite**:

```
.venv/bin/python -m pytest -q
...
152 passed in 2.03s
```

(148 prior + 4 new; nothing else broke.)

### Files changed (this round)

- `src/astrometa/disposition.py` — `quarantine()` only: idempotency check
  and destination-collision check, plus docstring
- `tests/test_disposition.py` — 4 new tests

Commit: `cb572cd` — "fix: quarantine idempotency and destination-collision
refusal"

**Status: DONE** — both review findings fixed with RED/GREEN evidence;
`inventory.py`/`mark_missing`/`session_thresholds`/backfill discrimination
untouched as instructed.
