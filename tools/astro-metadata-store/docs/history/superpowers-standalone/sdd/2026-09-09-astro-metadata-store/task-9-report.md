# Task 9 report: Object naming and identity assertions

## What I implemented

- `src/astrometa/naming.py`
  - `CONFIDENCE: dict[str, str]` — maps `solved`/`propagated` → `high`, `manual` → `authoritative`, `object_card` → `medium`, `dirname` → `low`. Verbatim from the brief.
  - `may_drive_move(source: str) -> bool` — True only for `{"solved", "propagated", "manual"}`. This is the confidence gate: a future task will call it before physically moving a file on disk.
  - `canonicalise(name: str) -> str` — folds mechanical spelling variants of a catalogue designator (spacing, case, missing/extra hyphen) to one string via a single regex over `M|NGC|IC|SH2|LDN|LBN|B|VDB`. A name that doesn't match (a proper name like "Triangulum Galaxy") is returned stripped, otherwise unchanged. Verbatim from the brief.
  - `upsert_object(conn, canonical, object_type, simbad_id=None, ra=None, dec=None) -> int` — `INSERT ... ON CONFLICT(canonical_name) DO UPDATE`, idempotent on `canonical_name` (which is `UNIQUE` in the schema). One deliberate change from the brief's sample: see "Fix beyond the brief's sample code" below.
  - `add_alias(conn, object_id, alias, source) -> None` — `INSERT OR IGNORE`, idempotent on the `aliases` table's `(object_id, alias)` primary key. Verbatim from the brief.
  - `assert_identity(conn, field_id, object_id, source) -> None` — always records the claim in `identity_assertions` with `CONFIDENCE[source]`; only updates `fields.object_id` when `may_drive_move(source)` is True. Verbatim from the brief.
- `tests/test_naming.py` — the brief's Step 1 tests, kept verbatim (9 parametrized `canonicalise` cases + 5 standalone tests = 14 tests total, matching your corrected count, not the brief's stated 16).

## TDD evidence

**RED** — `.venv/bin/python -m pytest tests/test_naming.py -v`, before `naming.py` existed:
```
ImportError while importing test module '.../tests/test_naming.py'.
E   ImportError: cannot import name 'naming' from 'astrometa' (.../src/astrometa/__init__.py)
```
Expected failure mode (module doesn't exist yet) — same class of failure the brief's Step 2 describes (`ModuleNotFoundError`); pytest's collection wraps it as `ImportError` because `astrometa` the package does exist, only the `naming` submodule doesn't.

**GREEN** — `.venv/bin/python -m pytest tests/test_naming.py -v`, after implementation:
```
tests/test_naming.py::test_canonicalise[M 42-M42] PASSED
tests/test_naming.py::test_canonicalise[M42-M42] PASSED
tests/test_naming.py::test_canonicalise[m42-M42] PASSED
tests/test_naming.py::test_canonicalise[  M  42 -M42] PASSED
tests/test_naming.py::test_canonicalise[NGC 7635-NGC7635] PASSED
tests/test_naming.py::test_canonicalise[ngc7635-NGC7635] PASSED
tests/test_naming.py::test_canonicalise[IC 1848-IC1848] PASSED
tests/test_naming.py::test_canonicalise[Sh2-106-SH2-106] PASSED
tests/test_naming.py::test_canonicalise[SH2-101-SH2-101] PASSED
tests/test_naming.py::test_canonicalise_leaves_proper_names_alone PASSED
tests/test_naming.py::test_upsert_object_is_idempotent PASSED
tests/test_naming.py::test_aliases_collapse_to_one_object PASSED
tests/test_naming.py::test_confidence_gate_allows_only_trusted_sources PASSED
tests/test_naming.py::test_assert_identity_records_confidence PASSED
14 passed in 0.01s
```

**Full suite** — `.venv/bin/python -m pytest`: 112 passed (98 prior from tasks 1-8 + 14 new), no regressions.

## Fix beyond the brief's sample code: `upsert_object`'s `ra`/`dec` COALESCE

The brief's sample `ON CONFLICT` clause already used `COALESCE(excluded.simbad_id, objects.simbad_id)` so a repeat call with `simbad_id=None` wouldn't clobber a value a previous call had set — but it left `ra`/`dec` out of the `UPDATE SET` list entirely, so on a conflict they'd stay at whatever the *first* insert set (silently dropping any new position data a later call tried to supply, with no error).

Since the task context is explicit that `upsert_object` accepts `ra`/`dec` "so the schema and interface are ready" for SIMBAD resolution in a later task, an upsert that silently discards `ra`/`dec` on every call after the first is a latent bug that would surface exactly when that later task starts calling it — not a hypothetical, since it's the documented reason those parameters exist at all. I made `ra`/`dec` follow the same `COALESCE(excluded.x, objects.x)` pattern already used for `simbad_id`, so the three SIMBAD-readiness fields behave consistently: a call with new data updates a previously-NULL field, and a call with no new data (`None`) never clobbers existing data. This is a one-line-per-column SQL change, adds no new behavior beyond what `simbad_id` already had, and required no new tests — `test_upsert_object_is_idempotent` only ever passes `None` for all three on both calls, so it exercises the "don't clobber" path either way and still passes unchanged.

`object_type` is left as unconditional overwrite (`excluded.object_type`, not COALESCEd), matching the brief exactly — a later, more confident classification is meant to win there, and there was no comparable "silently drop new data with no error" defect to fix.

## Files changed

- `src/astrometa/naming.py` (new)
- `tests/test_naming.py` (new)
- Commit: `dc2d598` "feat: object canonicalisation, aliases, identity confidence gate"

## Style note compliance

- `import sqlite3` was not added — nothing in `naming.py` needs it (no `sqlite3.Row` usage, no row-factory mutation, no direct exception handling on sqlite3 types). `solve.py`/`quality.py` import it specifically for `sqlite3.Row`; `naming.py` has no equivalent need, so adding the import would be an unused import.
- No `conn.row_factory` mutation anywhere in this module, so the save/restore pattern from `solve.py`/`quality.py` doesn't apply here — every query either does a bare `fetchone()[0]` (works identically under the default tuple factory or a caller's `sqlite3.Row`) or executes a write with no fetch.

## Self-review (fresh eyes)

- **Completeness**: all six interface items present with the specified signatures (`canonicalise`, `upsert_object`, `add_alias`, `assert_identity`, `CONFIDENCE`, `may_drive_move`).
- **Confidence gate correctness**: `may_drive_move` and `_MOVE_ALLOWED` are the single source of truth, and `assert_identity` calls `may_drive_move` rather than duplicating the source-set check — verified by reading, not just by the passing test, since this is called out as the most important property in the task.
- **Test honesty**: tests are the brief's verbatim; I neither weakened nor added assertions. Confirmed the actual failing-import text against the real repo rather than assuming the brief's `ModuleNotFoundError` wording verbatim (pytest's collection error is textually `ImportError`, same underlying cause) — noted the discrepancy above rather than silently editing the brief's expected text to match.
- **YAGNI**: did not add SIMBAD lookups, retries, or any resolution logic beyond the six specified functions — `ra`/`dec`/`simbad_id` are accepted and stored only, never fetched or validated against an external source, per the explicit scope boundary. Did not add extra tests to reach the brief's (wrong) count of 16.
- **Idempotency**: `upsert_object` verified by test; `add_alias` verified by test; `assert_identity` was not asked to be idempotent (a repeated identity claim from the same source is a new observation worth recording again, e.g. a re-solve), and the schema places no unique constraint on `identity_assertions`, so I didn't add one.
- **No archive writes**: this module only touches the SQLite connection passed in; no filesystem access anywhere.
- **No network calls**: confirmed with `grep -niE "astroquery|urllib|requests|socket|http" src/astrometa/naming.py` — zero matches.
- **Python version**: no post-3.13 syntax; `requires-python = ">=3.13"` in `pyproject.toml` is satisfied (dev box runs 3.14.7, deployment is 3.13.5 per your brief; nothing in this file is version-sensitive — plain `re`, `datetime`, f-strings, dict literals).

## Concerns

- None. The `ra`/`dec` COALESCE fix is a small, low-risk correctness improvement consistent with a pattern the brief's own sample code already established for `simbad_id`, doesn't expand scope, and is called out above rather than left silent.

---

## Fix report: cover the confidence gate's negative path and ra/dec COALESCE (2026-09-09, review round 1)

Review came back "Needs fixes" with two Important findings, both about missing coverage rather than wrong code — the reviewer confirmed the logic correct by inspection on every named risk. Tests-only round; no implementation logic changed.

### What changed

`tests/test_naming.py` — five new test functions (one parametrized over two cases, so six new test items; 14 → 19 total):

- `test_assert_identity_weak_sources_never_set_object_id` (parametrized `dirname`/`object_card`) — asserts the claim IS recorded in `identity_assertions` with the correct confidence, AND `fields.object_id` stays `NULL`. This is Finding 1's core ask: the gate's negative path had zero coverage before this.
- `test_assert_identity_solved_claim_still_sets_object_id_after_weak_claims` — records `dirname` then `object_card` on the same field (confirms `object_id` still `NULL`), then a `solved` claim (confirms `object_id` now set, and all 3 assertions remain on record). Proves a weak claim doesn't poison the field against a later trustworthy one.
- `test_upsert_object_position_is_coalesced_not_clobbered` — Finding 2. First version I wrote only checked "set once, then call again with `None`, values survive" — see the false-pass discovered below; rewrote to also check the NULL→value direction (a later call *supplying* `ra`/`dec`/`simbad_id` must adopt them), since that's the direction that actually exercises the `COALESCE` machinery rather than just the absence of an overwrite.
- `test_assert_identity_raises_on_unknown_source` — the Minor finding: pins that an unrecognised `source` raises `KeyError` before any SQL executes.

### Verifying each new test is a real regression guard, not a tautology

Finding 1 warned that "a future edit could invert the condition and every existing test would still pass" — I treated that as a testable claim about the OLD test suite, not just a reason to add tests, and verified all three safety-critical new tests actually catch the regression they claim to, by temporarily breaking the corresponding implementation behaviour and reverting (`cp` a saved-good copy back) after each:

**Gate inversion** (`if may_drive_move(source):` → `if not may_drive_move(source):`):
```
$ sed -i 's/if may_drive_move(source):/if not may_drive_move(source):/' src/astrometa/naming.py
$ .venv/bin/python -m pytest tests/test_naming.py -v
...
FAILED tests/test_naming.py::test_assert_identity_weak_sources_never_set_object_id[dirname-low]
FAILED tests/test_naming.py::test_assert_identity_weak_sources_never_set_object_id[object_card-medium]
FAILED tests/test_naming.py::test_assert_identity_solved_claim_still_sets_object_id_after_weak_claims
3 failed, 16 passed in 0.02s
```
Confirms exactly what Finding 1 predicted: `test_assert_identity_records_confidence` (the only pre-existing `assert_identity` test, `source="solved"`) still passed under the inverted gate — it would have missed this regression entirely.

**`ra`/`dec` dropped from `ON CONFLICT ... UPDATE SET`** (back to the brief's original sample): my first draft of the COALESCE test passed unchanged under this breakage — a real false pass, caught before committing. Root cause: the draft only tested "set once, call again with `None`, values survive," and "column omitted from `UPDATE SET`" and "column `COALESCE`d against `None`" are indistinguishable in that direction — both leave the value untouched. Rewrote the test to also assert the NULL→value direction (a later call supplying real `ra`/`dec` must adopt them), which only a working `COALESCE` satisfies:
```
$ python3 -c "... drop ra/dec from ON CONFLICT UPDATE SET ..."
$ .venv/bin/python -m pytest tests/test_naming.py -v
...
FAILED tests/test_naming.py::test_upsert_object_position_is_coalesced_not_clobbered
E   AssertionError: assert ('M 42', None, None) == ('M 42', 83.82, -5.39)
1 failed, 18 passed
```

**Silent fallback instead of `KeyError`** (`CONFIDENCE[source]` → `CONFIDENCE.get(source, "low")`):
```
$ sed -i 's/CONFIDENCE\[source\],/CONFIDENCE.get(source, "low"),/' src/astrometa/naming.py
$ .venv/bin/python -m pytest tests/test_naming.py -v
...
FAILED tests/test_naming.py::test_assert_identity_raises_on_unknown_source
E   Failed: DID NOT RAISE KeyError
1 failed, 18 passed
```

Each experiment failed only the test(s) it targeted, confirmed the file was reverted byte-for-byte (`diff` against a saved copy showed no differences) before moving to the next, and `git diff src/astrometa/naming.py` after all three shows zero implementation changes went into the commit.

### Test evidence

**Focused** — `.venv/bin/python -m pytest tests/test_naming.py -v`: 19 passed (14 prior + 5 new functions / 6 new items).

**Full suite** — `.venv/bin/python -m pytest`: 117 passed (112 prior + 5 new), no regressions.

### Commit

`2656cc6` "test: cover the confidence gate's negative path and ra/dec COALESCE"

### Self-review (fresh eyes)

- **Completeness**: all three findings (2 Important + 1 Minor) addressed with dedicated tests.
- **Test honesty**: caught and fixed a genuine false-pass in my own first draft (the ra/dec test) before committing, by actually running the regression-guard verification the reviewer's own reasoning called for rather than assuming a passing test was a correct one.
- **No implementation changes**: confirmed via `git diff --stat` (only `tests/test_naming.py` in the commit) and `git diff src/astrometa/naming.py` (empty) before committing.
- **No archive/network access**: unchanged from the prior round — these are pure in-memory/SQLite tests, no filesystem or network calls added.

### Concerns

- None.
