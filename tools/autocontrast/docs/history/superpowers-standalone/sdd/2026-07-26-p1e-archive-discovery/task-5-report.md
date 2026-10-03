# Task 5 report: Parse listing pages

## What I did

Followed the brief's TDD steps exactly, no deviation.

1. Verified the existing state of `src/autocontrast/db/discover/gallery.py` (Tasks 1-4
   already committed: `GalleryConfig`, `GALLERIES`, `text_lines`, `labelled_value`,
   coordinate parsers, `GalleryEntry`, `parse_detail`, `parse_credit`,
   `asserts_copyright`, `parse_filter_bands`, `parse_size_px`, `_image_url`, and the
   `_html` import alias). Confirmed `parse_listing`/`ListingItem`/`parse_result_total`
   did not yet exist.
2. Inspected both committed fixtures directly (`grep`/small Python snippet) to confirm
   ground truth before trusting the brief's numbers:
   - `hubble_listing_orion.html`: 35 `{...}` records inside `var images = [...]`,
     `heic0601a` has `width: 18000`, `height: 18000`, `url: '/images/heic0601a/'`,
     title `Hubble&#39;s sharpest view of the Orion Nebula`. Page text contains
     "Showing 1 to 35 of 35".
   - `eso_listing_orion.html`: 26 records, `eso1103a` has `width: 8948`, `height: 8597`,
     `url: '/public/images/eso1103a/'`. Page text contains "Showing 1 to 26 of 26".
   All matched the brief's expected ground truth exactly.
3. Wrote `tests/test_gallery_listing.py` verbatim from the brief (Step 1).
4. Ran the new test file and observed the expected failure:
   `ImportError: cannot import name 'parse_listing' from 'autocontrast.db.discover.gallery'`
5. Appended `ListingItem`, `parse_listing`, `parse_result_total` to
   `src/autocontrast/db/discover/gallery.py` verbatim from the brief (Step 3), reusing
   the existing `_html` alias and `re` import — no new imports added.
6. Re-ran the test file: all 9 passed.
7. Ran the full suite: 209 passed (200 pre-existing + 9 new — no parametrize expansion
   in this file, so the count matches 1:1).
8. Ran `ruff check` scoped to only the two files this task touched (not repo-wide, per
   the brief's constraint about the 25 pre-existing findings elsewhere): clean.
9. Committed both files together with the exact commit message from the brief.

## Commands run and output

```
$ .venv/bin/python -m pytest tests/test_gallery_listing.py -v
... (before implementation)
ImportError: cannot import name 'parse_listing' from 'autocontrast.db.discover.gallery'
1 error in 0.06s

$ .venv/bin/python -m pytest tests/test_gallery_listing.py -v
... (after implementation)
9 passed in 0.02s

$ .venv/bin/python -m pytest -q
209 passed in 5.90s

$ .venv/bin/python -m ruff check src/autocontrast/db/discover/gallery.py tests/test_gallery_listing.py
All checks passed!
```

## Deviations from the brief

None. Test file, implementation, and commit message all match the brief verbatim.

## Things I was unsure about

- The brief said "Expected: PASS, 9 tests" and the team-lead message separately warned
  that test counts in the brief may undercount parametrize expansion elsewhere in the
  plan. This file has no `@pytest.mark.parametrize`, so 9 written tests == 9 collected
  tests == 9 passed. No discrepancy to flag.
- I independently verified the fixture ground truth (record counts, specific field
  values for `heic0601a` and `eso1103a`, the "Showing X of Y" lines) rather than trusting
  the brief blindly, since a previous task on this plan (§2.2 downsample scale) turned
  up a real bug from an unverified assumption. Everything checked out exactly as stated,
  so no adjustment was needed.
- Nothing else was ambiguous — the brief's code, test file, and commit message were
  complete and self-consistent with the interfaces already in the file from Tasks 1-4.

## Commit

`567b703` — "discover: parse listing pages from the inline JS data literal"

## Fix round 1

**Finding (Important):** `test_navigation_links_are_not_mistaken_for_results` passed
against the real fixtures without ever exercising the `if not entry_id or not path:
continue` guard — in both fixtures every record already carries both `id:` and `url:`,
and nav-link ids never appear as a record's `id:` value (they live outside the `var
images` block entirely, excluded by construction). Same gap applied to two other
never-exercised paths: a malformed record missing `id`/`url`, and a record missing
`width`/`height` where the numeric field must yield `None`, not a bogus `0` (a later
task computes `fov_arcmin * 60 / width_px`, so a silent `0` would be a divide-by-zero
or a garbage pixel scale corrupting downstream band-limiting).

**Fix.** No production code changed — the implementation was already correct; this was
a test-coverage gap. In `tests/test_gallery_listing.py`:

1. Renamed `test_navigation_links_are_not_mistaken_for_results` to
   `test_navigation_links_never_enter_the_results_block`, with a docstring stating what
   it actually demonstrates (nav links sit outside `var images`, excluded by
   construction) and explicitly noting it does not exercise the guard clause.
2. Added `_SYNTHETIC_LISTING_TEMPLATE` and a `_record(**fields)` helper that builds a
   synthetic `var images = [...]` block matching the real JS-literal format exactly
   (unquoted keys, single-quoted string values, trailing `potw: ''`), since neither real
   fixture contains a malformed or dimension-less record.
3. Added `test_record_missing_id_is_skipped_but_siblings_still_parse` — a two-record
   synthetic block, one missing `id:`; asserts only the well-formed sibling appears and
   `len(items) == 1`.
4. Added `test_record_missing_url_is_skipped_but_siblings_still_parse` — same shape,
   missing `url:` instead.
5. Added `test_record_missing_dimensions_yields_none_not_zero` — a record with `id`/
   `url`/`title`/`potw` but no `width`/`height`; asserts `width_px is None` and
   `height_px is None` (explicitly `is None`, not `== 0`).

Confirmed each of the two "skipped" tests is non-vacuous: with the guard clause removed,
the malformed record would parse with `id=None` (test 3) or `detail_path=None` (test 4),
so the `{i.id: i for i in ...}` dict would gain a second entry and `len(items) == 1`
would fail. The dimensions test is already guaranteed non-vacuous by the existing
`number()` implementation (`int(match.group(1)) if match else None` — no zero fallback
exists to accidentally pass).

**Commands run and output:**

```
$ .venv/bin/python -m pytest tests/test_gallery_listing.py -v
tests/test_gallery_listing.py::test_hubble_listing_yields_every_result PASSED
tests/test_gallery_listing.py::test_eso_listing_yields_every_result PASSED
tests/test_gallery_listing.py::test_listing_publishes_dimensions_used_for_pixel_scale PASSED
tests/test_gallery_listing.py::test_eso_detail_paths_carry_the_public_prefix PASSED
tests/test_gallery_listing.py::test_titles_are_html_unescaped PASSED
tests/test_gallery_listing.py::test_listing_contains_the_curated_seed_ids PASSED
tests/test_gallery_listing.py::test_navigation_links_never_enter_the_results_block PASSED
tests/test_gallery_listing.py::test_record_missing_id_is_skipped_but_siblings_still_parse PASSED
tests/test_gallery_listing.py::test_record_missing_url_is_skipped_but_siblings_still_parse PASSED
tests/test_gallery_listing.py::test_record_missing_dimensions_yields_none_not_zero PASSED
tests/test_gallery_listing.py::test_result_total_is_read_from_the_page PASSED
tests/test_gallery_listing.py::test_page_without_a_results_block_yields_nothing_rather_than_raising PASSED
12 passed in 0.02s

$ .venv/bin/python -m pytest -q
212 passed in 5.88s

$ .venv/bin/python -m ruff check src/autocontrast/db/discover/gallery.py tests/test_gallery_listing.py
All checks passed!
```

**Commit:** `24694c9` — "test(gallery): cover skip/None-vs-0 paths in parse_listing with synthetic fixtures"
(no production code change; `src/autocontrast/db/discover/gallery.py` untouched this round).

Note: `.superpowers/sdd/2026-07-26-p1e-archive-discovery/` is gitignored in this repo, so
this report file itself is not part of the commit above — only the test file is.
