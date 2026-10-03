# Task 5 report: Inventory pass

## What I implemented

`src/astrometa/inventory.py`:

- `InventoryResult` dataclass (`added`, `updated`, `failed`, `seen_hashes`) exactly per the brief's interface.
- `scan(conn, roots: list[Path], read_pixels: bool = True) -> InventoryResult`: walks `roots` via `rglob("*.fit")`, excludes `_dedup_quarantine` / `@Recycle` paths and `*_thn*` thumbnails, computes `content_hash` per file, and upserts one row per frame into `frames` keyed on `content_hash`.

**Applied the team lead's correction**: the brief's single combined try block (header read + pixel read + fingerprint + stats, one `except` setting `header = {}`) was replaced with two independent helpers:

- `_read_header(path) -> (header_dict, error_or_None)` — on failure returns `({}, "header: <Type>: <detail>")`.
- `_read_pixels(path) -> (fp, bg, sat, error_or_None)` — does the single `fits.getdata()` call and derives both the fingerprint and pixel stats from that one in-memory array (still one pixel read, not two). On failure returns `(None, None, None, "pixels: <Type>: <detail>")`. Critically, `fp`/`bg`/`sat` are only committed to the real return values *after all three steps succeed* — if `pixel_stats` raises (e.g. on NaN) after `fingerprint` already returned a value, that value is discarded, not partially recorded. This avoids leaking a fingerprint computed from data that failed validation.

In `scan()`, `header, header_err = _read_header(path)` runs unconditionally; `fp, bg, sat, pixel_err = _read_pixels(path)` runs independently (only skipped entirely when `read_pixels=False`). `errors = [e for e in (header_err, pixel_err) if e]`; `read_error` is `"; ".join(errors)` or `None`; `res.failed += 1` once per frame if `errors` is non-empty (not once per error) — so a frame with both a bad header and bad pixels still only increments `failed` by 1.

The counter semantics from the correction are preserved exactly: a failing frame's row is still written and still counts toward `added` or `updated` (via the existing pre-insert `SELECT 1 FROM frames WHERE content_hash=?` check), and `failed` is incremented independently alongside it — never instead of it.

No schema changes — everything still goes through the single existing `read_error` TEXT column, with the two prefixes making the two failure modes machine-distinguishable within it.

## Tests written (`tests/test_inventory.py`)

The brief's 5 tests, plus 3 added to cover the correction and the real-archive facts called out in the task message:

1. `test_scan_records_frames` (brief) — happy path, verifies frame_type/camera/filter/fingerprint/disposition.
2. `test_scan_is_idempotent` (brief) — scanning twice doesn't duplicate rows.
3. `test_colliding_filenames_are_distinct_rows` (brief) — two files with the same name, different content, both get rows (content_hash is the key).
4. `test_excluded_paths_are_skipped` (brief) — `_dedup_quarantine` path is skipped entirely.
5. `test_unreadable_file_is_recorded_not_skipped` (brief, extended) — added `assert res.added == 1` alongside the existing `res.failed == 1` check, to directly prove the overlapping-counter semantics from the correction (not just that a failure is recorded, but that it's recorded *as well as* being added).
6. `test_zero_byte_file_is_recorded_not_skipped` (new) — the real-archive fact: a genuine 0-byte `.fit` file. Verifies `content_hash` succeeds, the row is written with `size == 0`, `failed == 1`, and `read_error` is set.
7. `test_failed_scan_is_idempotent_and_updates_on_rescan` (new) — scans a permanently-broken file twice: first scan is `added=1, failed=1`; second is `added=0, updated=1, failed=1`; row count stays 1. Proves a failing frame is retryable (re-scannable) rather than being a dead end.
8. `test_nan_pixels_dont_lose_header_metadata` (new) — the concrete scenario the correction exists for: a frame with a valid header (camera/filter/IMAGETYP all set) and one NaN pixel. Verifies `frame_type == "light"` (proves `classify()` ran against a real parsed header, not `{}`), `camera`/`filter` are populated, `fingerprint`/`bg_median`/`saturated_frac` are all `None` (the pixel-side atomic rollback), and `read_error` starts with `"pixels:"` and does **not** contain `"header:"`.

## TDD evidence

**RED** — `.venv/bin/python -m pytest tests/test_inventory.py -v`, before `inventory.py` existed:
```
ImportError while importing test module '.../tests/test_inventory.py'.
tests/test_inventory.py:3: in <module>
    from astrometa import db, inventory
E   ImportError: cannot import name 'inventory' from 'astrometa'
```
Expected and correct: the module didn't exist yet.

I also hit one self-inflicted RED after implementation, worth recording: my first draft of `test_nan_pixels_dont_lose_header_metadata` asserted `frame_type == "derived"` (reasoning from the filename `pp_light_stack_0001.fit`), but the test's own header sets `IMAGETYP = "Light Frame"`, and `classify.py` gives the `IMAGETYP` header priority over the filename by design. Got:
```
AssertionError: assert 'light' == 'derived'
```
This was a bug in my test's expectation, not in `inventory.py` — fixed the assertion to `frame_type == "light"`, which is actually the stronger proof (it shows the header was genuinely parsed and passed through `classify()`, not just that some value survived).

**GREEN** — `.venv/bin/python -m pytest tests/test_inventory.py -v`:
```
tests/test_inventory.py::test_scan_records_frames PASSED
tests/test_inventory.py::test_scan_is_idempotent PASSED
tests/test_inventory.py::test_colliding_filenames_are_distinct_rows PASSED
tests/test_inventory.py::test_excluded_paths_are_skipped PASSED
tests/test_inventory.py::test_unreadable_file_is_recorded_not_skipped PASSED
tests/test_inventory.py::test_zero_byte_file_is_recorded_not_skipped PASSED
tests/test_inventory.py::test_failed_scan_is_idempotent_and_updates_on_rescan PASSED
tests/test_inventory.py::test_nan_pixels_dont_lose_header_metadata PASSED
8 passed in 0.13s
```

**Full suite** — `.venv/bin/python -m pytest -v`: 43 passed (35 pre-existing from Tasks 1-4 + 8 new), 0 failures, 0 warnings.

## Manual verification of the double-failure case

Ran a standalone script scanning both a 0-byte file and a garbage-content file in one pass to confirm the "both if both failed" requirement literally, since no automated test asserts the exact joined string:
```
counters: added=2 updated=0 failed=2
('Light_empty_0001.fit', 0, 'header: FitsHeaderError: truncated header in .../Light_empty_0001.fit; '
                             'pixels: OSError: Empty or corrupt FITS file')
('Light_garbage_0001.fit', 22, 'header: FitsHeaderError: truncated header in .../Light_garbage_0001.fit; '
                                'pixels: OSError: No SIMPLE card found, this file does not appear to be '
                                'a valid FITS file...')
```
Confirms: both prefixes appear when both reads fail, `failed` increments once per frame (2 frames -> failed=2, not 4), and both frames still count toward `added`.

## Files changed

- `src/astrometa/inventory.py` (new)
- `tests/test_inventory.py` (new)

Commit: `577dceb` — "feat: inventory pass with independent header/pixel reads"

## Self-review (completeness, quality, YAGNI, test honesty)

- **Completeness**: every consumed interface from the brief (`db.connect`/`init_schema` via tests, `fitsheader.read_header`, `classify.classify` called with `path.name` as instructed, `imagekeys.content_hash`/`fingerprint`/`pixel_stats`) is exercised. All global constraints (no archive writes, exclusion markers, thumbnail skip, idempotency, no whitespace-splitting) are met and, where feasible, tested.
- **Quality**: introduced two small named helpers (`_read_header`, `_read_pixels`) instead of inlining both try blocks directly in `scan()` — keeps `scan()` readable and makes the "atomic assignment on full success only" logic for the pixel side self-contained and independently reasoned about. Preserved the brief's original SQL/param structure verbatim (didn't touch column order) to minimize risk of introducing a param-binding bug.
- **YAGNI**: did not add retry logic, logging, progress reporting, or a `--dry-run` mode — none of that was asked for. Did not add a schema column or migration, per the explicit instruction not to. Left `content_hash()` unwrapped (no try/except around it) — the correction was scoped to header vs. pixel reads only, and a raw byte-read failure (e.g. file vanished mid-scan) is a different failure class the brief didn't ask me to handle specially; wrapping it would be scope creep and would also mask a genuinely fatal condition (can't even hash the file) versus the two recoverable, expected failure modes the correction targets.
- **Test honesty**: caught and fixed one wrong assumption in my own NaN test (see RED section above) rather than loosening the assertion to make it pass — the fix made the assertion stronger, not weaker. The double-failure prefix format is verified by direct execution (shown above) even though no single automated test asserts the exact joined string with both prefixes; I judged that one dedicated automated test for the joined-string format would be redundant with the already-passing per-side tests (`test_unreadable_file...`/`test_zero_byte_file...` each individually produce both prefixes today given how `_read_header`/`_read_pixels` are structured) but chose to verify manually anyway since it's the correction's most specific literal requirement.

## Concerns

None blocking. Two things worth flagging for later tasks, not fixed here since they're out of this task's scope:

1. `imagekeys.fingerprint()` does not raise on a single embedded NaN in a large-enough array (confirmed empirically in the NaN test: `pixel_stats` is what catches it, not `fingerprint`) — the atomic-commit structure in `_read_pixels` means this doesn't leak a bad fingerprint into the DB, but it's worth Task 4's owner knowing `fingerprint()` alone is not NaN-safe if it's ever called outside this pairing.
2. `fits.getdata()` on the derived/stacked-frame path re-parses the FITS structure independently of `fitsheader.read_header()`'s raw parser — two different parsers reading the same file for two different purposes (astropy for pixel array shape/dtype, the custom parser for header cards). This is inherited from the original brief's reference implementation, not something I introduced, and changing it would be a larger redesign than this task's scope.

---

## Fix round 1 (post-review)

Review approved with two Important findings; the team lead verified both against the real archive and ruled fix both, and separately measured and parked a third (read-amplification) as not worth doing. This section covers the two fixes.

### FIX 1: `.fit`-only glob silently dropped `.fits` files

`_iter_fits` used `root.rglob("*.fit")`, a pattern that only matches the literal `.fit` extension. The team lead sampled the real archive and found 6,290 `.fits` files (a different rig's naming convention, e.g. `Light_M57_20250811_030222_#0001_0054_300.0s_Gain100_HDR_Bin1_Temp-10.0.fits`) against 28,123 `.fit` — roughly 22% of the science data was never seen by the walk, despite having valid `IMAGETYP` headers that `classify()` already handles correctly.

**Change**: replaced the glob pattern with a suffix check against a set, `p.suffix.lower() in (".fit", ".fits")`, walking `root.rglob("*")` instead of `root.rglob("*.fit")`. This is deliberately extension-agnostic in casing (`.fit`, `.Fit`, `.FIT`, `.fits`, `.Fits`, `.FITS`, ... all match) since the correction explicitly called out "upper-case variants." `.xisf` is not in the set — PixInsight's own format, needs a different parser, and is processed intermediates rather than subs, per the explicit "do not extend" instruction. The existing `_thn` and `_excluded()` (quarantine/`@Recycle`) filters are applied identically to the wider match, unchanged.

### FIX 2: a missing root was silently skipped

`_iter_fits` had `if not root.exists(): continue`. The team lead traced the concrete failure mode: the CLI calls `scan()` then hands `res.seen_hashes` to a `mark_missing` pass (a later task). If a root is an unmounted bind mount, `root.exists()` is `False`, the loop just moves on, and `scan()` returns cleanly with `added=0` and an **empty** `seen_hashes` — `mark_missing` would then flip every one of ~28,000 already-known rows to `missing`, indistinguishable from the operator having deleted the entire archive. `missing` is the exact status this system uses to record a deliberate cull, so this silent path was actively dangerous, not just an omission.

**Change**: a missing root now raises `FileNotFoundError(f"inventory root does not exist: {root}")` immediately, naming the path, before any file under it (or any subsequent root in the list) is processed. Since `scan()` never calls `conn.commit()` until the very end, an exception here means the whole call fails without partial results being persisted or handed to a caller — there is no path by which a missing root can produce a "successful" `InventoryResult` at all.

I did not treat either configured root as optional; per the team lead's ruling, both are read-only bind mounts that must be present, and their absence must be loud.

### Covering tests added (`tests/test_inventory.py`)

- `test_fit_and_fits_extensions_are_scanned_case_insensitively` — parametrized over `.fit`, `.fits`, `.FIT`, `.FITS`, `.Fits`; each independently asserts `res.added == 1`.
- `test_xisf_extension_is_not_scanned` — a `.xisf` file in the tree produces `added == 0, failed == 0`, guarding against accidentally widening the match to PixInsight's format.
- `test_missing_root_raises_loudly_instead_of_scanning_empty` — `scan(conn, [missing_path])` raises `FileNotFoundError` whose message contains the missing path (via `pytest.raises(..., match=str(missing))`).
- `test_missing_root_raises_even_when_another_root_is_valid` — a valid root plus a missing one still raises, so a later broken mount can't be masked by an earlier working one.

### TDD evidence

**RED** — `.venv/bin/python -m pytest tests/test_inventory.py -v -k "fits_extensions or xisf or missing_root"`, before the fix (against the old `*.fit`-glob / silent-`continue` code):
```
tests/test_inventory.py::test_fit_and_fits_extensions_are_scanned_case_insensitively[.fit] PASSED
tests/test_inventory.py::test_fit_and_fits_extensions_are_scanned_case_insensitively[.fits] FAILED
tests/test_inventory.py::test_fit_and_fits_extensions_are_scanned_case_insensitively[.FIT] FAILED
tests/test_inventory.py::test_fit_and_fits_extensions_are_scanned_case_insensitively[.FITS] FAILED
tests/test_inventory.py::test_fit_and_fits_extensions_are_scanned_case_insensitively[.Fits] FAILED
tests/test_inventory.py::test_xisf_extension_is_not_scanned PASSED
tests/test_inventory.py::test_missing_root_raises_loudly_instead_of_scanning_empty FAILED
tests/test_inventory.py::test_missing_root_raises_even_when_another_root_is_valid FAILED
6 failed, 2 passed, 8 deselected
```
Each `.fit`/`.fits`-variant failure was `assert 0 == 1` (`res.added` stayed 0 — the file was never seen). Both missing-root failures were `Failed: DID NOT RAISE FileNotFoundError` — confirming the silent-`continue` behavior. `.fit` (already matched before the fix) and `.xisf` (never matched, correctly) passed as expected — they're regression guards, not new-behavior assertions.

**GREEN** — `.venv/bin/python -m pytest tests/test_inventory.py -v`, after the fix:
```
16 passed in 0.15s
```
(8 tests from the original implementation + 8 new from this fix round, all passing.)

**Full suite** — `.venv/bin/python -m pytest -v`:
```
51 passed in 0.17s
```

### PARKED item (no action taken here)

The read-amplification finding (content_hash read + `fits.getdata()` read = two full reads per file) was measured by the team lead directly (0.175 s/frame vs. 0.176 s/frame for a single-buffer variant, page-cache-served second read, CPU-bound not I/O-bound) and explicitly ruled "do not refactor." No code change made for it; noted here only so the ledger and this report agree.

### Files changed (this round)

- `src/astrometa/inventory.py` — `_iter_fits`: suffix-based extension match (`.fit`/`.fits`, any case) instead of `rglob("*.fit")`; missing root raises `FileNotFoundError` instead of `continue`.
- `tests/test_inventory.py` — 8 new tests as listed above.

Commit: `4065bfa` — "fix: match .fits case-insensitively and raise loudly on a missing root"
