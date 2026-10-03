# Task 3 report: fixtures + detail page parsing

## What I did

1. **Fixture capture script** — wrote `tools/capture_gallery_fixtures.sh` verbatim from the
   brief, made it executable, and ran it. It fetched six real pages from esahubble.org and
   eso.org at one request per second with the specified `AutoContrast/0.1 (...)` User-Agent.
   No retries were needed — all six requests succeeded on the first attempt.

2. **Test file** — wrote `tests/test_gallery_detail.py` verbatim from the brief (16 test
   functions, no `@pytest.mark.parametrize`, so 16 collected == 16 run).

3. **Ran the test to confirm the expected failure.** Got
   `ImportError: cannot import name 'asserts_copyright' from 'autocontrast.db.discover.gallery'`
   rather than the brief's predicted `parse_detail` — a cosmetic difference only (the `from
   ... import (...)` block imports names alphabetically, and `asserts_copyright` sorts before
   `parse_detail`). Confirms the module didn't yet have any of the new names, as expected.

4. **Implementation** — appended to `src/autocontrast/db/discover/gallery.py`:
   `GalleryEntry`, `parse_credit`, `asserts_copyright`, `parse_filter_bands`, `parse_size_px`,
   `_image_url`, `parse_detail`. Added
   `from autocontrast.fingerprint.palette import palette_class_from_gallery_bands` to the
   top-of-file import block (Task 4 was already merged on this branch, so no stub was
   needed or created, per the team lead's note). Checked for circular imports first —
   `palette.py` imports nothing from this package, so none exists.

5. **One deviation from the brief's code, found via TDD, not guessed:** see "Deviation"
   below — `parse_filter_bands` needed a one-line fix beyond what the brief specified,
   discovered because the test failed on real fixture data on the first attempt.

6. Ran the full suite and ruff scoped to the two touched files, then committed.

## Deviation from the brief, and why

The brief's `parse_filter_bands` docstring assumes a 4-column row shape
(`Optical B | 435 nm | Hubble Space Telescope | ACS`) and filters cells with
`cells = [c for c in cells if c]` before checking `if not cells or cells[0]...`.

The real `heic0601a` page's "Colours & filters" table is actually 3 columns (Band,
Wavelength, Telescope) — the instrument text ("ACS") lives inside the *Telescope* cell, not
a separate 4th column. More importantly, the live table ends with a malformed trailing
`<tr>` — a template artifact, not a filter row — whose Band and Wavelength `<td>`s are
empty but whose Telescope `<td>` still contains `Hubble Space Telescope<br/>ACS`:

```html
<tr><td><span></span></td><td></td><td>
    Hubble Space Telescope
    <br/><span class="band_instrument">ACS</span></td></tr>
```

With the brief's exact code, `[c for c in cells if c]` drops the two empty cells, leaving
`cells == ["Hubble Space Telescope ACS"]` — non-empty and not starting with "band" — so the
loop appended a spurious 6th band, `"ACS"`, making `parse_filter_bands` return
`["B", "V", "H-alpha", "I", "Z", "ACS"]` against the test's expected `["B", "V", "H-alpha",
"I", "Z"]`. (`test_palette_is_derived_from_published_filters` passed regardless, since
`palette_class_from_gallery_bands` ignores unrecognized band tokens — only
`test_filter_bands_are_extracted`, which checks the list verbatim, caught it.)

**Fix:** stopped discarding empty cells before the check; instead required the *first* cell
(the Band cell) specifically to be non-empty:

```python
if not cells or not cells[0] or cells[0].lower().startswith("band"):
    continue
```

This correctly skips both the header row (`cells[0] == "Band"`) and the malformed trailing
row (`cells[0] == ""`), while still processing all 5 real filter rows. Updated the
docstring to describe the actual 3-column shape and note the malformed row explicitly.

This is a live-site finding, not a spec question: the brief's own comment above Step 2
anticipated exactly this ("Commit 12e7041 shipped two bugs past 138 green tests because
every fixture was a 2D mono FITS — the gap was fixture diversity, not coverage"). Real
fixture data surfaced a real edge case the hand-written example didn't cover; I fixed it
rather than adjusting the test to accommodate a wrong answer.

## Other observation (not a deviation, but worth flagging per the report contract)

`data/seed_catalog.json`'s hand-built entry for `opo0205c` lists `"license": "CC BY 4.0"`.
The live-parsed credit line is `"Copyright © Anglo-Australian Observatory. Photograph by
David Malin"`, which `asserts_copyright` correctly flags as `True`, so `parse_detail`
correctly returns `license=None` for this entry — contradicting the seed catalog's naive
license field. This is not a bug in either the parser or the test (the test asserts
`license is None` and that's what happens); it's confirmation of exactly the finding the
team lead's brief called out as property #2 (licensing must fail closed). The seed
catalog's `CC BY 4.0` for opo0205c was itself wrong and this parser now catches it. Flagging
in case a downstream task cross-checks `GalleryEntry.license` against the seed catalog and
is surprised by the mismatch — the mismatch is correct.

## Fixtures captured

All six landed inside the brief's expected 40–90 KB range; nothing empty or truncated.

| File | Bytes |
|---|---|
| `hubble_listing_orion.html` | 49,062 |
| `eso_listing_orion.html` | 84,007 |
| `hubble_detail_heic0601a.html` | 60,850 |
| `eso_detail_eso1103a.html` | 85,970 |
| `hubble_detail_opo0205c.html` | 41,048 |
| `hubble_detail_heic0211i.html` | 42,512 |

Spot-checked (not just trusted): none of the six contain an HTTP-error body; the detail
pages carry real `Id`/`Name` labels; `opo0205c`'s credit line and `heic0211i`'s `Type:
Artwork` were confirmed present in the raw HTML exactly as the test expects.

## Commands run and output

```
$ .venv/bin/python -m pytest tests/test_gallery_detail.py -v
...
tests/test_gallery_detail.py::test_hubble_position_matches_the_curated_catalog PASSED
tests/test_gallery_detail.py::test_eso_position_matches_the_curated_catalog PASSED
tests/test_gallery_detail.py::test_footprint_radii_match_the_curated_catalog PASSED
tests/test_gallery_detail.py::test_pixel_scale_is_derivable_from_published_metadata PASSED
tests/test_gallery_detail.py::test_image_urls_point_at_the_cdn PASSED
tests/test_gallery_detail.py::test_names_types_and_release_dates PASSED
tests/test_gallery_detail.py::test_palette_is_derived_from_published_filters PASSED
tests/test_gallery_detail.py::test_filter_bands_are_extracted PASSED
tests/test_gallery_detail.py::test_size_is_published_as_a_cross_check_on_listing_dimensions PASSED
tests/test_gallery_detail.py::test_attribution_is_the_full_credit_line PASSED
tests/test_gallery_detail.py::test_license_defaults_to_the_gallery_license_when_no_copyright_is_asserted PASSED
tests/test_gallery_detail.py::test_render_with_no_published_position_yields_nulls_not_a_guess PASSED
tests/test_gallery_detail.py::test_copyrighted_render_is_flagged_so_it_is_never_ingested_as_cc_by PASSED
tests/test_gallery_detail.py::test_permissive_credits_are_not_flagged_as_copyrighted PASSED
tests/test_gallery_detail.py::test_artwork_has_no_position_and_no_filters PASSED
tests/test_gallery_detail.py::test_a_page_that_does_not_parse_at_all_reports_failure PASSED
============================== 16 passed in 0.04s ==============================

$ .venv/bin/python -m pytest -q
........................................................................ [ 36%]
........................................................................ [ 72%]
........................................................                 [100%]
200 passed in 5.88s

$ .venv/bin/python -m ruff check src/autocontrast/db/discover/gallery.py tests/test_gallery_detail.py
All checks passed!
```

Suite went from 184 (baseline, confirmed before starting) to 200 (+16, matching the
brief's estimate exactly — no parametrize expansion in this file).

## Things I was unsure about, resolved by checking rather than guessing

- Whether to add the `palette_class_from_gallery_bands` import at the top of the file (PEP
  8 / this repo's own convention, as seen in every other module) versus inline near
  `GalleryEntry` as the brief's code block literally shows it. Put it at the top, after
  `from __future__ import annotations` and the stdlib/dataclass/urlencode imports — matches
  how every other file in this codebase orders imports, and avoids a mid-file import that
  ruff or a future reader would flag.
- Whether the `parse_filter_bands` fix (see Deviation) was in scope for this task or should
  be escalated. Decided it's squarely in scope: the brief's own governing test
  (`test_filter_bands_are_extracted`) specifies the exact correct output, TDD surfaced the
  gap against real data, and the fix is a one-line tightening of an existing filter with no
  API change — exactly the kind of "written spec vs. real world" gap this SDD plan expects
  fixture-driven tests to catch.

## Commit

```
git add tools/capture_gallery_fixtures.sh tests/fixtures/gallery \
        src/autocontrast/db/discover/gallery.py tests/test_gallery_detail.py
git commit -m "discover: parse gallery detail pages; six committed fixtures
..."
```
(full message as specified in the brief, plus a short deviation note — see the actual
commit for the exact text).
