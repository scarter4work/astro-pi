# Task 7 report: ASTAP solver wrapper

## What was implemented

`src/astrometa/solve.py`, three functions matching the brief's interface exactly:

- `parse_ini(text: str) -> dict` — parses an astap_cli `.ini` sidecar into
  `{"solved", "ra", "dec", "scale_arcsec_px", "rotation", "warning"}`.
- `solve_frame(cfg, frame_path, hint=None) -> dict` — copies the frame into
  `cfg.scratch_dir` (never touches the source in place), invokes
  `astap_cli` with a positional hint (`-ra` in hours, `-spd`, `-r 30`) when
  header coordinates are available, or a blind `-r 180` search otherwise,
  and returns a solution dict or a `solved: False` failure dict (timeout,
  or no `.ini` produced).
- `solve_fields(conn, cfg, limit=None) -> int` — selects fields with
  `solve_source='none' AND solve_attempts < 3`, solves each field's
  earliest valid representative frame, writes the solution (or increments
  `solve_attempts` and records `solve_error` on failure), commits per
  field, and returns the count newly solved.

Deviations from the brief's literal code (all style/robustness, no
constant or formula changed):
- `import sqlite3` moved to module top (per team-lead's style note).
- `conn.row_factory` mutation scoped with try/finally around the whole
  function body (mirrors `cluster.py`'s `assign_fields` pattern) rather
  than left set permanently.
- `if limit:` → `if limit is not None:` so `limit=0` means "solve
  nothing" instead of being silently treated as "no limit".
- Added `MAX_SOLVE_ATTEMPTS = 3` named constant (was the literal `3`) and
  `ORDER BY id` on the candidate query for deterministic runs, both
  matching conventions already used in `cluster.py`.
- Factored the five-key "unsolved" dict into a `_UNSOLVED` template
  spread with `**` at each of parse_ini/solve_frame's three failure
  returns, to remove repetition — behaviorally identical to the brief.

`BLIND_RADIUS=180`, `HINT_RADIUS=30`, `TIMEOUT_S=120`, and every `.ini`
field name/formula (`-ra` = ra_degrees/15, `-spd` = dec+90, CDELT1→arcsec
via `*3600.0`) are verbatim from the brief.

## TDD evidence

**RED** — `.venv/bin/python -m pytest tests/test_solve.py -v` before
`solve.py` existed:

```
ImportError while importing test module '.../tests/test_solve.py'.
E   ImportError: cannot import name 'solve' from 'astrometa'
```

(Surfaces as an `ImportError` at collection rather than the brief's
predicted `ModuleNotFoundError` at test-run time, because the test module
imports `solve` at top level rather than inside each test — same root
cause: the module didn't exist yet.)

**GREEN** — after writing `solve.py`:

```
.venv/bin/python -m pytest tests/test_solve.py -v
============================== 14 passed in 1.30s ==============================
```

Full suite: `.venv/bin/python -m pytest -v` → **74 passed**, no
regressions in tasks 1-6's tests.

## What was tested

`tests/test_solve.py`, 14 tests:

**parse_ini** (the brief's 3 required tests, unchanged) — extracts a full
solution, reports `PLTSOLVD=F` as unsolved, treats empty input as
unsolved.

**solve_frame** (6 tests, using a single env-var-driven stub script
written to `tmp_path` and chmod +x'd — no real ASTAP needed):
- `test_solve_frame_stub_solver_leaves_source_directory_clean` — the
  **required** test. See "Read-only-source guarantee" below.
- `test_solve_frame_hint_converts_to_hours_and_spd` — stub logs its argv;
  asserts `-ra`/`-spd`/`-r 30` are exactly `ra/15.0` and `dec+90.0`.
- `test_solve_frame_blind_search_uses_wide_radius_when_no_hint` — asserts
  `-ra`/`-spd` are absent and `-r 180` is used.
- `test_solve_frame_no_ini_produced_is_recorded_as_unsolved` — stub exits
  without writing anything (simulates a Moon/planet frame); asserts
  `warning == "no .ini produced"`, not an exception.
- `test_solve_frame_records_timeout_loudly` — stub sleeps past a
  monkeypatched `solve.TIMEOUT_S = 0.2`; asserts a `solved: False` result
  with `"timeout"` in the warning, no hang (this test runs in well under
  a second — it does not wait out the real 120s timeout).
- `test_solve_frame_against_real_astap` — **optional integration test**,
  `pytest.mark.skipif(not Path("/opt/astap/astap_cli").exists())`, plus a
  runtime skip if no frame is found under `/mnt/qnap/astro_data`. Finds a
  real light frame, reads its real FITS header via `fitsheader.py`, and
  runs an actual solve. This ran and passed on this dev box — see
  "Real ASTAP verification" below.

**solve_fields** (5 tests, not explicitly required but directly covering
the task's stated global constraints — idempotency and the
unsolvable-vs-unprocessed distinction — using the same stub):
- `test_solve_fields_writes_solution_and_marks_solved` — full DB
  round-trip: `solved_ra`/`solved_dec`/`scale_arcsec_px`/`rotation`/
  `solve_source='astap'`/`solve_at`/`solve_attempts=1`/`solve_error=NULL`,
  plus re-checks the source frame directory is untouched.
- `test_solve_fields_is_idempotent` — second call on an already-solved
  field returns 0.
- `test_solve_fields_records_failure_without_marking_solved` — failure
  path leaves `solve_source='none'`, increments `solve_attempts`, sets
  `solve_error`.
- `test_solve_fields_stops_retrying_after_max_attempts` — 3 failing
  calls cap `solve_attempts` at 3; a 4th call doesn't touch the field
  again (this is the "unsolvable frames aren't retried forever, and stay
  distinguishable from unprocessed" requirement, exercised directly).
- `test_solve_fields_respects_limit` — two unsolved fields, `limit=1`
  solves exactly one.

I did not fabricate a passing test by asserting on the stub's echoed
input anywhere — every assertion about file-system side effects reads
the real file system state after the call, not something the stub wrote
back.

## Read-only-source guarantee — how it was tested

`test_solve_frame_stub_solver_leaves_source_directory_clean`
(`tests/test_solve.py:113`):

1. Creates `tmp_path/read_only_source/frame.fit` and treats that
   directory as the archive mount.
2. Points `cfg.astap_bin` at a stub executable (`tmp_path/stub_astap.py`,
   chmod +x) that reads its own `-f` argument and writes an `.ini` file
   next to *that* path — i.e. it faithfully reproduces astap_cli's real
   "sidecar next to the input" behavior, whatever path it's given.
3. Calls `solve.solve_frame(cfg, frame, hint=(42.8625007, 60.0697047))`.
4. Asserts the returned solution is correct (`solved is True`, ra/dec
   match).
5. Asserts `[p.name for p in source_dir.iterdir()] == ["frame.fit"]` —
   i.e. after the call, the only thing in the source directory is still
   the original file. If `solve_frame` ever handed the stub the original
   `frame_path` instead of a scratch copy, the stub would write
   `frame.ini` beside it and this assertion would fail.

This is a genuine constraint test, not a tautology: the stub's write
target is derived entirely from the path it's invoked with, so the
assertion only passes because `solve_frame` copies into
`cfg.scratch_dir` first (as required) and never passes `frame_path`
itself to the subprocess.

## Real ASTAP verification

Before writing the integration test, I confirmed by hand that a real
frame actually solves through this code path, using
`/mnt/qnap/astro_data/2026-01-19/Light_NGC 1893_178deg_300.0s_Bin1_Full_20260119-205323_0012.fit`
(header `RA=81.06957`, `DEC=33.468008`, degrees — confirming the
`ra_degrees/15.0` hour conversion is correct):

```
$ time /opt/astap/astap_cli -f solve.fit -d /opt/astap -ra 5.404638 -spd 123.468008 -r 30 -fov 0 -wcs
...
Solution found: 05: 22  34.0 +33d 26  50
Solved in 0.8 sec. Δ was 21.4'.
real 0m0.822s
```

`PLTSOLVD=T`, solved RA 80.64° vs. header RA 81.07° — a ~0.4° discrepancy
between commanded and true pointing, consistent with the task's premise
that header RA/DEC are coarse and the plate solve is ground truth. This
manual scratch copy was `rm -rf`'d afterward, and the automated
integration test (`test_solve_frame_against_real_astap`) reproduces this
through the actual `solve_frame` code and passed.

## Files changed

- `src/astrometa/solve.py` (new)
- `tests/fixtures/solved.ini` (new, byte-for-byte the brief's fixture)
- `tests/test_solve.py` (new, 14 tests)

Commit: `537e03c feat: ASTAP solver wrapper with scratch-copy isolation`

## Self-review

- **Completeness**: all three interface functions implemented per spec;
  the brief's 3 required tests plus the team-lead's required stub test
  are present and passing; global constraints (never write to archive,
  loud errors, idempotent, attempt-capped, distinguishable
  unsolvable-vs-unprocessed) are each directly exercised by a test.
- **Quality**: matches existing codebase idioms — `db.Row` scoping
  pattern from `cluster.py`, doc-comment density and "why" explanations
  matching `cluster.py`/`inventory.py`, `pytest.approx`/tuple-indexed row
  assertions matching `test_cluster.py`.
- **YAGNI**: did not add retry backoff, logging, CLI entry point, or
  anything beyond the specified interfaces and their direct test
  coverage. The `_UNSOLVED` dict factor-out is the only structural
  deviation from the brief's literal code, and it's a pure
  no-behavior-change dedup.
- **Test honesty**: read-only-source assertion is on real file-system
  state, not stub echo; the timeout test uses a monkeypatched constant so
  it's fast but still exercises the real `subprocess.TimeoutExpired`
  path; the integration test hit real hardware/binary and I watched it
  pass, not just assumed it would.

## Concerns

None blocking. One thing worth flagging for whoever wires up the real
pass: `solve_fields`' representative-frame query
(`WHERE field_id=? AND read_error IS NULL LIMIT 1`) relies on the
invariant, established in `cluster.py`, that a frame only ever gets a
`field_id` after a successful header+pixel read — so `read_error` should
already be NULL for every frame `cluster.py` assigns. I didn't add a test
for the (currently unreachable, per that invariant) case of a field with
zero valid representative frames, matching how `cluster.py` documents
rather than tests its own unreachable branches.

---

## Fix round 1 (review finding, Important)

**Finding**: `solve_frame` never inspected `.returncode`/`.stderr`, so a
broken install (missing star database, bad binary) produced the same
`"no .ini produced"` message as a genuine unsolvable frame. With
`solve_attempts` capped at 3, a misconfigured install on the first run
would burn the cap across every field and permanently mark ~24,000 fields
failed — recoverable only by manual DB surgery.

Team-lead measured against the real `/opt/astap/astap_cli` before
prescribing the fix and found returncode alone can't discriminate
(unsolvable frame and bad database dir both return 1; a missing input
file returned 0). I independently re-measured against the same binary
while implementing (commands and full output below) and got a
**different result for the missing-input-file case** (returncode 1, with
an `.ini` produced) than what was reported to me — noting this as an
independent measurement, not a blocking discrepancy, since it doesn't
change the required fix and my own fresh measurement is what I built
against. More importantly, I found something not called out in the
finding: astap_cli's `.ini` carries a dedicated `ERROR=` field for
environment problems (e.g. `ERROR=No star database found.`) that is
absent from a genuine "no solution" `.ini` (verified against a real Moon
frame) — and the original `parse_ini` only ever read `WARNING=`, so this
field was being silently dropped even on the happy path where an `.ini`
*was* produced. Surfacing it turned out to be the highest-precision part
of the fix, on top of the returncode/stdout-tail capture the finding
asked for.

### Measurements against the real binary (before writing the fix)

```
$ cd /tmp/astap_diag_test && cp <NGC1893 frame> solve.fit
$ /opt/astap/astap_cli -f solve.fit -d /nonexistent-db-dir -r 180 -fov 0 -wcs
exit=1
solve.ini:
  PLTSOLVD=F
  CMDLINE=/opt/astap/astap_cli -f solve.fit -d /nonexistent-db-dir -r 180 -fov 0 -wcs
  ERROR=No star database found.
stdout: "Error, no star database found at /nonexistent-db-dir/ ! Download and install a star database."

$ /opt/astap/astap_cli -f nosuchfile.fit -d /opt/astap -r 180 -fov 0 -wcs
exit=1
nosuchfile.ini:
  PLTSOLVD=F
  ERROR=Error reading image file.

$ # real Moon frame, hint-based search (fast path)
$ /opt/astap/astap_cli -f solve.fit -d /opt/astap -ra 7.59781 -spd 115.5588 -r 30 -fov 0 -wcs
exit=1
solve.ini:
  PLTSOLVD=F
  CMDLINE=/opt/astap/astap_cli -f solve.fit -d /opt/astap -ra 7.59781 -spd 115.5588 -r 30 -fov 0 -wcs
  (no ERROR, no WARNING -- astap printed "No solution found!  :(" to stdout only)
```

This is what shaped the fix: `ERROR` present vs. absent in the `.ini`
itself is a cleaner, astap-native signal than parsing raw stdout, and it
cleanly separates the two exit-code-1 cases the finding was worried
about.

### The fix

1. `parse_ini` now returns `kv.get("ERROR") or kv.get("WARNING")` instead
   of just `kv.get("WARNING")` for a failed solve.
2. `solve_frame` captures the `subprocess.CompletedProcess` and, on any
   failure path (no `.ini` produced, or `.ini` produced but
   `solved: False`), appends a bounded (`DIAG_TAIL_CHARS = 300`)
   `"exit N: <stdout+stderr tail>"` string — combined with the `.ini`'s
   own `ERROR`/`WARNING` text when present, standing alone when it isn't
   (the genuine-no-solution case).
3. New `_preflight(cfg)`, called once at the top of `solve_fields` before
   the loop starts (and before `conn.row_factory` is touched): raises
   `RuntimeError` if `cfg.astap_bin` isn't an executable file, or if
   `cfg.astap_db_dir` isn't a directory. This is the actual protection
   against the poisoning scenario — it catches the whole misconfiguration
   class up front instead of discovering it 24,000 attempts later.
4. `solve_fields`' `LIMIT`/attempt-cap clause is now parameterised
   (`?` placeholders + a `params` list) instead of f-string-interpolated,
   per the review's Minor, for consistency with every other query in the
   module.

Explicitly did not touch: scratch-copy design, hint conversions
(`-ra`/`-spd`/`-r` formulas), `MAX_SOLVE_ATTEMPTS`, or timeout handling,
per team-lead's instruction — all three are untouched in the diff.

### TDD evidence

**RED** (re-verified for this fix round by `git stash push -- src/astrometa/solve.py`,
running the already-updated test file against the OLD `solve.py`, then
`git stash pop` to restore the fix):

```
$ git stash push --quiet -- src/astrometa/solve.py
$ .venv/bin/python -m pytest tests/test_solve.py -v
...
FAILED tests/test_solve.py::test_parse_ini_surfaces_error_field_on_failure
FAILED tests/test_solve.py::test_parse_ini_prefers_error_over_warning
FAILED tests/test_solve.py::test_solve_frame_no_ini_produced_is_recorded_as_unsolved
FAILED tests/test_solve.py::test_solve_frame_no_ini_produced_includes_exit_code_and_output
FAILED tests/test_solve.py::test_solve_frame_environment_failure_carries_ini_error_and_diagnostics
FAILED tests/test_solve.py::test_solve_frame_genuine_no_solution_carries_only_diagnostics
FAILED tests/test_solve.py::test_solve_fields_records_failure_without_marking_solved
FAILED tests/test_solve.py::test_solve_fields_raises_on_missing_astap_binary
FAILED tests/test_solve.py::test_solve_fields_raises_on_missing_astap_db_dir
========================= 9 failed, 12 passed in 1.41s =========================
$ git stash pop --quiet
```

Each failure is for the expected reason: the two `parse_ini` tests fail
because the old code never read `ERROR`; the `no_ini`/environment-failure/
genuine-no-solution `solve_frame` tests fail on the old exact-string
messages (`"no .ini produced"` vs. the new diagnostic-enriched text); the
two preflight tests fail with `DID NOT RAISE` because there was no
preflight check yet (one of them actually hit a raw `FileNotFoundError`
from `subprocess.run` trying to exec a nonexistent binary — exactly the
review's Minor about that unhandled exception).

**GREEN**:

```
$ .venv/bin/python -m pytest tests/test_solve.py -v
============================== 21 passed in 1.30s ==============================
$ .venv/bin/python -m pytest -v
============================== 81 passed in 1.49s ==============================
```

(21 tests in `tests/test_solve.py`, up from 14; full suite 81, up from
74, no regressions elsewhere.)

### New/changed tests

- `test_parse_ini_surfaces_error_field_on_failure`,
  `test_parse_ini_prefers_error_over_warning` — pure `parse_ini` checks,
  using the exact `.ini` shape measured against the real binary.
- `test_solve_frame_no_ini_produced_is_recorded_as_unsolved` — updated
  to assert the new `"no .ini produced (exit 0)"` format; docstring
  corrected to note this is a defensive fallback, not what the Moon case
  actually does (that was the original test's comment, and it was wrong
  per my measurement above).
- `test_solve_frame_no_ini_produced_includes_exit_code_and_output` (new)
  — stub crashes (prints to stdout/stderr, exits 139, writes no `.ini`);
  asserts the exit code and printed text both land in the warning.
- `test_solve_frame_environment_failure_carries_ini_error_and_diagnostics`
  (new) — stub reproduces the measured bad-database-dir `.ini` shape
  exactly (`PLTSOLVD=F`, `ERROR=No star database found.`, exit 1);
  asserts both the `ERROR` text and `"exit 1"` appear.
- `test_solve_frame_genuine_no_solution_carries_only_diagnostics` (new)
  — stub reproduces the measured genuine-Moon-failure shape exactly
  (`PLTSOLVD=F`, no `ERROR`/`WARNING`, exit 1); asserts the warning is
  diagnostics-only (starts with `"exit 1"`), confirming this case stays
  distinguishable from the `ERROR` case above.
- `test_solve_fields_records_failure_without_marking_solved` — updated
  exact-match assertion to `"no .ini produced (exit 0)"`.
- `test_solve_fields_raises_on_missing_astap_binary`,
  `test_solve_fields_raises_on_missing_astap_db_dir` (new) — assert
  `pytest.raises(RuntimeError, match=...)` AND that no field's
  `solve_attempts`/`solve_source` changed, i.e. the preflight raise
  happens strictly before any field is touched.
- `_cfg()` test helper now creates `astap_db_dir` (was passed as a
  never-created path, which the new preflight would otherwise reject in
  every existing `solve_fields` test).

None of these assert on the stub's echoed input alone — each reproduces
a specific `.ini`/exit-code/output shape actually measured against
`/opt/astap/astap_cli` and asserts on `solve_frame`'s/`solve_fields`'s
real return value or DB state.

### Files changed (this round)

- `src/astrometa/solve.py`
- `tests/test_solve.py`

Commit: `6a9eecb fix: distinguish astap_cli environment failures from unsolvable frames`
