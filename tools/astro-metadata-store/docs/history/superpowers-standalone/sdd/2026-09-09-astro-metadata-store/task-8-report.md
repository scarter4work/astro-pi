# Task 8 report: quality measurement

## What I implemented

`src/astrometa/quality.py`:

- `parse_analyse(stdout: str) -> dict` — extracts `HFD_MEDIAN`/`STARS` from
  `astap_cli -analyse` stdout via the two regexes from the brief, tolerant of
  extra surrounding lines. Returns `None` for either field when absent.
- `measure_frame(cfg, frame_path) -> dict` — copies the frame into
  `cfg.scratch_dir` (never touches the archive), runs
  `astap_cli -f <scratch copy> -analyse` with `TIMEOUT_S = 60`, and returns
  `{"hfd_median", "star_count", "ok", "error"}`. `ok` is `True` only when the
  process exited 0 **and** both metrics parsed from stdout — that combination
  is what distinguishes a genuine measurement (including a legitimate
  "zero stars found") from a failure. On failure it folds in astap_cli's own
  `.ini` `ERROR=` line (measured: `-analyse` *does* write an `.ini` on a
  file-access failure even though a successful run writes none at all) plus
  a bounded exit-code/output-tail diagnostic.
- `measure_frames(conn, cfg, limit=None) -> QualityResult` — selects
  candidates via `LEFT JOIN quality q ON q.content_hash=f.content_hash WHERE
  f.frame_type='light' AND f.read_error IS NULL AND q.content_hash IS NULL`,
  calls `_preflight(cfg)` first, and for each candidate either inserts a
  `quality` row (genuine measurement) or counts it as `failed` and writes
  nothing.
- `QualityResult` — `@dataclass` with `measured: int = 0` and
  `failed: int = 0`, mirroring the existing `InventoryResult` precedent in
  `inventory.py` rather than inventing a new return-shape convention.

`tests/test_quality.py` — 17 tests (3 from the brief verbatim, 14 added for
the corrections and the module's own failure modes), plus its own
self-contained stub `astap_cli` (env-var driven: `solved`, `no_metrics`,
`error_ini`, `crash`, `sleep`) built the same way `test_solve.py`'s stub is,
and a `skipif`-guarded test against the real `/opt/astap/astap_cli`.

## Real-binary probing (before writing any code)

Copied a real archive frame to scratch and ran the actual binary to pin down
exact behaviour rather than trust the brief's claims blindly:

```
$ /opt/astap/astap_cli -f frame.fit -analyse
HFD_MEDIAN=4.8
STARS=229
exit=0
```
No sidecar file was written — confirms `-analyse` (unlike a solve) succeeds
with stdout-only output.

```
$ /opt/astap/astap_cli -f frame.fit -d /nonexistent-dir -analyse
HFD_MEDIAN=4.8
STARS=229
exit=0
```
Confirms `-analyse` doesn't consume `-d` at all — no database needed for star
detection, only for a full solve.

```
$ echo "not a fits file" > garbage.fit
$ /opt/astap/astap_cli -f garbage.fit -analyse
Error, accessing the file!
exit=1
$ cat garbage.ini
PLTSOLVD=F
CMDLINE=/opt/astap/astap_cli -f garbage.fit -analyse
ERROR=Error reading image file.
```
Confirms the exact failure shape the corrections warn about: exit 1, no
metrics on stdout, but an `.ini` **is** written with an `ERROR=` field even
though `-analyse` writes nothing on success. This directly shaped
`measure_frame`'s failure path.

```
$ /opt/astap/astap_cli -f bias.fit -analyse   # a real bias frame, no stars
HFD_MEDIAN=21.5
STARS=0
exit=0
```
Confirms the "legitimate zero-star measurement" case from Correction 2 is
real, not hypothetical: exit 0, both metrics present, `STARS=0`. This is
`ok: True` in `measure_frame` and gets written normally.

## TDD evidence

RED — `.venv/bin/python -m pytest tests/test_quality.py -v` before
`src/astrometa/quality.py` existed:
```
ImportError: cannot import name 'quality' from 'astrometa'
```
Failed for the expected reason (module doesn't exist yet), 1 collection
error, 0 tests run.

GREEN — same command after implementation:
```
tests/test_quality.py::test_parse_analyse_extracts_metrics PASSED
tests/test_quality.py::test_parse_analyse_tolerates_extra_lines PASSED
tests/test_quality.py::test_parse_analyse_on_garbage_returns_none PASSED
tests/test_quality.py::test_measure_frame_stub_solver_leaves_source_directory_clean PASSED
tests/test_quality.py::test_measure_frame_zero_stars_is_still_a_genuine_measurement PASSED
tests/test_quality.py::test_measure_frame_unparseable_output_is_a_failure_not_a_measurement PASSED
tests/test_quality.py::test_measure_frame_crash_carries_exit_code_and_output PASSED
tests/test_quality.py::test_measure_frame_environment_failure_carries_ini_error PASSED
tests/test_quality.py::test_measure_frame_records_timeout_loudly PASSED
tests/test_quality.py::test_measure_frame_against_real_astap PASSED
tests/test_quality.py::test_measure_frames_writes_quality_row_for_a_genuine_measurement PASSED
tests/test_quality.py::test_measure_frames_only_considers_light_frames_without_read_error PASSED
tests/test_quality.py::test_measure_frames_is_idempotent PASSED
tests/test_quality.py::test_measure_frames_failure_writes_no_row_and_stays_a_candidate PASSED
tests/test_quality.py::test_measure_frames_respects_limit PASSED
tests/test_quality.py::test_measure_frames_raises_on_missing_astap_binary PASSED
tests/test_quality.py::test_measure_frames_raises_on_missing_astap_db_dir PASSED
============================== 17 passed in 0.48s ==============================
```

Full suite before commit — `.venv/bin/python -m pytest -v`:
```
============================== 98 passed in 1.90s ==============================
```
(81 pre-existing + 17 new, no regressions.)

## What I reused from solve.py vs wrote fresh

Reused directly (imported, not copied):
- `solve._preflight(cfg)` — called as the very first thing in
  `measure_frames`, exactly as Correction 1 asked.
- `solve._diagnostics(result)` — the bounded exit-code/stdout/stderr-tail
  formatter, used in `measure_frame`'s failure path. Required calling
  `subprocess.run` without `text=True` (bytes stdout/stderr) to match its
  expected input type, same as `solve_frame` does.

Wrote fresh (genuinely different behaviour, not a copy of solve.py's shape):
- `parse_analyse` / `_ini_error` — `-analyse` output parsing is regex-based
  stdout parsing, nothing like `solve.parse_ini`'s key=value `.ini` parsing.
- `measure_frame`'s success/failure boundary — solve.py's success signal is
  `PLTSOLVD=T` in an `.ini` that's *always* written; `-analyse`'s success
  signal is exit-0-plus-both-stdout-metrics, and the `.ini` only exists on
  failure. These are different enough shapes that sharing one function would
  have forced an awkward abstraction over two different sidecar-presence
  semantics — not worth it for ~15 lines.
- `measure_frames`'s candidate query and write path — no per-item retry cap
  (unlike `solve_fields`' `MAX_SOLVE_ATTEMPTS`); a failed frame just stays a
  candidate indefinitely by construction (no row written), which is the
  entire point of Correction 2.

## The return shape I chose for measure_frames, and why

`QualityResult(measured: int, failed: int)`, a `@dataclass`, replacing the
brief's bare `-> int`. I looked for an existing precedent before inventing a
shape: `inventory.py` already has exactly this pattern —
`InventoryResult(added, updated, failed, seen_hashes)` returned from `scan()`
— so `QualityResult` follows the same convention rather than introducing a
tuple or dict return that would be a one-off in this codebase. A caller can
now tell "nothing left to measure" (`measured=0, failed=0`) apart from
"N frames are stuck failing every run" (`measured=0, failed=N`), which a
single int could never distinguish and which is exactly the operational
signal Correction 2 exists to preserve.

## Files changed

- `src/astrometa/quality.py` (new)
- `tests/test_quality.py` (new)

Commit: `a180a1d feat: per-frame quality measurement via astap -analyse`

## Self-review findings

- Re-read `quality.py` end to end against both corrections line by line —
  both are implemented and each has a dedicated test
  (`test_measure_frames_raises_on_missing_astap_binary`/`_db_dir` for #1,
  `test_measure_frames_failure_writes_no_row_and_stays_a_candidate` for #2,
  which also asserts the retry actually succeeds on a second run once the
  stub is switched back to `solved`).
  Also both are covered at the `measure_frame` level directly, not just via
  `measure_frames` end to end (e.g.
  `test_measure_frame_unparseable_output_is_a_failure_not_a_measurement`).
- No unused imports, no dead code paths.
- YAGNI check: I deliberately did not add a `-d`/database argument to the
  `astap_cli` invocation, matching both the brief's example and the measured
  real behaviour that `-analyse` ignores it. `_preflight` still checks
  `astap_db_dir` per Correction 1's explicit instruction to reuse it as-is —
  this checks something `-analyse` doesn't strictly need, but preflighting
  it anyway costs nothing and keeps the two astap wrappers' failure modes
  predictable and symmetric.
- No retry cap analogous to `solve_fields`' `MAX_SOLVE_ATTEMPTS` — correct
  per the task: Correction 2 explicitly wants failed frames to always stay
  candidates (unlike a genuinely unsolvable field, a failed *measurement* has
  no "permanently give up" state described anywhere in the brief or
  corrections, so I didn't invent one).
- Test honesty: every failure-path test asserts on the actual error string
  content (exit code, `.ini` ERROR text, "timeout"), not just `ok is False`,
  so a regression that silently drops the diagnostic would fail these tests.

## Concerns

- `measure_frame`/`measure_frames` import two underscore-prefixed
  "private" names from `solve.py` (`_preflight`, `_diagnostics`). This is
  exactly what Correction 1 asked for ("reuse it rather than writing a
  second copy"), and it's the same package so it's not a real encapsulation
  violation, but it does mean `quality.py` is now coupled to `solve.py`'s
  internals — a rename or refactor of either function in a future task would
  need to check this module too. If a third module ever needs the same
  preflight/diagnostics behaviour, promoting both to a small shared
  `astap.py` helper module (not owned by either task) would be the cleaner
  fix; I didn't do that here since it would mean touching Task 7's
  already-reviewed module, which is out of scope for this task.
- No behaviour, other than the two named corrections, differs from the
  brief — I did not find anything else in the brief that looked wrong
  against Task 7's patterns or the measured real-binary behaviour.
