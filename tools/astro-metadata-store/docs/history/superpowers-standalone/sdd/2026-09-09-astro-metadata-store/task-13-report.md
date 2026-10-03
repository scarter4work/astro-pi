# Task 13: CLI — Report

## What I implemented

- `src/astrometa/cli.py` — an `argparse`-based CLI with subcommands
  `scan`, `cluster`, `solve`, `measure`, `group`, `manifest`, `status`.
  Each subcommand is its own `argparse` subparser inheriting a shared
  `--db` (required) from a common parent parser, plus its own flags:
  `--root` (repeatable, `scan`), `--limit` (`solve`, `measure`),
  `--out-dir` (`manifest`). No `quarantine` subcommand, per the global
  constraint that this tool never touches FITS data.
- `pyproject.toml` — added `[project.scripts] astrometa = "astrometa.cli:main"`.
- `tests/test_cli.py` — 13 tests, all using `tmp_path`.

`main(argv) -> int` parses args, builds `Config()`, connects/inits the
db, dispatches on `args.command`, and returns 0 on success. Argparse's
own usage errors (`SystemExit`, e.g. unknown subcommand or missing
`--db`) are caught and translated into a plain non-zero return.
Everything else — `FileNotFoundError` from `inventory.scan`,
`ValueError` from `disposition.mark_missing`, `RuntimeError` from
`solve.solve_fields`/`quality.measure_frames` on a broken astap_cli
install — is left uncaught, per the instruction that these must surface
loudly as a real exception and non-zero exit, not a tidy message.

## Brief call sites I corrected against the real interfaces

The brief's sample `cli.py` was written before Tasks 1–12 ran. I did
not use it as a call-site reference at all beyond the subcommand list
and the `--db`/`--root`/`--limit` flag shape; every actual call was
written from the real module signatures I read first. Concretely, vs.
the brief's sample:

1. `cluster.assign_fields(conn)` — brief printed `fields={...}`. The
   real function returns fields **created this call**, not the total
   (confirmed from its own docstring: "Returns the number of NEW
   fields created by this call (0 on a re-run...)"). I print
   `fields_created={created}` so a re-run's `0` reads as "nothing new"
   rather than "no fields exist" — tested by
   `test_cluster_labels_newly_created_fields` (asserts `fields_created=1`
   then `fields_created=0` on immediate re-run).

2. `quality.measure_frames(conn, cfg, limit)` — brief printed a single
   `measured={...}`. The real function returns
   `QualityResult(measured, failed)` (a dataclass). I print
   `measured={res.measured} failed={res.failed}` — tested by
   `test_measure_reports_measured_and_failed`.

3. `inventory.scan(conn, roots)` — brief's `res.added`/`res.updated`/
   `res.failed` were treated as if disjoint. The real `InventoryResult`
   has `failed` **overlapping** `added`/`updated` (a frame that fails a
   read is still inventoried, per the module's own comment on the
   INSERT statement). I kept the same four numbers but appended an
   explicit parenthetical: `(failed overlaps added/updated: a frame
   that fails a read is still inventoried, with its failure recorded)`
   — tested by `test_scan_reports_failed_overlapping_added_not_disjoint`,
   which forces one unreadable frame alongside one good frame and
   asserts `added=2 failed=1` plus the word "overlap" in the output.

4. `manifest.export_all(conn, out_dir=None, dry_run=False)` — brief
   called it with no `out_dir`. I added a `--out-dir` CLI option and
   pass it through (`Path(args.out_dir) if args.out_dir else None`) —
   tested by `test_manifest_out_dir_mirrors_leaf_structure`, which
   scans a frame, runs `manifest --out-dir <dir>`, and asserts the
   `.astro-manifest.json` lands under the mirrored `out_dir` tree and
   nothing is written beside the source frame.

5. Loud-failure passes: brief's sample had no exception handling around
   business logic at all (it would have propagated anyway, since the
   sample never wrapped these calls in try/except) — I kept that same
   shape deliberately, and added tests proving it: `test_scan_missing_root_raises_loudly`
   (`FileNotFoundError` from a nonexistent `--root`),
   `test_rescan_against_empty_root_raises_loudly` (`ValueError` from
   `mark_missing` refusing an empty-but-present-frames rescan),
   `test_solve_missing_astap_raises_loudly` and
   `test_measure_missing_astap_raises_loudly` (`RuntimeError` from
   `_preflight` when `astap_bin`/`astap_db_dir` don't exist — done via
   `monkeypatch.setattr(cli, "Config", lambda: fake_cfg)` since the CLI
   itself has no config-override flags and I didn't add any, to avoid
   scope creep the brief and interface notes didn't ask for).

## Testing

`.venv/bin/python -m pytest tests/test_cli.py -v` — 13 passed.
`.venv/bin/python -m pytest tests/ -q` — **179 passed** (166 prior +
13 new), full suite, no regressions.

All tests use `tmp_path` for the db and every scanned root; none touch
`/mnt/qnap`, `/archive`, or `/live`. `solve`/`measure` tests use a
stub `astap_cli` script (same pattern as `tests/test_solve.py` /
`tests/test_quality.py`: a small executable written to `tmp_path` that
writes a solved `.ini` or prints `HFD_MEDIAN=`/`STARS=` lines) injected
via `monkeypatch.setattr(cli, "Config", lambda: fake_cfg)`, so no real
external `astap_cli` install is required for the CLI tests to be
deterministic (this dev box happens to have one at `/opt/astap/astap_cli`,
but the tests don't depend on that).

### TDD evidence

**RED** — before `cli.py` existed:
```
$ .venv/bin/python -m pytest tests/test_cli.py -v
ImportError while importing test module '.../tests/test_cli.py'.
tests/test_cli.py:8: in <module>
    from astrometa import cli, manifest
E   ImportError: cannot import name 'cli' from 'astrometa'
```
Expected and correct: `src/astrometa/cli.py` did not exist yet.

**GREEN** — after implementing `cli.py` and the `pyproject.toml` entry point:
```
$ .venv/bin/python -m pytest tests/test_cli.py -v
tests/test_cli.py::test_scan_then_status PASSED
tests/test_cli.py::test_unknown_command_returns_nonzero PASSED
tests/test_cli.py::test_missing_db_flag_returns_nonzero PASSED
tests/test_cli.py::test_scan_missing_root_raises_loudly PASSED
tests/test_cli.py::test_rescan_against_empty_root_raises_loudly PASSED
tests/test_cli.py::test_scan_reports_failed_overlapping_added_not_disjoint PASSED
tests/test_cli.py::test_cluster_labels_newly_created_fields PASSED
tests/test_cli.py::test_group_reports_project_count PASSED
tests/test_cli.py::test_manifest_out_dir_mirrors_leaf_structure PASSED
tests/test_cli.py::test_solve_reports_solved_count PASSED
tests/test_cli.py::test_measure_reports_measured_and_failed PASSED
tests/test_cli.py::test_solve_missing_astap_raises_loudly PASSED
tests/test_cli.py::test_measure_missing_astap_raises_loudly PASSED
13 passed in 0.18s

$ .venv/bin/python -m pytest tests/ -q
179 passed in 2.01s
```

### Console script verification

```
$ .venv/bin/pip install -e . -q
$ ls .venv/bin | grep astrometa
astrometa
$ .venv/bin/astrometa --help
usage: astrometa [-h] {scan,cluster,solve,measure,group,manifest,status} ...
  scan       walk archive roots, inventory frames, then mark absent frames missing
  cluster    cluster unassigned light frames into fields
  solve      plate-solve one representative frame per unsolved field
  measure    measure HFD/star-count quality for unmeasured light frames
  group      rebuild projects from light frames
  manifest   export a manifest sidecar for every leaf directory
  status     print row counts for the core tables
```

Also ran an end-to-end smoke test of the *installed* binary (not just
`cli.main()` called in-process) against a tmp scratch dir: wrote one
synthetic light frame, ran `astrometa scan --db ... --root ...` (got
`added=1 updated=0 failed=0 missing=0 (...)`), ran `astrometa status`
(`frames: 1`, etc.), and ran `astrometa nonsense --db ...` to confirm
exit code 2 with a real argparse usage error. All matched the pytest
behavior exactly.

## Files changed

- `src/astrometa/cli.py` (new)
- `tests/test_cli.py` (new)
- `pyproject.toml` (added `[project.scripts]`)

Commit: `8ff3dcb` — "feat: command-line interface"

## Self-review (fresh eyes)

- **Completeness**: all 7 required subcommands present; `--db` shared;
  `--root`/`--limit`/`--out-dir` on the right subcommands only; console
  script entry point added and verified both via pytest-level
  `cli.main()` calls and via the actually-installed `astrometa` binary.
- **Quality**: every imported pass module (`cluster`, `db`,
  `disposition`, `grouping`, `inventory`, `manifest`, `quality`,
  `solve`) is referenced exactly once in `cli.py` (twice for `db`) —
  no dead imports. `py_compile` clean on both files. No shadowing
  between the `manifest` module import and the `manifests` local
  variable.
- **YAGNI**: deliberately did NOT add a `--fp-threshold` override for
  `cluster` or a `--dry-run` flag for `manifest` — neither was in the
  brief's command list or the interface-change notes, and both would
  be scope creep past what was asked. Did not add config-override
  flags (e.g. `--astap-bin`) to the CLI surface for the same reason;
  test coverage for the loud-failure RuntimeError paths uses
  `monkeypatch` on `cli.Config` instead, which needs no new CLI
  surface.
- **Test honesty**: every test constructs its own `tmp_path` archive
  root and db; nothing reads or writes `/mnt/qnap`, `/archive`, or
  `/live`. The two "loud failure" tests for `solve`/`measure` assert
  `RuntimeError` is raised with a config pointing at a nonexistent
  binary — a genuine negative-path test, not a rigged assertion. RED
  was captured before GREEN, with the actual `ImportError` output
  showing the expected reason (module didn't exist yet).

## Concerns

- None blocking. One deliberate design choice worth flagging for
  review: `--db` (and every subcommand-specific flag) must be given
  **after** the subcommand name (e.g. `astrometa scan --db x.sqlite`),
  not before (`astrometa --db x.sqlite scan` does not work) — this
  follows directly from using `argparse` subparsers with a shared
  parent, and matches the exact invocation order used in the task
  brief's own sample tests (`["scan", "--db", ...]`), so I did not
  treat it as a gap.
- Task 14 (reconciliation) will read the `scan` output's
  `added`/`updated`/`failed`/`missing` numbers — I made the overlap
  between `failed` and `added`/`updated` explicit in the printed
  output text itself (not just in a comment), since that's the number
  a human operator running the CLI directly will see.
