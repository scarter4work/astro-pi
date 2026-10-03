### Task 5: Recipe (the audit artifact)

**Files:**
- Create: `src/autocontrast/optimize/recipe.py`
- Test: `tests/test_optimize_recipe.py`

**Interfaces:**
- Consumes: `Action` from Task 1.
- Produces: `Recipe` (frozen: `actions: tuple[Action, ...]`); `Recipe.empty()`, `.extend(action) -> Recipe`, `.key -> str`, `.applied_kinds -> frozenset[str]`, `.to_dict() -> dict`, `Recipe.from_dict(d) -> Recipe`, `.to_pixinsight_steps() -> list[dict]`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_optimize_recipe.py
from autocontrast.optimize.actions import Action
from autocontrast.optimize.recipe import Recipe


def _a(kind="local_contrast", level="moderate", scale=8.0):
    return Action(kind=kind, level=level, scale_arcsec=scale, params={"layer": 3})


def test_recipe_is_immutable_and_extends_to_a_new_object():
    empty = Recipe.empty()
    one = empty.extend(_a())
    assert len(empty.actions) == 0
    assert len(one.actions) == 1


def test_recipe_key_distinguishes_order():
    a, b = _a(kind="black_point", scale=None), _a(kind="chroma", scale=None)
    assert Recipe.empty().extend(a).extend(b).key != Recipe.empty().extend(b).extend(a).key


def test_applied_kinds_tracks_once_only_actions():
    r = Recipe.empty().extend(_a(kind="star_split", level="", scale=None))
    assert "star_split" in r.applied_kinds


def test_round_trips_through_dict():
    r = Recipe.empty().extend(_a()).extend(_a(kind="chroma", scale=None))
    assert Recipe.from_dict(r.to_dict()).key == r.key


def test_renders_to_stock_pixinsight_steps():
    r = Recipe.empty().extend(_a())
    steps = r.to_pixinsight_steps()
    assert steps[0]["process"] == "MultiscaleLinearTransform"
    assert steps[0]["params"]["layer"] == 3
    assert "strength" in steps[0]["params"]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_optimize_recipe.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.optimize.recipe'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/autocontrast/optimize/recipe.py
"""The recipe: an ordered, replayable audit log (SS12).

SS12 requires the output be reconstructible as a sequence of stock PixInsight
processes. The recipe IS that artifact -- it must be sufficient to reproduce the
result with AutoContrast absent from the machine. Auditability is the point.
"""

from __future__ import annotations

from dataclasses import dataclass

from .actions import Action

# SS6.2's action -> stock PixInsight process mapping.
PROCESS_FOR_KIND = {
    "local_contrast": "MultiscaleLinearTransform",
    "local_equalize": "LocalHistogramEqualization",
    "core_hdr": "HDRMultiscaleTransform",
    "tonal_reshape": "CurvesTransformation",
    "black_point": "HistogramTransformation",
    "chroma": "ColorSaturation",
    "background_neutralize": "BackgroundNeutralization",
    "star_split": "StarXTerminator",
}


@dataclass(frozen=True)
class Recipe:
    actions: tuple[Action, ...] = ()

    @classmethod
    def empty(cls) -> "Recipe":
        return cls(actions=())

    def extend(self, action: Action) -> "Recipe":
        return Recipe(actions=self.actions + (action,))

    @property
    def key(self) -> str:
        """Order-sensitive identity, used for beam distinctness."""
        return " | ".join(a.key for a in self.actions)

    @property
    def applied_kinds(self) -> frozenset[str]:
        return frozenset(a.kind for a in self.actions)

    def to_dict(self) -> dict:
        return {
            "actions": [
                {"kind": a.kind, "level": a.level,
                 "scale_arcsec": a.scale_arcsec, "params": a.params}
                for a in self.actions
            ]
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Recipe":
        return cls(actions=tuple(
            Action(kind=a["kind"], level=a["level"],
                   scale_arcsec=a["scale_arcsec"], params=a.get("params", {}))
            for a in d["actions"]
        ))

    def to_pixinsight_steps(self) -> list[dict]:
        """Render to stock PI process invocations -- the SS12 audit artifact."""
        steps = []
        for a in self.actions:
            params = dict(a.params)
            params["strength"] = a.strength
            if a.scale_arcsec is not None:
                params["scale_arcsec"] = a.scale_arcsec
            steps.append({
                "process": PROCESS_FOR_KIND[a.kind],
                "action": a.key,
                "params": params,
            })
        return steps
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_optimize_recipe.py -v`
Expected: PASS (5 tests)

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/optimize/recipe.py tests/test_optimize_recipe.py
git commit -m "optimize: the recipe is the audit artifact

SS12 requires the result be reconstructible as stock PixInsight processes.
The recipe must suffice to reproduce it with AutoContrast absent from the
machine -- that is what auditability means here."
```

---

