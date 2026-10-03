# Final review fix wave — report

**Branch:** `feat/metadata-store` (worktree `.worktrees/metadata-store`)
**Baseline:** 26 commits, 179 tests passing
**Result:** 34 commits, **215 tests passing**, 0 failing, 0 skipped-by-default changes

All eight fixes are implemented. No existing test was weakened or deleted. Two
tests changed shape and one changed name; each is called out below with the
reason, and every assertion in them is unchanged or strengthened.

| Fix | Commit | Tests added |
|---|---|---|
| 1 — upsert refreshes derived columns | `3833c2f` | 4 |
| 2 — narrowed `--root` skips the missing sweep | `879d888` | 3 (+1 rewritten) |
| 3 — persist `OBJECT` card and leaf dir | `ae88034` | 3 |
| 4 — grouping fallback identity | `2844668` | 5 |
| 5 — numeric coercion + clustering resilience | `88c3a2d` | 7 |
| 6 — `CHECK` on `frames.disposition` | `6792967` | 4 |
| 7 — `backfill_known_culls` matched/unmatched | `4146a42` | 4 |
| 8 — `captured_at` fallback + dropped count | `a86edee` | 6 |

---

## FIX 1 — `inventory.py`'s `ON CONFLICT` updated too few columns

**Changed** — `src/astrometa/inventory.py`. The upsert's `DO UPDATE SET` now
refreshes every column a scan derives: `path`, `filename`, `size`, `mtime`,
`last_seen`, `frame_type`, `camera`, `filter`, `exptime`, `captured_at`,
`header_ra`, `header_dec`, `focallen`, `xpixsz`, `naxis1`, `naxis2`,
`fingerprint`, `bg_median`, `saturated_frac`, `read_error` (plus
`object_card`/`leaf_dir` from FIX 3). Converted to named parameters — 25
positional binds across an INSERT and an UPDATE clause was already at the edge
of readable.

- `first_seen` is deliberately absent from the update clause, so it survives
  every rescan.
- The `CASE WHEN disposition='quarantined'` guard is byte-for-byte unchanged,
  as instructed.

**Done differently than described:** the refresh is *gated on which read
actually succeeded* (`:header_ok` / `:pixel_ok`) rather than written blindly
from `excluded`. Writing blindly would fix the two reported scenarios but
introduce a third: a *later* transient pixel failure would overwrite a good
`fingerprint` with NULL, which drops that frame out of `cluster._seed_reps`
(it only considers non-null fingerprints as representatives) and spawns
duplicate fields on the next clustering run. `test_pixel_read_failure_does_not_
wipe_a_previously_good_fingerprint` locks that down.

**Covering tests** (`tests/test_inventory.py`):
- `test_rescan_after_manifest_restore_repopulates_derived_columns` — **RED**:
  `assert row[0] is not None` → `header_ra` was still NULL after a full rescan
  of the real files. Green: all six pointing/geometry columns plus
  `bg_median`/`saturated_frac`/`fingerprint` populate, and the restored store
  clusters (`assign_fields == 1`, both frames assigned).
- `test_successful_rescan_fills_columns_a_failed_pixel_read_left_null` —
  **RED**: `assert fp is not None and bg is not None and sat is not None` →
  `assert (None is not None)`. Simulated by patching `fits.getdata` to raise on
  the first scan only, so the *same bytes* (same `content_hash`) are rescanned.
- `test_pixel_read_failure_does_not_wipe_a_previously_good_fingerprint` — guard
  against the fix regressing (green before and after).
- `test_first_seen_is_preserved_across_rescans` — guard (green before and after).

---

## FIX 2 — `scan` mass-marked missing after a partial scan

**Changed** — `src/astrometa/cli.py`. The whole-database `mark_missing` sweep now
runs only when the scanned roots cover the configured root set, tested as a
resolved-path superset. Deliberately conservative: a `--root` that is merely a
*parent* of the configured roots is not recognised as covering them and also
skips. Skipping is always the safe direction. It is never silent — the skip and
its reason print where the count would be:

```
added=5 updated=0 failed=1 missing=skipped (--root narrowed this scan to 1
root(s); the whole-database missing sweep runs only when the scan covers the
configured archive and live roots, so frames outside the scanned roots are NOT
marked missing) (failed overlaps added/updated: ...)
```

**Covering tests** (`tests/test_cli.py`):
- `test_narrowed_root_scan_does_not_mark_unscanned_frames_missing` — **RED**:
  `assert {'missing', 'present'} == {'present'}`, the IC 59 frame flipped to
  `missing` by a scan that never walked its directory.
- `test_narrowed_root_scan_says_the_missing_sweep_was_skipped` — **RED**:
  `assert 'missing=skipped' in 'added=1 updated=0 failed=0 missing=0 ...'`.
- `test_full_configured_root_scan_still_runs_the_missing_sweep` — proves the
  sweep is not disabled: a vanished frame still becomes `missing`.

**Existing test whose premise this invalidates — stated explicitly, not quietly
edited.** `test_rescan_against_empty_root_raises_loudly` drove the "an empty
scan must not mark everything missing" guard through `--root <empty dir>`.
Under FIX 2 a `--root` scan deliberately suppresses the sweep, so that route no
longer reaches `mark_missing` at all. I renamed it
`test_rescan_against_empty_configured_roots_raises_loudly` and drove the
identical guarantee through the configured-root path that still performs the
sweep (monkeypatching `cli.Config` at two empty roots). Same `pytest.raises(
ValueError)`, same guarantee, different route in. Nothing was relaxed.

---

## FIX 3 — persist the `OBJECT` card and the leaf directory name

**Changed** — `src/astrometa/db.py` (two columns on `frames`: `object_card TEXT`,
`leaf_dir TEXT`), `src/astrometa/inventory.py` (populate both; `leaf_dir` is the
directory *name*, not the full path). `object_card` is coerced to `str` because
`fitsheader._parse_value` returns whatever a card parses to, so an unquoted
numeric `OBJECT` would otherwise land in a TEXT column as an int. Both are
stored verbatim and are explicitly not identity claims.

**Covering tests** (`tests/test_inventory.py`): `test_scan_persists_object_card_
and_leaf_dir`, `test_object_card_absent_is_null_but_leaf_dir_still_recorded`
(the real `Lights`-only directories), `test_object_card_and_leaf_dir_refresh_on_
rescan`. All three **RED** with `sqlite3.OperationalError: no such column`.

**Choice worth flagging:** I did *not* add these two fields to the manifest
sidecar. The brief scoped FIX 3 to inventory, the manifest document format is
versioned and documented as v1, and FIX 1 now makes a rescan repopulate them
after a restore anyway. Adding them is a one-line change to `manifest._FIELDS`
plus `import_file` if you want the labels to survive a DB loss without a rescan.

---

## FIX 4 — `build_projects` lumped every target together

**Verified against the unfixed code before fixing.** Two frames, IC 59 and M 42,
same night, same `HaO3` filter → `projects created = 1`, memberships
`[('h1', 1), ('h2', 1)]`. Both targets in one project.

**Changed** — `src/astrometa/grouping.py` (new `frame_identity()`; bucket key is
now `(object_id, identity_name, filter, session_date)`), `src/astrometa/db.py`
(`projects.identity_name`, `projects.identity_source`).

`identity_source` mirrors the spec §7 confidence model: `object`
(authoritative — `object_id` resolved, `identity_name` NULL), `object_card`
(medium), `dirname` (low), `unknown`. A fallback label can therefore never be
mistaken for a solved one, and per spec §7 neither fallback may drive a Phase 2
move.

**Covering tests** (`tests/test_grouping.py`):
- `test_different_targets_same_night_same_filter_are_separate_projects` — the
  requested proof: two projects, named `IC 59` and `M 42`.
- `test_identity_falls_back_to_leaf_dir_when_no_object_card` — `Veil2` and
  `NGC6960` stay separate, both sourced `dirname`.
- `test_identity_source_records_how_the_bucket_was_named`.
- `test_resolved_object_id_takes_precedence_over_the_fallback` — `M31` and
  `M 31` (real sibling directories here) stay in **one** project once
  `object_id` is resolved. This is why `object_id` remains the primary key of
  the bucket rather than being replaced by the fallback.
- `test_frames_with_no_identity_at_all_still_group_by_night`.

**Done differently:** `build_projects` now returns a `GroupingResult` dataclass
rather than a bare `int`, matching `inventory.scan`'s `InventoryResult` and
`quality.measure_frames`'s `QualityResult`. FIX 8 needed a second number out of
this function and there was nowhere to put it. Seven existing tests were updated
**at the call site only** (`build_projects(conn)` → `build_projects(conn).projects`);
every assertion is character-for-character unchanged.

---

## FIX 5 — numeric headers not coerced; one bad row discarded a whole run

**Verified against the unfixed code before fixing.** Three frames, one with a
sexagesimal `RA` (`typeof(header_ra) = text`): `assign_fields` raised
`TypeError: unsupported operand type(s) for -: 'float' and 'str'`, and after
closing the connection a fresh one showed **0 fields persisted, 0 frames with a
`field_id`**. The two good frames' work was thrown away.

**Changed (half 1)** — `src/astrometa/inventory.py`: `_coerce_numeric()` coerces
`EXPTIME`, `RA`, `DEC`, `FOCALLEN`, `XPIXSZ` (float) and `NAXIS1`, `NAXIS2`
(int) at read time. A value that will not coerce is stored as **NULL** and named
in `read_error`; the frame counts as `failed`. Booleans are rejected outright so
a FITS `T` card cannot silently become a pointing of `1.0`.

Sexagesimal is deliberately **not** parsed into a decimal. `OBJCTRA` is the card
that carries that form and this code does not read it; inventing a decimal for a
card the parser does not claim to understand is exactly the silent fallback the
project rules forbid. Recording the failure is the honest outcome.

**Changed (half 2)** — `src/astrometa/cluster.py`: new `ClusteringError`; new
`_pointing()` validates a row's `header_ra`/`header_dec` are actually numeric
**before anything is written for that row**. Unusable rows are skipped, and
`assign_fields` **commits the run's successful work first, then raises**
`ClusteringError` naming every failure (capped at 10 plus a count) and how many
fields it kept. Still loud; no longer catastrophic.

Validating up front matters independently: without it a bad row reaching an
empty `reps` list becomes a field representative with a text `ra`, and every
frame later compared against it fails too — cascading one bad value into a
whole-run failure by a second route.

**Covering tests:**
- `tests/test_inventory.py`: `test_sexagesimal_ra_is_not_stored_as_text_in_a_
  real_column` (**RED**: `res.failed` was `0`; now `typeof(header_ra) = 'null'`
  and `read_error` names `RA`), `test_uncoercible_value_is_loud_but_the_rest_of_
  the_header_survives`, `test_numeric_headers_are_stored_with_the_right_sqlite_
  type`.
- `tests/test_cluster.py`: `test_one_unusable_row_does_not_discard_the_whole_
  clustering_run` (**RED**; asserts persistence through a **fresh
  `sqlite3.connect`**, not the same connection's open transaction — that
  distinction is what makes the assertion meaningful),
  `test_unusable_row_is_never_made_a_field_representative`,
  `test_clustering_error_names_every_failure_and_the_work_it_kept`,
  `test_a_clean_run_still_returns_the_new_field_count` (guard on the unchanged
  `int` return for the success path).

---

## FIX 6 — `CHECK` constraint on `frames.disposition`

**Changed** — `src/astrometa/db.py`:
`CHECK (disposition IN ('present', 'missing', 'quarantined'))`.

**Covering tests** (`tests/test_db.py`):
`test_disposition_is_constrained_to_the_three_contract_values` (**RED**: `Failed:
DID NOT RAISE IntegrityError` on `'quarantine'`, the singular typo that would
silently defeat inventory's `CASE WHEN disposition='quarantined'` guard), plus
`test_the_three_contract_dispositions_are_accepted` parametrised over all three.

---

## FIX 7 — `backfill_known_culls` silently no-opped on the motivating case

**Verified against the unfixed code before fixing.** One cull pattern matching
no frame: `return value = 0`, `rows written anywhere = 0`. Silent, on the exact
case this project exists for.

**Changed** — `src/astrometa/db.py` (new `known_culls` table),
`src/astrometa/disposition.py` (new `BackfillResult`; rewritten
`backfill_known_culls`).

- Returns `BackfillResult(applied, matched, unmatched, unmatched_patterns)`. A
  cull matching zero frames is now a named, counted outcome.
- Every cull entry is persisted in `known_culls` — matched or not — so the
  knowledge exists independently of any frame row, since for the 39 IC 59 culls
  there may never be one.
- Re-runs upsert one row per pattern and refresh `matched`, so the record
  self-corrects if a frame later turns up. `recorded_at` is preserved across
  re-runs: it records when the knowledge was first written down.
- **No `frames` row is synthesised** for a file that exists nowhere, per your
  ruling.

**Done differently:** the table carries a fourth column, `matched INTEGER`,
beyond the three the brief named. Persisting *only* unmatched culls (the literal
reading) leaves a stale "unmatched" record forever once a frame appears; the
count is what makes a re-run self-correcting, and it lets an operator see at a
glance which historical culls the store can and cannot account for.
`test_backfill_is_idempotent_and_self_corrects_when_a_frame_appears` covers it.

**Covering tests** (`tests/test_disposition.py`):
`test_backfill_reports_unmatched_culls_instead_of_a_silent_zero`,
`test_an_unmatched_cull_is_persisted_independently_of_any_frame_row` (also
asserts `COUNT(*) FROM frames == 0` — nothing invented),
`test_matched_culls_are_recorded_too`,
`test_backfill_is_idempotent_and_self_corrects_when_a_frame_appears`. All **RED**
(`AttributeError` / `no such table: known_culls`). The three existing backfill
tests were updated **at the call site only** (`n` → `res.applied`).

---

## FIX 8 — `captured_at` had no fallback; dropped frames were silent

**Changed** — `src/astrometa/inventory.py`: `_captured_at()` returns `DATE-OBS`,
falling back to the filename capture instant (spec §7). The fallback reuses
`grouping.capture_instant` — the same parser `build_projects` buckets on, not a
second copy of the regex — so the two can never disagree about what a capture
instant is. When neither source has one, `captured_at` stays NULL; nothing is
invented.

**Changed** — `src/astrometa/grouping.py` + `src/astrometa/cli.py`:
`GroupingResult.skipped_no_capture_instant` counts light frames dropped for
lacking the `_YYYYMMDD-HHMMSS_` token, and `group` prints it.

**Covering tests:**
- `tests/test_inventory.py`: `test_captured_at_falls_back_to_the_filename_
  capture_instant` (**RED**), `test_date_obs_wins_over_the_filename_fallback`,
  `test_captured_at_is_null_when_neither_source_has_one`.
- `tests/test_grouping.py`: `test_frames_without_a_capture_instant_are_counted_
  not_silently_dropped` (**RED**), `test_nothing_skipped_reports_zero`.
- `tests/test_cli.py`: `test_group_reports_frames_skipped_for_lack_of_a_capture_
  instant` (**RED**).

---

## Composed end-to-end check

Beyond the unit suite I ran the real CLI over a synthetic two-target archive
(two targets × two nights, plus one zero-byte unreadable file), in a temp dir:

```
scan --root <archive>   added=5 updated=0 failed=1 missing=skipped (--root narrowed...)
cluster                 fields_created=2
group                   projects=4 skipped_no_capture_instant=0
status                  frames: 5  fields: 2  objects: 0  projects: 4
```

Projects came out as four distinct `(target, night)` buckets, each with
`identity_source='object_card'` — not one lumped project. The unreadable file
was inventoried with a loud `read_error` naming both the header and pixel
failures, its `leaf_dir` recorded, and `captured_at` left NULL. Temp fixture
removed afterwards.

---

## Concerns and residuals

1. **A legacy store can still hold a corrupt field representative.** FIX 5
   validates candidate rows before they can become representatives, but
   `_seed_reps` loads representatives already assigned by a pre-fix run. If one
   has a text `header_ra`, frames whose fingerprint is close to it will fail
   (loudly, contained, one row each) rather than the whole run. I chose
   containment over raising at seed time: refusing to run at all would block the
   pass on a store-level problem that only affects one field's matching. Flagging
   it because it is a real degradation mode, not a hypothetical.

2. **Three return types changed** — `build_projects` → `GroupingResult`,
   `backfill_known_culls` → `BackfillResult`, `assign_fields` now raises
   `ClusteringError`. Nothing outside `cli.py` and the tests calls these, and
   `cli.py` is updated. Noting it because it is API surface a reviewer should
   see rather than discover.

3. **`tests/test_quality.py:223` and `tests/test_solve.py:308` reference
   `/mnt/qnap/astro_data`.** Pre-existing (Tasks 7/8), not mine, read-only, and
   guarded by a `root.exists()` skip — but the standing constraint says no test
   may touch `/mnt/qnap` at all. Left alone since changing them is outside this
   wave; raising it so the decision is yours.

4. **`quality.py` still measures a frame whose `bg_median` is NULL** and writes a
   row with NULL `sky_background`. FIX 1 removes the way that state used to
   *persist* across a successful rescan, so the composed defect is gone, but the
   underlying leniency in `quality.py` is untouched — it was not in this wave.

5. **The manifest sidecar does not carry `object_card`/`leaf_dir`** — see FIX 3.
   A DB restored from a manifest alone has no grouping identity until it is
   rescanned. Rescanning fixes it; a one-line manifest change would remove the
   dependency.

Not attempted, per your explicit exclusions: live SIMBAD resolution, spec §6.9
adjacent-night project merging, and the noted duplication (five `row_factory`
save/restore blocks, two scratch-copy+subprocess blocks, two `_now()`).

---

# Addendum — corrections from the scoped re-review

**Commit:** `4ae4928` — `fix: gate frame_type on header_ok, and let captured_at fall back on rescan`
**Tests:** 215 → **219 passing**, 0 failing. 4 added, 2 of them RED first.

Both defects were real, both were mine, and both sat in the single
`ON CONFLICT ... DO UPDATE SET` clause FIX 1 rewrote. Neither existed before
this wave — they are consequences of columns FIX 1 newly *added* to the update
clause without carrying the gating rationale across to them.

## CORRECTION 1 (Important) — `frame_type` updated unconditionally

`frame_type` was the one header-derived column left ungated, so it inherited
none of FIX 1's reasoning even though it has exactly the same failure mode.
`classify()` falls back to the filename when the header dict is empty, and a
failed header read produces an empty dict.

**RED, verified against the pre-correction code:**

```
assert conn.execute("SELECT frame_type FROM frames").fetchone()[0] == "light"
E  AssertionError: assert 'derived' == 'light'
E    - light
E    + derived
```

A frame carrying `IMAGETYP='Light Frame'` but named `Autosave001.fit` scanned
as `light`, then a transient header failure on rescan rewrote it to `derived`.
Both `cluster.assign_fields` and `grouping.build_projects` filter
`frame_type='light'`, so the frame silently left the pipeline — the exact class
of silent drop FIX 1 was written to close, reintroduced one line above it.

**Fixed:** `frame_type=CASE WHEN :header_ok THEN excluded.frame_type ELSE
frame_type END`, matching every other header-derived column.

**Covering tests** (`tests/test_inventory.py`):
- `test_transient_header_failure_does_not_reclassify_a_light_frame` — asserts
  `frame_type` survives the failed rescan **and** that the concrete consequence
  is gone: `cluster.assign_fields(conn) == 1` (it returned 0 before).
- `test_a_readable_header_still_reclassifies_a_renamed_frame` — the gate must
  not freeze `frame_type` forever. A readable header stays authoritative, so a
  corrected `IMAGETYP` still takes effect on rescan. Guard against
  over-correcting.

## CORRECTION 2 (Minor, same clause) — `captured_at` fallback never reached a rescan

`captured_at` was gated on `:header_ok` like its siblings, but it is the one
header-derived column whose *fallback source needs no header*: `_captured_at`
falls back to the filename capture instant (spec §7). So FIX 8's fallback
applied on first insert and never on a rescan whose header failed — the one
case it was added for. A row restored from a manifest carrying no
`captured_at` could never acquire one from its own filename.

**RED, verified against the pre-correction code:**

```
assert conn.execute("SELECT captured_at FROM frames").fetchone()[0] \
    == "2026-09-08T22:00:00"
E  AssertionError: assert None == '2026-09-08T22:00:00'
```

**Fixed:**

```sql
captured_at=CASE WHEN :header_ok THEN excluded.captured_at
                 ELSE COALESCE(captured_at, excluded.captured_at) END
```

With a failed header read, `excluded.captured_at` *is* the filename fallback,
so no new bind was needed. The COALESCE order is load-bearing in both
directions: a readable header stays authoritative, and when it is not readable
an already-recorded `DATE-OBS` (UTC, sub-second) is never downgraded to the
filename's local wall-clock token.

**Covering tests** (`tests/test_inventory.py`):
- `test_filename_fallback_fills_captured_at_when_the_header_read_fails` —
  the manifest-restored-row scenario.
- `test_a_recorded_date_obs_is_not_clobbered_by_the_filename_fallback` — guard
  on the COALESCE order, so the fix cannot degrade the better source.

## Parked, per your ruling — not touched

- **`init_schema` has no migration path.** A store built from the pre-wave
  schema lacks the new columns and `build_projects` fails with
  `OperationalError`. Recorded as the top follow-up; latent today because no
  `.sqlite` exists in the tree and the first real run creates a fresh store.
- **`known_culls.matched` stores a count, not the boolean its name and
  `DEFAULT 0` imply.** Cosmetic; rename or re-document later.

## Standing concerns from the main report

Items 1 and 3–5 of the original report are unchanged and still stand (legacy
corrupt field representative; the two pre-existing `/mnt/qnap` test references;
`quality.py`'s leniency on a NULL `bg_median`; the manifest not carrying
`object_card`/`leaf_dir`). Item 2 (changed return types) is now also joined by
`assign_fields` raising `ClusteringError`, as already noted.
