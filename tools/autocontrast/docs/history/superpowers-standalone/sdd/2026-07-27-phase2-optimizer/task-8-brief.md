### Task 8: Beam expansion and pruning

**Files:**
- Create: `src/autocontrast/optimize/beam.py`
- Test: `tests/test_optimize_beam.py`

**Interfaces:**
- Consumes: `Recipe`, `Action`.
- Produces: `Branch` (frozen: `recipe: Recipe`, `image_path: str`, `distance: float`, `alive: bool`), `prune(candidates, width) -> list[Branch]`, `BeamConfig` (frozen: `width=3`, `top_k=3`, `iteration_cap=20`, `convergence_window=3`, `epsilon=1e-3`, `epsilon_improve=1e-3`).

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_beam.py
from autocontrast.optimize.actions import Action
from autocontrast.optimize.beam import Branch, prune
from autocontrast.optimize.recipe import Recipe


def _branch(distance, *kinds, alive=True):
    r = Recipe.empty()
    for k in kinds:
        r = r.extend(Action(k, "moderate", None, {}))
    return Branch(recipe=r, image_path=f"/tmp/{'-'.join(kinds) or 'root'}.xisf",
                  distance=distance, alive=alive)


def test_prune_keeps_the_lowest_distances():
    kept = prune([_branch(0.5, "a"), _branch(0.1, "b"), _branch(0.3, "c")], width=2)
    assert [b.distance for b in kept] == [0.1, 0.3]


def test_prune_respects_width():
    assert len(prune([_branch(i / 10, f"k{i}") for i in range(9)], width=3)) == 3


def test_prune_drops_dead_branches():
    kept = prune([_branch(0.1, "a", alive=False), _branch(0.4, "b")], width=3)
    assert [b.recipe.key for b in kept] == [_branch(0.4, "b").recipe.key]


def test_prune_deduplicates_identical_recipes():
    # Two branches that reached the same action sequence must not both hold slots.
    kept = prune([_branch(0.2, "a", "b"), _branch(0.25, "a", "b"), _branch(0.3, "c")],
                 width=3)
    assert len(kept) == 2


def test_prune_returns_empty_when_everything_is_dead():
    assert prune([_branch(0.1, "a", alive=False)], width=3) == []
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_beam.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.beam'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/beam.py
"""Beam search state (SS6.1).

A beam, not a hill-climb: improvement is non-monotonic, and a greedy loop walks
straight into the over-cooked attractor because every individual step "increased
contrast" (SS6.1).
"""

from __future__ import annotations

from dataclasses import dataclass

from .recipe import Recipe


@dataclass(frozen=True)
class BeamConfig:
    width: int = 3
    top_k: int = 3
    iteration_cap: int = 20
    convergence_window: int = 3
    epsilon: float = 1e-3
    epsilon_improve: float = 1e-3


@dataclass(frozen=True)
class Branch:
    recipe: Recipe
    image_path: str
    distance: float
    alive: bool = True


def prune(candidates: list[Branch], width: int) -> list[Branch]:
    """Keep the ``width`` best live, distinct branches.

    Distinctness is by recipe: two branches that arrived at the same action
    sequence must not both hold a beam slot, or the beam silently narrows to one
    line of search while appearing to be three.
    """
    seen: set[str] = set()
    kept: list[Branch] = []
    for branch in sorted((c for c in candidates if c.alive), key=lambda b: b.distance):
        if branch.recipe.key in seen:
            continue
        seen.add(branch.recipe.key)
        kept.append(branch)
        if len(kept) == width:
            break
    return kept
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_beam.py -v`
Expected: PASS (5 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/beam.py tests/test_optimize_beam.py
git commit -m "optimize: beam pruning with recipe distinctness

Two branches that reached the same action sequence must not both hold a
slot, or the beam narrows to one line of search while appearing to be
three. A beam and not a hill-climb because SS6.1 improvement is
non-monotonic."
```

---

