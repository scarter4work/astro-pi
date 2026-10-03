# Task 12 report — offline end-to-end over real reference data

Status: **DONE**. File: `tests/test_optimize_offline_real.py`.

## Reference resolution

`FingerprintStore('data/fingerprints.sqlite').get('eso1103a')` resolves;
`get('esa_hubble:opo9545a1')` returns `None` (confirmed, matches the
controller pre-flight). So every test in this file runs against `rid =
'eso1103a'`, a broadband RGB ESO press-release image, `palette_class='RGB'`,
`pixel_scale_arcsec=1.7746866666666667`, `psf_fwhm_arcsec=2.0`.

## Provenance shape (as predicted by the pre-flight)

`rec.provenance` = `{'source_url': 'https://www.eso.org/public/images/eso1103a/',
'license': 'CC BY 4.0', 'attribution': 'ESO / Igor Chekalin', 'ingested_utc':
'', 'wcs_source': 'manual'}` — no `cache_file` key, as briefed. Traced the
actual cache-naming convention to `discover.py`'s `_cached_image`: the file is
written as `{entry_id}{suffix}` where `entry_id` IS the record's own `id`. So
the test derives the cache path as `data/discovery_cache/{rid}.*` (globbed,
in case the suffix isn't `.jpg`) rather than reading anything out of
provenance. `data/discovery_cache/eso1103a.jpg` is the only match.

## Step 3 change from the brief's literal Step 1 code

Replaced every `"data/discovery_cache/" + rec.provenance["cache_file"]` with
a `_cached_raster_path(rid)` helper (globs `{rid}.*`, `pytest.fail`s with the
Phase 1 discovery instruction if nothing matches — never skips). Also,
per the controller note, the decline tests use `_cached_raster_path(rid)` for
the SAME `rid` the fixture resolved (never an arbitrary/sorted-first file
from the cache dir) — this is the only way the fail-safe assertion is
meaningful, since two unrelated targets are legitimately far apart and
"improving" them would be correct, not a fail-safe bug.

## Measured results (real run, `.venv/bin/python`, script outside pytest to capture full detail)

### Recovery test: `flatten(source, strength=0.6)` against eso1103a's own fingerprint

- baseline_distance: **0.14295646073201262**
- best distance: **0.08643475565544909**
- improved: **True**
- iterations: **7**
- convergence_reason: `"every branch was discarded by guardrails or executor failure"`
- distance_history: `[0.14296, 0.11741, 0.10299, 0.09724, 0.09222, 0.08794, 0.08643]` (monotone decreasing every iteration)
- final recipe: 6 actions — `Counter({'black_point': 3, 'local_contrast': 2, 'chroma': 1})`
- guardrail_log entries: 112 (attempt-cap notices + guardrail/executor rejections across 7 iterations at top_k=3, max_attempts=9)
- wall time: ~37s (800px max_dim raster, 769x800x3 actual)

### Decline test: eso1103a's own cached render, undegraded, against its own fingerprint

- baseline_distance: **0.017308995224050445**
- best distance: **0.017308995224050445** (unchanged — literally the input, per `outcome()`'s fail-safe path)
- improved: **False**
- iterations: **1**
- convergence_reason: `"every branch was discarded by guardrails or executor failure"`
- recipe actions: `[]`
- guardrail_log entries: 10
- wall time: ~2s

Note the baseline distance for the undegraded render (0.0173) is well below
the flattened render's *best-achieved* distance (0.0864) — i.e. the
recovered/optimized flattened image is still measurably farther from the
reference than the untouched original ever was. This is expected: `flatten`
at strength=0.6 is a heavy degradation and `NumpyExecutor`'s action set is an
approximation (spec §7.4); the test only asserts *directional* recovery
(`distance < baseline_distance`), not full recovery to the pristine floor.

## Observations on the three open calibration questions (NOT tuned around)

1. **`iteration_cap=20`**: **never approached, and NOT what bound this run**.
   My first draft of this report misattributed the recovery run's 7-iteration
   stop to the iteration cap. That is wrong. Re-instrumenting the run
   standalone shows the actual mechanism: the ranked menu for this fixture
   holds **18 distinct `(priority, band, kind)` groups**, and
   `max_attempts = 3 x top_k = 9` — so **each branch attempts only half the
   menu** (9 of 18 groups) before giving up on that iteration. `applied_kinds`
   was not the cause either: it withholds only ONCE_ONLY kinds
   (`star_split`, `background_neutralize`), and neither was used here, so the
   full 18-group menu was available, unshrunk, on every iteration. At
   iteration 7 all 3 branches hit the attempt cap holding 0/3 live candidates,
   with guardrail failures (noise_floor, star_integrity, shadow_clipping,
   highlight_clipping) escalating each iteration — genuine guardrail
   pressure, but 9 of 18 groups per branch were never even attempted.
   `iteration_cap = 20` was never binding in either run (7 and 1 iterations).
   That question — whether 20 is under-sized — stays **OPEN and untouched by
   this task**; the fixture where distance was still falling at iteration 20
   is the one that speaks to it, not this one, and this task did not adjust
   `iteration_cap`, `max_attempts`, or any other tunable to manufacture or
   rescue a data point.

   **Consequence for reading `improved: true` here**: the measured best
   `0.08643` is a floor set by the bounded retry (half the menu, per branch,
   per iteration), not evidence that it is the best achievable recipe over
   the full 18-group menu. `improved: true` means "the loop found a genuine,
   guardrail-clean improvement" — it does not mean "the loop found the
   optimum," and this fixture cannot distinguish the two.
2. **`local_contrast` dominance (previously 15/19)**: NOT reproduced here —
   the recovery recipe is `{black_point: 3, local_contrast: 2, chroma: 1}`,
   i.e. `local_contrast` is a minority (2/6), and `black_point` dominates
   instead. Different fixture, different dominant action — worth noting as a
   second real data point rather than a confirmation or refutation.
3. **"no scale-denominated action" (previously 16/25)**: NOT reproduced as a
   pure case here — `local_contrast` (scale/layer-denominated) DOES appear
   (2 of 6 actions), alongside `black_point` (not scale-denominated) and one
   `chroma` action. So this fixture's recipe is mixed, not scale-absent.

None of these three findings were used to adjust any tunable; they are
reported as observations only, per the brief's explicit instruction.

## What the baseline-vs-best gap does and does not prove

The undegraded render's baseline (0.017309) sits well below the flattened
render's *best-achieved* distance (0.086435) — the optimizer's recovered
output is still ~5x farther from the reference than the pristine original
ever was. This is expected and does **not** indicate a bug or a weak
recovery:

- **What it proves**: the exit criterion the loop and these tests check is
  *measurable improvement* — `result["distance"] < result["baseline_distance"]`
  strictly, by more than `epsilon_improve` — over whatever state the search
  started from. That criterion held: 0.14296 → 0.08643, monotone-decreasing
  at every one of the 7 iterations.
- **What it does NOT prove**: that the loop recovers the flattened proxy all
  the way back to the pristine floor. It does not, and nothing in spec §3.2
  requires it to — `NumpyExecutor` is a cheap approximation of the SS6.2
  actions (spec §7.4), not PixInsight's actual processes, and heavy
  `strength=0.6` flattening destroys information (blurred detail, compressed
  contrast, a lifted floor) that a bounded, guardrailed local search over an
  approximate action set is not guaranteed to fully reconstruct.

This distinction matters for how Task 14's exit criterion should be read: a
live-PI run that improves a real degraded image without fully returning it to
its untouched-original distance is a **pass**, not a partial result — "improved"
means "closer to the reference than where it started," never "recovered to
the theoretical floor."

## Palette gate note

`eso1103a` is broadband RGB, not narrowband/HOO. Since every test in this
file measures the render against its OWN fingerprint, `palette_class`
matches on both sides trivially — the SS2.3 palette-mismatch chroma-drop path
is NOT exercised by this file. (The recovery recipe DOES include a `chroma`
action, confirming the non-gated, palette-compatible path works.) This is a
real gap in offline coverage (this store has no cached HOO reference to pair
against a mismatched proxy), not a defect in these tests — flagged in the
test file's module docstring.

## Test run

`.venv/bin/python -m pytest tests/test_optimize_offline_real.py -v`:

```
tests/test_optimize_offline_real.py::test_flattening_a_real_render_is_recovered PASSED [ 33%]
tests/test_optimize_offline_real.py::test_a_real_professional_render_is_declined PASSED [ 66%]
tests/test_optimize_offline_real.py::test_a_declined_run_explains_itself PASSED [100%]
3 passed in 43.25s
```

Full suite (`.venv/bin/python -m pytest -q`), real output:

```
........................................................................ [ 16%]
........................................................................ [ 33%]
........................................................................ [ 50%]
........................................................................ [ 67%]
........................................................................ [ 84%]
.................................................................        [100%]
425 passed in 253.46s (0:04:13)
```

425 = baseline 422 + 3 new. Nothing deselected, no warnings, no `-m` filter
applied.

## Commit

`tests/test_optimize_offline_real.py` committed with the brief's message
(exact SHA in the reply to the team lead).
