# Task 4 Report: Palette class from gallery filter bands

## What I did

Followed the brief's TDD steps exactly, verbatim:

1. Wrote `tests/test_palette_gallery.py` exactly as specified in the brief (17 test
   functions/cases; the brief's "18 tests" count is a counting artifact of the two
   `@pytest.mark.parametrize` blocks — confirmed with the team lead's note before running).
2. Ran the test file and confirmed it failed with the expected import error before writing
   any implementation.
3. Appended `palette_class_from_gallery_bands`, `_GALLERY_BROADBAND`, and
   `_GALLERY_NARROWBAND` to `src/autocontrast/fingerprint/palette.py`, verbatim from the
   brief's Step 3. Did not modify `palette_class_from_filters` or any other existing code
   in the file.
4. Re-ran the new tests: all pass.
5. Ran the existing `tests/test_palette.py` unchanged: all pass (15 tests, same as before).
6. Ran the full suite: 184 passed (167 pre-existing + 17 new — matches expectation).
7. Ran `ruff check` scoped only to the two files this task touched (per the "lint only what
   you touch" constraint) — clean, no findings.
8. Committed both files together with the exact commit message from the brief's Step 6.

## Commands and output

```
$ .venv/bin/python -m pytest tests/test_palette_gallery.py -v
... 17 passed in 0.01s

$ .venv/bin/python -m pytest tests/test_palette.py -q
15 passed in 0.01s

$ .venv/bin/python -m pytest -q
184 passed in 5.83s

$ .venv/bin/python -m ruff check src/autocontrast/fingerprint/palette.py tests/test_palette_gallery.py
All checks passed!
```

Commit: `2cbb3f3` — "§4.3: derive palette class from gallery-published filter bands"
(2 files changed, 119 insertions(+), 0 deletions(-) — pure addition, no modification to
existing lines in `palette.py`).

## Deviations from the brief

None. Implementation and test file were written verbatim from the brief's code blocks.

## The load-bearing asymmetry

Confirmed and preserved exactly as instructed: `palette_class_from_gallery_bands([])`
returns `"unknown"` via its own early `if not bands: return "unknown"` guard — it never
calls or delegates to `palette_class_from_filters`. The two functions remain fully
independent; `palette_class_from_filters` was not touched (diff shows zero changes to any
pre-existing line in the file, only new code appended below the existing `return
"unknown"` at the end of that function).

## Concerns for the team lead (AUTHOR DECISION callout)

The brief flags, and I want to surface explicitly: the default policy — classifying a
broadband-dominated composite as `RGB` even when a narrowband (Ha) layer is blended in —
is a domain judgment with a real cost. It means a reference like heic0601a, which has an
H-alpha layer contributing real chroma information, will be treated as a plain `RGB`
reference by any §2.3 gate that checks palette-class equality. If the acquisition's own
palette class is `HaRGB`, that gate will read `RGB != HaRGB` and reject/downgrade a
reference that is otherwise a very close match, purely because of this classification
choice — even though the "RGB" label is defensible per the curated catalog's own naming.

I implemented the default exactly as specified (I did not substitute my own policy), and
all tests pass. I'm flagging this per the brief's own instruction to raise it rather than
silently pick a different default. No action taken beyond noting it.
