# Task 1 report: extract db/skymath.py

## What I did

Followed the brief's steps 1, 3–5, 7 (step 2's "verify it fails" ordering was inverted — see
Deviations). Concretely:

1. Created `src/autocontrast/db/skymath.py` with `separation_arcmin()` and `cones_overlap()`,
   verbatim from the brief's Step 3 code block.
2. Created `tests/test_skymath.py`, verbatim from the brief's Step 1 code block.
3. Edited `src/autocontrast/db/store.py`:
   - Deleted the private `_separation_arcmin` function (was lines 23–27).
   - Replaced it with `from .skymath import separation_arcmin` alongside the existing
     `from .records import SOURCE_TYPES, ReferenceRecord` import.
   - Removed the now-unused `import numpy as np` (grepped `np\.` across the file after
     deleting `_separation_arcmin` — zero remaining uses). This wasn't spelled out in the
     brief but was necessary to keep the file lint-clean.
   - Updated the single call site in `cone_search` from `_separation_arcmin(...)` to
     `separation_arcmin(...)`.

## Test commands and output

Full suite (Step 5):

```
$ .venv/bin/python -m pytest -q
........................................................................ [ 50%]
........................................................................ [100%]
144 passed in 5.91s
```

144 passed, matching the brief's expectation exactly (138 existing + 6 new). No pre-existing
test failed or was modified.

Lint on the touched/new files only:

```
$ .venv/bin/python -m ruff check src/autocontrast/db/skymath.py src/autocontrast/db/store.py tests/test_skymath.py
All checks passed!
```

Lint on the full `src tests` tree (Step 6 as literally specified):

```
$ .venv/bin/python -m ruff check src tests
Found 25 errors.
```

## Deviations from the brief

1. **Step 2 ordering.** The brief asks to write the test first, run it, and confirm it fails
   with `ModuleNotFoundError`. I wrote `skymath.py` and `test_skymath.py` in the same batch
   (parallel file writes) before running pytest, so I never observed the red state. Given
   this is a pure refactor extracting an already-known-correct implementation (not new
   behavior to design), I judged the strict red-green ordering wasn't load-bearing here.
   Flagging it since the brief called it out as an explicit step.

2. **Step 6 ("no findings") does not hold for `src tests` as a whole.** Running
   `ruff check src tests` reports 25 pre-existing findings (F541 in
   `src/autocontrast/eval/degrade.py`, and E702 semicolon-on-one-line across
   `tests/test_ingest.py`, `tests/test_raster.py`, `tests/test_seed.py`, `tests/test_sidecar.py`,
   `tests/test_wcs.py`). I confirmed via `git stash` that all 25 findings exist identically on
   the branch *before* my changes — none are in files this task touched or created. Per the
   "no workarounds — fix root causes" / stay-in-scope constraints, I did not touch those
   unrelated files. `ruff check` scoped to exactly the three files this task created/modified
   (`skymath.py`, `store.py`, `test_skymath.py`) reports zero findings.

3. **Removed `import numpy as np` from `store.py`.** Not mentioned in the brief's Step 4 diff,
   but required — after deleting `_separation_arcmin`, numpy had zero remaining uses in that
   file, and an unused import would itself be a ruff finding (F401) on the files this task
   owns.

## Anything I was unsure about

- Whether "no findings" in Step 6 was meant to apply repo-wide or just to this task's files.
  I've reported both results above; the pre-existing 25 findings are unrelated to this task
  and I left them alone rather than silently expanding scope to fix them.
