# Task 6 report: the position index

## What I did

Followed the brief's TDD steps in order:

1. Wrote `tests/test_gallery_index.py` verbatim from the brief.
2. Ran it, confirmed the expected failure: `ModuleNotFoundError: No module named 'autocontrast.db.discover.index'`.
3. Wrote `src/autocontrast/db/discover/index.py` verbatim from the brief — `GalleryIndex`
   (`upsert`, `get`, `has`, `count`, `cone_search`, `get_sync_state`, `set_sync_state`, `close`)
   and `IndexMatch`. Reuses `separation_arcmin`/`cones_overlap` from `..skymath` (Task 1) and
   `GalleryEntry` from `.gallery` (Task 3) without reimplementing either. `_COLUMNS` is derived
   from `dataclasses.fields(GalleryEntry)`.
4. Ran the test file — 11/12 passed; `test_cone_search_handles_ra_wrap` failed.

## Deviation from the brief, and why

**The brief's `test_cone_search_handles_ra_wrap` was arithmetically impossible as written.**

Original:
```python
index.upsert(entry(ra=359.9, dec=0.0, radius=5.0))
assert len(index.cone_search(0.1, 0.0, radius_arcmin=5.0)) == 1
```

Verified: `separation_arcmin(0.1, 0.0, 359.9, 0.0) = 12.0` arcmin (consistent with the docstring's
own claim). `cones_overlap` is boundary-inclusive on `sep <= radius_a + radius_b`; here
`radius_a + radius_b = 5.0 + 5.0 = 10.0`, and `12.0 > 10.0`, so the cones genuinely do not
overlap under the Task 1 definition this task is required to reuse. The test's own numbers
contradicted its own docstring's stated intent (RA wrap must be caught, i.e., the entry must
be found).

I stopped rather than force a pass by loosening `cones_overlap`/`skymath.py` (explicitly
forbidden — that logic is shared with `FingerprintStore.cone_search` and is correct) or by
picking new numbers myself, and escalated to the team lead per their instruction to ask
rather than guess on anything that looks wrong.

**Team lead's ruling** (reproduced for the record): the defect was in the plan's fixture
values, not the intent or the shared math. Fix applied, per their explicit direction:

```python
def test_cone_search_handles_ra_wrap(index):
    """The dec-band SQL prefilter cannot express RA wrap, so the precise haversine
    check must catch it: 359.9 and 0.1 are 12 arcmin apart, not 359 degrees.

    The separation assertion is the load-bearing one — a wrap-broken implementation
    computes |359.9 - 0.1| * 60 = 21588 arcmin, so asserting ~12 discriminates a
    correct implementation from a wrong one in a way a bare match count cannot.
    """
    index.upsert(entry(ra=359.9, dec=0.0, radius=20.0))
    matches = index.cone_search(0.1, 0.0, radius_arcmin=5.0)
    assert len(matches) == 1
    assert matches[0].separation_arcmin == pytest.approx(12.0, abs=0.1)
```

Two changes from the brief: entry `radius` 5.0 → 20.0 (so `12 <= 5 + 20` genuinely holds —
20.0 is also the `entry()` helper's own default), and a new `separation_arcmin` assertion
that discriminates a correct short-way-round haversine computation from a wrap-broken one
(a naive `|ra1 - ra2| * 60` would yield ~21588 arcmin, not ~12), which a bare match-count
assertion alone could not have caught. `skymath.py` and `cones_overlap` were left untouched,
as directed.

No other deviations. Implementation file matches the brief's Step 3 code exactly, including
the SQL dec-band prefilter comment and the deliberate separate-SQLite-file design.

## Test commands and output

```
$ .venv/bin/python -m pytest tests/test_gallery_index.py -v
...
12 passed in 0.06s
```

```
$ .venv/bin/python -m pytest
...
224 passed in 5.92s
```
(212 pre-existing + 12 new = 224, matches expectation.)

```
$ .venv/bin/python -m ruff check src/autocontrast/db/discover/index.py tests/test_gallery_index.py
All checks passed!
```
(Lint scoped to the two files this task touched, per the brief's constraint not to use
repo-wide ruff as a gate — the 25 pre-existing findings elsewhere are untouched.)

## Anything I was unsure about

Only the RA-wrap test numbers, covered above and resolved by the team lead's ruling before
implementation was finalized. Everything else in the brief matched the actual interfaces
in the repo (`GalleryEntry` field order, `skymath.separation_arcmin`/`cones_overlap`
signatures and boundary-inclusive semantics) exactly as described, so no other judgment
calls were needed.
