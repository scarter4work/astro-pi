# Task 12 Report: Manifest export and import

## Summary

Implemented `src/astrometa/manifest.py` (`export_dir`, `export_all`, `import_file`) and
`tests/test_manifest.py`, following the task brief with both corrections applied.

## What I implemented

- `export_dir(conn, leaf_dir, out_dir=None) -> Path` — selects a leaf directory's frames
  from `frames` via an **escaped** `LIKE` prefix and writes a `.astro-manifest.json`
  sidecar (into `out_dir` if given, else into `leaf_dir` itself). Raises `OSError`,
  naming both the leaf directory and the destination path, on any write failure.
- `export_all(conn, out_dir=None, dry_run=False) -> int` — exports every leaf directory
  referenced by `frames`. `dry_run=True` counts without touching the filesystem. When
  `out_dir` is given, each leaf's manifest is written beneath a **mirror** of that leaf's
  full directory structure under `out_dir` (via a new private `_mirror_leaf_dir` helper),
  never flattened. Propagates write failures from `export_dir` unchanged (no
  try/except swallowing) — a partial run never completes silently.
- `import_file(conn, manifest_path) -> int` — reconstructs `frames` rows from one
  manifest via an upsert keyed on `content_hash` (`ON CONFLICT(content_hash) DO UPDATE`),
  so a frame's identity is never its filename and re-importing never duplicates rows.

## Correction 1 — LIKE escaping

Added `_like_escape(s)`, which escapes `\`, `%`, and `_` in the leaf-dir prefix before
it's interpolated into the pattern; the query pairs it with `ESCAPE '\'`:

```python
pattern = _like_escape(str(leaf_dir)) + "/%"
rows = conn.execute(
    f"SELECT {', '.join(_FIELDS)} FROM frames WHERE path LIKE ? ESCAPE '\\'",
    (pattern,)).fetchall()
```

Only the leaf-dir portion is escaped; the trailing `/%` wildcard is appended
unescaped afterward, so it still matches "everything under this directory".

**Proof the bug is real** (ad hoc, not part of the test suite, run during self-review):
built two sibling dirs `astro_data/Sh2-106` and `astroXdata/Sh2-106` (same length,
differing only where the underscore sits) with one frame each, and ran the *naive*
`f"{leaf}/%"` pattern directly against the DB — it matched **both** frames (`h1` and
`h2`). The shipped `export_dir` matches only `h1`.

Test: `test_like_escape_rejects_underscore_wildcard_sibling`.

## Correction 2 — export_all + read-only archive

- `export_all` now accepts and propagates `out_dir` (signature exactly as specified in
  the correction: `export_all(conn, out_dir=None, dry_run=False)`).
- Mirrored layout: `_mirror_leaf_dir(leaf_dir, out_dir)` strips the leaf directory's
  anchor (`/`) and rejoins the remaining parts under `out_dir`, e.g. leaf
  `/archive/2026-09-08/Sh2-106` with `out_dir=/data/manifests` becomes
  `/data/manifests/archive/2026-09-08/Sh2-106/.astro-manifest.json`. This preserves the
  **full** leaf path (not just the basename), so two leaves that share a basename under
  different parents can never collide.
- Write failures raise loudly: `export_dir`'s `except OSError as e: raise OSError(...)
  from e` names the leaf directory and destination path; `export_all` does not catch
  anything, so the exception propagates as-is to the caller. No directory is ever
  skipped silently.

Tests: `test_export_all_mirrors_leaf_structure_under_out_dir`,
`test_export_all_out_dir_avoids_basename_collision` (two leaves with identical
basename `Sh2-106` under different date directories — both manifests survive, each
`leaf_dir` field distinct), `test_export_dir_raises_loudly_on_write_failure`,
`test_export_all_raises_and_names_the_failing_leaf` (both use `os.chmod` on a real
directory to induce a genuine `PermissionError`, not a mock).

## TDD evidence

**RED** — `.venv/bin/python -m pytest tests/test_manifest.py -v`, before
`src/astrometa/manifest.py` existed:

```
ImportError: cannot import name 'manifest' from 'astrometa'
```

Expected, since the module didn't exist yet.

**GREEN** — same command after implementation:

```
collected 12 items
tests/test_manifest.py::test_export_writes_manifest PASSED
tests/test_manifest.py::test_roundtrip_reconstructs_labels_after_db_loss PASSED
tests/test_manifest.py::test_filename_with_space_survives_roundtrip PASSED
tests/test_manifest.py::test_importing_twice_does_not_duplicate_rows PASSED
tests/test_manifest.py::test_exporting_twice_produces_the_same_manifest PASSED
tests/test_manifest.py::test_like_escape_rejects_underscore_wildcard_sibling PASSED
tests/test_manifest.py::test_export_all_writes_into_each_leaf_by_default PASSED
tests/test_manifest.py::test_export_all_dry_run_counts_without_writing PASSED
tests/test_manifest.py::test_export_all_mirrors_leaf_structure_under_out_dir PASSED
tests/test_manifest.py::test_export_all_out_dir_avoids_basename_collision PASSED
tests/test_manifest.py::test_export_dir_raises_loudly_on_write_failure PASSED
tests/test_manifest.py::test_export_all_raises_and_names_the_failing_leaf PASSED
12 passed in 0.02s
```

Full suite: `.venv/bin/python -m pytest -q` → `164 passed in 1.95s` (152 pre-existing +
12 new).

## Self-review

- **Test honesty check**: temporarily stripped the `except OSError: raise OSError(...)`
  wrapping from `export_dir` and re-ran the two write-failure tests — both failed as
  expected (the raw `PermissionError` message doesn't contain the leaf directory's path
  in either case, since the failing mkdir path is the destination/mirror path, not the
  leaf path). Restored the file (verified byte-identical to the committed version via
  `diff`) and confirmed the full suite is green again (164 passed). This confirms the
  two failure tests are exercising real, load-bearing behavior, not asserting on
  incidental substring overlap.
- **Row factory discipline**: `export_dir` saves/restores `conn.row_factory` around its
  `SELECT`, matching `solve.py`/`quality.py`/`grouping.py`. `export_all` and
  `import_file` never mutate `row_factory` (they don't need named-column access), so no
  save/restore needed there.
- **YAGNI**: no CLI entry point was added (not asked for by this task's interface list).
  `_mirror_leaf_dir` and `_like_escape` are private helpers scoped to this module only.
- **Global constraints**: no test touches `/mnt/qnap`, `/archive`, or `/live` — every
  test uses `tmp_path`. No FITS data is ever written or moved by this module. Filenames
  with spaces (`NGC 7635`) round-trip exactly (brief's own test, unmodified). Identity
  is `content_hash` throughout (`frames.content_hash` is the table's `PRIMARY KEY`, and
  `import_file`'s upsert keys on it, never on filename).
- **Python version**: no post-3.13 syntax used; `X | None` annotations are the same
  style already used throughout the codebase (`grouping.py`, `disposition.py`) and work
  at runtime back to 3.10.

## Files changed

- `src/astrometa/manifest.py` (new)
- `tests/test_manifest.py` (new)

## Concerns

None. Both corrections are implemented and independently regression-tested; the write
side (`export_dir`/`export_all`) never touches FITS data, only the sanctioned manifest
sidecar, consistent with the "never write to the archive" ruling as clarified by the
team lead.

---

## Fix round 1 (review response)

Review came back Approved with one Important finding and two Minors, all addressed in
commit `8a9a3ac`.

### Important — `_mirror_leaf_dir` only defended the absolute case

`_mirror_leaf_dir` stripped the anchor for an absolute `leaf_dir`, but a relative
`leaf_dir` containing `..` segments kept them literally in `parts`, and
`out_dir.joinpath(*parts)` does not collapse them. `export_dir` then `mkdir`s whatever
path comes back, so a crafted or buggy caller (the Task 13 CLI wrapper was named as an
upcoming new caller) could make `export_all` create directories outside `out_dir`.

**Fix**: `_mirror_leaf_dir` now resolves both `out_dir` and the computed mirror target
(`Path.resolve()`, which collapses `..` and symlinks) and checks containment with
`Path.is_relative_to` — not string-prefix matching, which a sibling directory like
`out_dir_evil` would defeat. A target that resolves outside `out_dir` raises
`ValueError`, naming both the original `leaf_dir` and where it actually resolved to,
before any `mkdir`/write is attempted. The function now returns the *resolved* target
(not the raw joined one still containing literal `..` segments), which also avoids
leaving stray intermediate directories behind as a side effect of the OS traversing
through a literal `..` path component.

**Proof the bug was real, not hypothetical** (RED evidence below): before the fix, a
crafted absolute frame path with six leading `../` segments caused `export_dir` to
attempt `mkdir` at `/escape` — true filesystem root — and it only failed in this
sandbox because the test user lacks permission to write to `/`. Run as root, or with a
smaller `..` count against a writable path, this would have silently created a real
directory outside `out_dir`.

Two new tests, both using only `tmp_path` (no real archive path touched):
- `test_export_all_rejects_relative_leaf_dir_that_escapes_out_dir` — a relative leaf
  dir (`"somedir/../../escape/..."`) that walks back out of `out_dir`.
- `test_export_all_rejects_leaf_dir_that_resolves_outside_out_dir` — an absolute leaf
  dir carrying more `..` than the anchor-strip accounts for (`"/" + "../"*6 +
  "escape/..."`), demonstrating the escape is not limited to relative inputs.

Both assert `pytest.raises(ValueError, match="out_dir")` and that nothing was created
anywhere (`list(out_dir.rglob(MANIFEST_NAME)) == []`, plus explicit checks that no
stray directory was left behind).

**RED** — run against the pre-fix code (`_mirror_leaf_dir` with no resolve/containment
check):

```
$ .venv/bin/python -m pytest tests/test_manifest.py::test_export_all_rejects_relative_leaf_dir_that_escapes_out_dir tests/test_manifest.py::test_export_all_rejects_leaf_dir_that_resolves_outside_out_dir -v
...
E           OSError: failed to write manifest for leaf directory /../../../../../../escape to /tmp/pytest-of-scarter4work/pytest-68/test_export_all_rejects_leaf_d0/manifests/../../../../../../escape/.astro-manifest.json: [Errno 13] Permission denied: '/tmp/pytest-of-scarter4work/pytest-68/test_export_all_rejects_leaf_d0/manifests/../../../../../../escape'
FAILED tests/test_manifest.py::test_export_all_rejects_relative_leaf_dir_that_escapes_out_dir
FAILED tests/test_manifest.py::test_export_all_rejects_leaf_dir_that_resolves_outside_out_dir
2 failed in 0.05s
```

Failed for the expected reason: no `ValueError` was raised — the code proceeded all the
way to a real `mkdir` attempt at `/escape`, outside `out_dir` and outside `tmp_path`
entirely, only stopped by the OS's own permission check, not by any guard in our code.

**GREEN** — after the fix:

```
$ .venv/bin/python -m pytest tests/test_manifest.py -v
collected 14 items
tests/test_manifest.py::test_export_writes_manifest PASSED
tests/test_manifest.py::test_roundtrip_reconstructs_labels_after_db_loss PASSED
tests/test_manifest.py::test_filename_with_space_survives_roundtrip PASSED
tests/test_manifest.py::test_importing_twice_does_not_duplicate_rows PASSED
tests/test_manifest.py::test_exporting_twice_produces_the_same_manifest PASSED
tests/test_manifest.py::test_like_escape_rejects_underscore_wildcard_sibling PASSED
tests/test_manifest.py::test_export_all_writes_into_each_leaf_by_default PASSED
tests/test_manifest.py::test_export_all_dry_run_counts_without_writing PASSED
tests/test_manifest.py::test_export_all_mirrors_leaf_structure_under_out_dir PASSED
tests/test_manifest.py::test_export_all_out_dir_avoids_basename_collision PASSED
tests/test_manifest.py::test_export_all_rejects_relative_leaf_dir_that_escapes_out_dir PASSED
tests/test_manifest.py::test_export_all_rejects_leaf_dir_that_resolves_outside_out_dir PASSED
tests/test_manifest.py::test_export_dir_raises_loudly_on_write_failure PASSED
tests/test_manifest.py::test_export_all_raises_and_names_the_failing_leaf PASSED
14 passed in 0.02s
```

All previously-passing tests (mirroring, basename-collision avoidance, write-failure
naming) still pass — confirms the fix doesn't regress the collision-avoidance layout or
loud-failure behaviour the review explicitly said not to touch.

### Minor 1 — `export_all` non-atomicity undocumented

Added a paragraph to `export_all`'s docstring stating explicitly that it aborts on the
first failure (not atomic): manifests already written for leaves processed earlier in
the same call remain on disk, by design, so a raised `export_all` should be read as
"some real manifests exist, some leaves are not yet done" rather than "nothing
happened".

### Minor 2 — unexplained field exclusion in idempotency test

Added a comment to `test_exporting_twice_produces_the_same_manifest` explaining that it
compares only `doc["frames"]`, not the whole document, because `generated_at` is a
wall-clock timestamp that legitimately differs between the two `export_dir` calls —
comparing the whole document would make the test flaky (or invite "fixing" it by
freezing time instead of narrowing the comparison to what idempotency actually
promises).

### Full suite after the fix

```
$ .venv/bin/python -m pytest -q
........................................................................ [ 43%]
........................................................................ [ 86%]
......................                                                   [100%]
166 passed in 1.97s
```

166 passed (164 previous + 2 new traversal-guard tests).

### Files changed (fix round 1)

- `src/astrometa/manifest.py` — `_mirror_leaf_dir` containment check; `export_all`
  docstring non-atomicity note.
- `tests/test_manifest.py` — two new traversal tests; one clarifying comment.

### Commit

`8a9a3ac` "fix: guard manifest mirror path against out_dir traversal"

### Concerns

None. The three items from review are closed: the traversal guard is proven both by a
RED/GREEN cycle and by a concrete demonstration that the pre-fix code reached a real
`mkdir("/escape")` attempt at filesystem root; the two documentation minors are one-line
additions with no behavioral change (confirmed by the full suite staying green and by
the untouched LIKE-escaping, basename-collision, and write-failure tests all continuing
to pass unmodified).
