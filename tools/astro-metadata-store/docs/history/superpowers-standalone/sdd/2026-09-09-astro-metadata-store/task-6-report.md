# Task 6 report: Field clustering

## What I implemented

- `src/astrometa/cluster.py`
  - `angular_separation(ra1, dec1, ra2, dec2) -> float` — great-circle separation in degrees, as specified in the brief.
  - `frame_fov_deg(row) -> tuple[float, float]` — FOV width/height in degrees from focal length, pixel size, and axis dimensions; falls back to `DEFAULT_FOV_DEG = 1.0` when any input is missing/zero, as specified.
  - `_seed_reps(conn) -> list[dict]` (new, not in the brief) — loads one representative frame per field already in `fields`, so `assign_fields` starts from the true clustering state instead of an empty list. See "Idempotency fix" below.
  - `assign_fields(conn, fp_threshold: int = 12) -> int` — clusters `frame_type='light'` frames with a non-null fingerprint and header pointing, using the brief's greedy leader-clustering loop (fingerprint Hamming distance AND angular separation within the candidate frame's own FOV tolerance), corrected for idempotency.
- `tests/test_cluster.py` — the brief's Step 1 tests, kept verbatim per your instruction.

## TDD evidence

**RED** — `.venv/bin/python -m pytest tests/test_cluster.py -v`, before `cluster.py` existed:
```
ImportError while importing test module '.../tests/test_cluster.py'.
E   ImportError: cannot import name 'cluster' from 'astrometa'
```
Expected failure mode (module doesn't exist yet) — matches the brief's Step 2 expectation, module-not-found rather than a logic failure.

**GREEN** — `.venv/bin/python -m pytest tests/test_cluster.py -v`, after implementation:
```
tests/test_cluster.py::test_angular_separation_is_correct PASSED
tests/test_cluster.py::test_frames_of_same_field_share_a_field_id PASSED
tests/test_cluster.py::test_distant_pointings_are_separate_fields PASSED
tests/test_cluster.py::test_same_pointing_different_fingerprint_is_separate PASSED
tests/test_cluster.py::test_assign_fields_is_idempotent PASSED
tests/test_cluster.py::test_frames_without_pointing_are_left_unassigned PASSED
6 passed in 0.04s
```

**Full suite** — `.venv/bin/python -m pytest -v`: 57 passed, no regressions in tasks 1-5's tests.

**Manual incremental-run check** (not part of the committed test file — ad hoc verification of the idempotency fix beyond the single-frame case the brief's test covers): inserted `h1`, ran `assign_fields` (created=1); inserted `h2` (same field as h1) and `h3` (distant, new field), ran again (created=1, `h2`→field 1, `h3`→field 2); ran a third time with nothing new (created=0). Final state: `h1`→1, `h2`→1, `h3`→2, `fields` count=2. Confirms seeding correctly extends prior clustering state rather than only handling the trivial "nothing changed" case.

## Idempotency fix — how `reps` is seeded

The brief's code initialized `reps: list[dict] = []` fresh every call, so a second run matched nothing and inserted a duplicate `fields` row per frame — the exact bug you flagged.

Fix: `_seed_reps(conn)` runs before the clustering loop and loads, for every `field_id` already present on any frame, the representative frame's `fingerprint`/`header_ra`/`header_dec` — specifically the frame with the **smallest `content_hash`** carrying that `field_id`. That's provably the frame that originally created the field: `assign_fields` processes candidates in ascending `content_hash` order, and a field is only ever created when a frame fails to match every rep already in `reps`, so the first (smallest-hash) member of a field is necessarily the one that created it — every later member matched against it (or a rep chained from it). Query:

```sql
SELECT f.field_id AS id, f.fingerprint, f.header_ra AS ra, f.header_dec AS dec
FROM frames f
INNER JOIN (
    SELECT field_id, MIN(content_hash) AS rep_hash
    FROM frames WHERE field_id IS NOT NULL GROUP BY field_id
) rep ON f.field_id = rep.field_id AND f.content_hash = rep.rep_hash
ORDER BY f.field_id
```

`reps` is then extended by the normal clustering loop as before. A frame that already has a `field_id` gets re-evaluated on every run too (the candidate query doesn't filter on `field_id`), but since its seeded rep is either itself or a rep it already matched, it always re-matches the same field — a no-op UPDATE, not a new field.

## One deliberate interpretation beyond the brief's sample code

The brief's Step 3 code returns `len(reps)` (total field count after the call). The task's interface spec says `assign_fields` returns "the number of fields created." Once `reps` is seeded from the database, `len(reps)` includes fields that already existed before this call, not just ones created by it — literally wrong for a re-run (would report duplicating counts on every call). I return a `created` counter incremented only on actual `INSERT INTO fields`, so a rerun that finds nothing new correctly returns 0. All five brief tests still pass under this reading (in every test, all fields present ARE newly created in that call, so `len(reps)` and my `created` count coincide) — this wasn't a case where the tests forced my hand, it's a literal reading of the interface doc plus what makes sense for a re-run. Flagging it since it's a value-level behavior change from the brief's sample code, not just wiring your idempotency fix.

## Files changed

- `src/astrometa/cluster.py` (new)
- `tests/test_cluster.py` (new)
- Commit: `36f02b9` "feat: field clustering by fingerprint and pointing"

## Style note compliance

Imported `sqlite3` normally at module top (not the brief's inline `__import__`). Scoped `conn.row_factory = sqlite3.Row` more tightly than the brief: saved the previous factory and restored it in a `finally` block around the whole function body, rather than leaving the mutation permanent on the shared connection — per your note, this didn't require restructuring the pass.

## Self-review (fresh eyes)

- **Completeness**: all brief interface functions present with the specified signatures; idempotency correction implemented and verified beyond just the brief's single-frame test.
- **Correctness of the representative-selection proof**: walked through it above; it holds because candidate ordering (`ORDER BY content_hash`) is identical between the original clustering loop and the `MIN(content_hash)` used to pick each field's representative on reseed.
- **YAGNI**: did not add filtering already-assigned frames out of the re-run candidate query as a performance optimization — not requested, and correctness doesn't depend on it (already-assigned frames just re-confirm their existing field, an idempotent no-op). Did not add a `frame_type='light'` filter to `_seed_reps` — redundant, since `field_id` is only ever set by this function's own UPDATE, which only fires on rows already filtered to `frame_type='light'`.
- **Test honesty**: tests are the brief's verbatim; I did not weaken or narrow any assertion. The idempotency test's `COUNT(*) FROM fields == 1` after two runs is the real requirement encoded, unchanged.
- **Error handling**: `hamming`'s `ValueError` on width-mismatched fingerprints is not caught anywhere in this pass, consistent with the instruction that such a raise is a real signal (all fingerprints in the current pipeline are produced at the same width by `imagekeys.fingerprint`, so this should not fire in practice).
- **No archive writes**: this pass only touches the SQLite connection passed in; no filesystem/archive access.

## Concerns

- The `created`-vs-`len(reps)` return-value interpretation above is a judgment call, not something the brief's tests forced — worth a quick confirm since it's user-facing return semantics, though I'm confident it's the correct reading of the interface doc.
- Matching algorithm is greedy/single-link against a fixed per-field representative (as specified by the brief, unchanged by me): a frame that would only match a *non-representative* member of a field (not the original rep, and not within tolerance of the rep itself) creates a new field instead of joining. This is inherited from the brief's design, not something I introduced or was asked to change — flagging in case it's a known/accepted limitation for Task 7's downstream consumption, since it means a field can theoretically be split across two `fields` rows if pointing/fingerprint drift chains away from the original representative.

---

## Fix report: fp_threshold correction (2026-09-09, post-review)

Team lead measured real archive frames and found the brief's `fp_threshold` default of 12 unusable: real same-field frames measured 20-45 bits apart (dithering/seeing noise, excluding a meridian-flip outlier at 143 that legitimately splits), real different-field frames measured 113+. At 12, no two real frames would ever cluster, defeating the pass's purpose.

### What changed

- `src/astrometa/cluster.py`: added `FP_THRESHOLD_DEFAULT = 80` as a named module-level constant with a comment recording the measured basis (same-field 20-45, different-field 113+, measured 2026-09-09) and the reasoning for choosing 80 (clears both bands with margin, errs toward the cheaper failure mode of an extra solve rather than a false merge). `assign_fields`'s default now reads `fp_threshold: int = FP_THRESHOLD_DEFAULT` instead of the literal `12`.
- `tests/test_cluster.py`:
  - Added `_fp_pair(distance, width_hex=64) -> tuple[str, str]` — builds two hex fingerprints at an exact, controlled Hamming distance (flips the low `distance` bits of an all-zero base), at the 64-hex-char/256-bit width `imagekeys.fingerprint()` actually emits.
  - Added `test_measured_same_field_distance_clusters` — a 45-bit-apart pair (the observed same-field maximum) with identical pointing must cluster (`assign_fields(conn) == 1`).
  - Added `test_measured_different_field_distance_does_not_cluster` — a 113-bit-apart pair (the observed between-field minimum) with identical pointing must not cluster (`== 2`).
  - Fixed `test_same_pointing_different_fingerprint_is_separate`: its original 8-hex-char (32-bit) fingerprint literals cap out at a max possible Hamming distance of 32, which is below the new threshold of 80 — under the new default this test would silently start asserting the wrong thing (expects 2 fields, would get 1). Assertion and intent are unchanged; only the input fingerprints were widened via `_fp_pair(200)` so the test actually exercises "different fingerprint" at the real default. This is the only existing test that needed touching — the other four don't depend on fp_threshold's exact value (two rely on pointing distance instead, two don't compare fingerprints at all).

### Verifying the new tests are real regression guards, not tautologies

Before committing, confirmed `test_measured_same_field_distance_clusters`'s scenario (a 45-bit pair, same pointing) would fail under the old threshold of 12:
```
$ .venv/bin/python -c "... cluster.assign_fields(conn, fp_threshold=12) ..."
with old threshold=12, fields created: 2
```
i.e. it would have produced 2 fields (wrong) instead of 1 — confirming the test would have caught the original bug.

### Test evidence

`.venv/bin/python -m pytest tests/test_cluster.py -v`:
```
tests/test_cluster.py::test_angular_separation_is_correct PASSED
tests/test_cluster.py::test_frames_of_same_field_share_a_field_id PASSED
tests/test_cluster.py::test_distant_pointings_are_separate_fields PASSED
tests/test_cluster.py::test_same_pointing_different_fingerprint_is_separate PASSED
tests/test_cluster.py::test_measured_same_field_distance_clusters PASSED
tests/test_cluster.py::test_measured_different_field_distance_does_not_cluster PASSED
tests/test_cluster.py::test_assign_fields_is_idempotent PASSED
tests/test_cluster.py::test_frames_without_pointing_are_left_unassigned PASSED
8 passed in 0.04s
```

`.venv/bin/python -m pytest -v` (full suite): 59 passed (57 prior + 2 new), no regressions.

### Commit

`7a94fca` "fix: raise fp_threshold default from 12 to 80, measured against real frames"

---

## Fix report: representative drift correction (2026-09-09, review round 1)

Review found an Important-severity hole in the idempotency fix: `_seed_reps` picked each field's representative by `MIN(content_hash)`, but content_hash (a BLAKE2b digest) has no relation to insertion order. On a normal incremental run, a newly inserted frame has roughly even odds of sorting before the field's original representative and silently taking over the role. Because matching is greedy single-link, a member is only guaranteed to be within tolerance of whichever frame was representative *at the time it matched* — a later reseed to a different representative could move an already-assigned frame to a different field, violating the idempotency constraint.

### Reproducing the bug before fixing (evidence before theory)

Built a scenario with asymmetric FOV between two frames (so the angular-separation tolerance is directional, not symmetric — see the test's comment for why symmetric FOV can't expose this bug): `z_orig` (long focal length, tight ~0.13° tolerance) creates a field; `a_drift` (default/short focal length, generous ~1.3° tolerance, content_hash sorting *before* z_orig's) joins it correctly on the next run, matched against z_orig using a_drift's own generous tolerance. A *third* call with no new data at all — pure idempotency check — is where the bug surfaced:
```
run1 created: 1
run2 created: 0
after run2: {'z_orig': 1, 'a_drift': 1}
run3 created: 1
after run3: {'z_orig': 2, 'a_drift': 1}
field count: 2
```
`z_orig` was silently moved from field 1 to a brand-new field 2 with zero new data inserted, purely because reseeding picked `a_drift` (smaller content_hash) as the new representative, and re-evaluating z_orig against it used z_orig's own tight tolerance (0.5° actual separation > 0.13° tolerance) — a genuine corruption of an already-committed assignment. Confirmed against the actual pre-fix source (not a hypothetical), before writing any fix code.

### Fix

`src/astrometa/cluster.py`:
1. **Structural guarantee (part 1)**: `assign_fields`'s candidate query now adds `AND field_id IS NULL`, so a frame already assigned to a field is never re-evaluated or reassigned by a later call — this holds regardless of how the representative is chosen, closing the corruption hole directly.
2. **Convergence (part 2)**: `_seed_reps` now selects each field's representative via `MIN(rowid)` instead of `MIN(content_hash)`. SQLite's implicit `rowid` always increases with insertion order, so the frame that originally created a field keeps the representative role across incremental runs — this is what keeps a newly added frame joining the *same* field a single full run would have put it in (not just "not corrupting old data", but "matching what a fresh run would decide"). Documented in a comment that this relies on rowid stability, which a `VACUUM` on the store can break.
3. **Minor**: added `AND fingerprint IS NOT NULL` to `_seed_reps`'s candidate-selection subquery, with a comment noting it's currently unreachable (field_id is only ever set by this module on rows that already passed a fingerprint-not-null check) but documents the invariant so a future change can't silently violate it.

Did not touch: `FP_THRESHOLD_DEFAULT` and its measured-basis comment, the `created` return semantics, or the greedy single-link matching strategy — all confirmed settled by the reviewer.

### New regression test

`tests/test_cluster.py::test_incremental_rerun_does_not_reassign_when_new_hash_sorts_first` reproduces the exact scenario above (asymmetric FOV, new frame's content_hash sorting before the existing representative's, then a third call with nothing new to cluster) and asserts neither frame's `field_id` changes and no duplicate field appears.

**RED** (confirmed against the actual pre-fix committed code via `git stash`, not a rewritten copy):
```
$ git stash push -- src/astrometa/cluster.py
$ .venv/bin/python -m pytest tests/test_cluster.py -v -k incremental_rerun
...
>       assert cluster.assign_fields(conn) == 0
E       assert 1 == 0
FAILED tests/test_cluster.py::test_incremental_rerun_does_not_reassign_when_new_hash_sorts_first
1 failed, 8 deselected in 0.04s
$ git stash pop
```
Failed exactly as predicted: the third (no-op) call created a new field instead of 0.

**GREEN**:
```
$ .venv/bin/python -m pytest tests/test_cluster.py -v
...
tests/test_cluster.py::test_incremental_rerun_does_not_reassign_when_new_hash_sorts_first PASSED
9 passed in 0.04s
```

Full suite: `.venv/bin/python -m pytest -v` → 60 passed (59 prior + 1 new), no regressions.

### Commit

`55b9fa7` "fix: never reassign an already-clustered frame; seed reps by insertion order"
