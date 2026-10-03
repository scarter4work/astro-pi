### Task 1: Extract shared spherical math

Pure refactor, no behavior change. `index.py` needs the same separation math `store.py` has
privately; duplicating a haversine in two files invites silent divergence.

**Files:**
- Create: `src/autocontrast/db/skymath.py`
- Modify: `src/autocontrast/db/store.py:23-27` (delete `_separation_arcmin`, import instead)
- Test: `tests/test_skymath.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `separation_arcmin(ra1, dec1, ra2, dec2) -> float`;
  `cones_overlap(sep_arcmin, radius_a_arcmin, radius_b_arcmin) -> bool`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_skymath.py
"""Shared spherical helpers (§5.1 cone-search geometry)."""

from __future__ import annotations

import pytest

from autocontrast.db.skymath import cones_overlap, separation_arcmin


def test_zero_separation_for_identical_positions():
    assert separation_arcmin(83.82, -5.39, 83.82, -5.39) == pytest.approx(0.0, abs=1e-9)


def test_one_degree_of_declination_is_sixty_arcmin():
    assert separation_arcmin(10.0, 0.0, 10.0, 1.0) == pytest.approx(60.0, rel=1e-9)


def test_ra_separation_shrinks_with_cos_dec():
    """One degree of RA subtends less angle away from the equator — the cos(dec)
    factor is exactly what a naive coordinate difference gets wrong."""
    at_equator = separation_arcmin(0.0, 0.0, 1.0, 0.0)
    at_sixty = separation_arcmin(0.0, 60.0, 1.0, 60.0)
    assert at_equator == pytest.approx(60.0, rel=1e-6)
    assert at_sixty == pytest.approx(30.0, rel=1e-3)


def test_ra_wrap_across_zero_is_short_way_round():
    """359.5 -> 0.5 is one degree apart, not 359."""
    assert separation_arcmin(359.5, 0.0, 0.5, 0.0) == pytest.approx(60.0, rel=1e-6)


def test_matches_the_two_real_m42_references():
    """heic0601a vs eso1103a, from data/seed_catalog.json."""
    sep = separation_arcmin(83.7905, -5.4140, 83.82217, -5.39099)
    assert sep == pytest.approx(2.34, abs=0.05)


def test_cones_overlap_at_and_beyond_the_boundary():
    assert cones_overlap(10.0, 4.0, 6.0) is True     # exactly touching
    assert cones_overlap(10.001, 4.0, 6.0) is False   # just past
    assert cones_overlap(0.0, 1.0, 1.0) is True
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_skymath.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.db.skymath'`

- [ ] **Step 3: Write the implementation**

```python
# src/autocontrast/db/skymath.py
"""Spherical geometry shared by the fingerprint store and the gallery index.

Both do §5.1 cone-overlap tests. The haversine lived privately in ``store.py``;
it is here so the two callers cannot silently diverge.
"""

from __future__ import annotations

import numpy as np


def separation_arcmin(ra1: float, dec1: float, ra2: float, dec2: float) -> float:
    """Great-circle separation in arcminutes (haversine).

    Handles RA wrap and the cos(dec) foreshortening for free — a plain coordinate
    difference gets both wrong.
    """
    r1, d1, r2, d2 = np.radians([ra1, dec1, ra2, dec2])
    a = np.sin((d2 - d1) / 2) ** 2 + np.cos(d1) * np.cos(d2) * np.sin((r2 - r1) / 2) ** 2
    return float(np.degrees(2 * np.arcsin(np.sqrt(a))) * 60.0)


def cones_overlap(sep_arcmin: float, radius_a_arcmin: float, radius_b_arcmin: float) -> bool:
    """Two cones overlap when their center separation is at most the sum of radii.

    Boundary-inclusive: exactly touching counts as overlapping, matching
    ``FingerprintStore.cone_search``.
    """
    return sep_arcmin <= radius_a_arcmin + radius_b_arcmin
```

- [ ] **Step 4: Rewire `store.py` to the shared helper**

Delete lines 23–27 of `src/autocontrast/db/store.py` (the `_separation_arcmin` definition),
add the import alongside the existing `from .records import ...`:

```python
from .records import SOURCE_TYPES, ReferenceRecord
from .skymath import separation_arcmin
```

Then update the single call site inside `cone_search` (was line 125):

```python
            sep = separation_arcmin(ra_deg, dec_deg, row["ra_deg"], row["dec_deg"])
```

- [ ] **Step 5: Run the full suite — the refactor must not change any existing behavior**

Run: `.venv/bin/python -m pytest -q`
Expected: PASS, **144 passed** (138 existing + 6 new). If any pre-existing test fails, the
refactor is wrong — revert and redo, do not adjust the old tests.

- [ ] **Step 6: Lint**

Run: `.venv/bin/python -m ruff check src tests`
Expected: no findings.

- [ ] **Step 7: Commit**

```bash
git add src/autocontrast/db/skymath.py src/autocontrast/db/store.py tests/test_skymath.py
git commit -m "Extract db/skymath.py: shared cone-overlap geometry

index.py needs the same haversine store.py had privately. Two copies of a
spherical-distance formula is a silent-divergence risk, so it now has one home.
Pure refactor: no behavior change, existing tests untouched."
```

---

