# Task 8 Report: Beam expansion and pruning

## What was implemented

Created `src/autocontrast/optimize/beam.py` exactly per the brief:

- `BeamConfig` (frozen dataclass): `width=3`, `top_k=3`, `iteration_cap=20`,
  `convergence_window=3`, `epsilon=1e-3`, `epsilon_improve=1e-3`.
- `Branch` (frozen dataclass): `recipe: Recipe`, `image_path: str`,
  `distance: float`, `alive: bool = True`.
- `prune(candidates, width) -> list[Branch]`: filters to `alive` branches,
  sorts by `distance` ascending, deduplicates by `recipe.key` (order-sensitive
  recipe identity), and takes the first `width` survivors.

Created `tests/test_optimize_beam.py` exactly per the brief (5 tests). No
existing files were modified; both files are new.

## Step 2: confirm failing

```
$ .venv/bin/python -m pytest tests/test_optimize_beam.py -v
...
ImportError while importing test module '.../tests/test_optimize_beam.py'.
tests/test_optimize_beam.py:2: in <module>
    from autocontrast.optimize.beam import Branch, prune
E   ModuleNotFoundError: No module named 'autocontrast.optimize.beam'
=========================== short test summary info ============================
ERROR tests/test_optimize_beam.py
!!!!!!!!!!!!!!!!!!!! Interrupted: 1 error during collection !!!!!!!!!!!!!!!!!!!!
=============================== 1 error in 0.05s ===============================
```

Matches the brief's expected failure exactly (`ModuleNotFoundError`).

## Step 4: confirm passing

```
$ .venv/bin/python -m pytest tests/test_optimize_beam.py -v
============================= test session starts ==============================
collected 5 items

tests/test_optimize_beam.py::test_prune_keeps_the_lowest_distances PASSED [ 20%]
tests/test_optimize_beam.py::test_prune_respects_width PASSED            [ 40%]
tests/test_optimize_beam.py::test_prune_drops_dead_branches PASSED       [ 60%]
tests/test_optimize_beam.py::test_prune_deduplicates_identical_recipes PASSED [ 80%]
tests/test_optimize_beam.py::test_prune_returns_empty_when_everything_is_dead PASSED [100%]

============================== 5 passed in 0.01s ===============================
```

Also ran the full suite to check for regressions:

```
$ .venv/bin/python -m pytest -q
362 passed in 43.76s
```

No tests were deselected; all 362 pass, up from the pre-existing count plus
these 5 new ones.

## Commit

```
0df60c36db59d820ba11117099ea0b0447e5531f
optimize: beam pruning with recipe distinctness
```

Files changed: `src/autocontrast/optimize/beam.py` (new),
`tests/test_optimize_beam.py` (new). No existing files touched.

## Analysis: the distance-tie question

Question posed: `prune` is "sort, then slice" (same shape as Task 7's
degenerate proposer). Could a tie on `distance` cause the beam to fill with
near-identical branches, or prefer a branch for a reason unrelated to
quality?

**Structural collapse (Task 7's actual failure mode): no, already closed.**
Task 7's bug was that a *flat* top-k slice over a space with a real axis
(kind x band x magnitude) let ties on one axis silently annihilate the other
axes -- e.g. one (kind, band) group supplying all three of its own magnitude
levels and starving every other kind. `prune` cannot reproduce that failure
because it does not slice a raw ranked list -- it slices only AFTER
deduplicating by `recipe.key`, which is the one thing in this data structure
that must remain diverse. Two branches tied on distance but carrying
*different* recipes both survive the sort untouched (verified by
`test_prune_deduplicates_identical_recipes`'s sibling case: `_branch(0.3,
"c")` at a different distance survives alongside the deduped pair). Two
branches carrying the *identical* recipe collapse to one slot regardless of
whether their distances happen to match (they always will, since distance is
a deterministic function of the recipe's output) -- that is the correct,
required behavior, not a bug.

**Residual tie-break bias: yes, but low-severity and not the same shape.**
Python's `sorted` is stable, so when two *distinct* recipes tie exactly on
`distance`, `prune` keeps whichever appeared earlier in the `candidates` list
argument, i.e. earlier in whatever order the (not-yet-written) beam loop
concatenates a parent branch's children. Given `propose.py`'s design, that
order is not random: candidates are grouped by (priority, band, kind) where
priority is the size of the gap being addressed, so on a tie the loop will
tend to favor children of higher-ranked parents and, within a parent, the
action addressing the largest remaining gap first. That is a defensible
default (rank by decreasing importance) rather than an arbitrary one, and
unlike Task 7 it cannot make a single value monopolize every slot in one
step -- dedup already guarantees the survivors are structurally distinct
recipes. The remaining risk is narrower: over many iterations, if two
lineages happen to reach exactly the same measured distance, the same
lineage wins the tie every time, which could quietly narrow exploration
diversity across iterations even though the two lineages are equally good.
I did not change `prune`'s behavior for this -- the brief specifies exact
implementation code and GLOBAL CONSTRAINTS require deterministic output,
which stable-sort-on-input-order satisfies. Flagging for your call: if this
matters, the fix would live in the *loop's* candidate-ordering (Task 10),
not here, e.g. shuffling with a fixed seed or breaking ties on a secondary
key (such as recipe length) rather than insertion order.

## Self-review

- Test file and implementation are copied verbatim from the brief; no
  deviations.
- Confirmed both required properties for deduplication are asserted directly:
  `test_prune_deduplicates_identical_recipes` checks collapse (2 kept, not
  3), and implicitly within the same test a third, genuinely distinct recipe
  (`_branch(0.3, "c")`) also survives -- covering both directions.
  `test_prune_drops_dead_branches` and `test_prune_returns_empty_when_everything_is_dead`
  cover the `alive` filter and the legitimate empty-result convergence path.
- Ran the full suite (362 passed), not just the new file, to check for
  regressions -- none.
- No new dependencies, no existing files modified, single commit on
  `phase2-optimizer` with the exact message from the brief.
