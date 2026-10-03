### Task 5: Cross-match detected stars to catalog

**Files:**
- Create: `src/gaia_depth_grade/match.py`
- Test: `tests/test_match.py`

**Interfaces:**
- Consumes: detection `Table` (cols `x, y, flux, fwhm`) from Task 3; catalog `Table` (cols `ra, dec, r_*_geo, source_id`) from Task 4; `astropy.wcs.WCS` from Task 2.
- Produces:
  - `MatchStats(n_detected: int, n_matched: int, match_rate: float, median_offset_px: float)` — frozen dataclass.
  - `cross_match(detected, catalog, wcs, tolerance_px, neutral_strength_distance=None) -> tuple[Table, MatchStats]`. Returns `detected` plus columns `matched` (bool), `r_med_geo`, `r_lo_geo`, `r_hi_geo` (nan where unmatched). Matching projects catalog `ra/dec` to pixels via `wcs`, then nearest-neighbor within `tolerance_px` (scipy `cKDTree`). Each catalog source matches at most one detection (closest wins).

- [ ] **Step 1: Write the failing test**

`tests/test_match.py`:
```python
import numpy as np
import pytest
from astropy.table import Table
from gaia_depth_grade.wcs import load_wcs
from gaia_depth_grade.match import cross_match, MatchStats


def _catalog_from_wcs(w, pixel_positions, dists):
    sky = w.pixel_to_world([p[0] for p in pixel_positions], [p[1] for p in pixel_positions])
    t = Table()
    t["ra"] = sky.ra.deg
    t["dec"] = sky.dec.deg
    t["r_med_geo"] = [d for d in dists]
    t["r_lo_geo"] = [d * 0.95 for d in dists]
    t["r_hi_geo"] = [d * 1.05 for d in dists]
    t["source_id"] = list(range(len(dists)))
    return t


def test_match_within_tolerance(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    detected = Table()
    detected["x"] = [150.0, 50.0]
    detected["y"] = [100.0, 60.0]
    detected["flux"] = [1.0, 0.5]
    detected["fwhm"] = [4.0, 4.0]
    # catalog: one ~1px from first detection, one far away
    catalog = _catalog_from_wcs(w, [(150.5, 100.2), (10.0, 10.0)], [120.0, 800.0])
    out, stats = cross_match(detected, catalog, w, tolerance_px=3.0)
    assert isinstance(stats, MatchStats)
    assert stats.n_detected == 2
    assert stats.n_matched == 1
    assert bool(out["matched"][0]) is True
    assert out["r_med_geo"][0] == pytest.approx(120.0)
    assert bool(out["matched"][1]) is False
    assert np.isnan(out["r_med_geo"][1])
    assert stats.match_rate == pytest.approx(0.5)


def test_no_catalog_zero_matches(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    detected = Table()
    detected["x"] = [150.0]; detected["y"] = [100.0]
    detected["flux"] = [1.0]; detected["fwhm"] = [4.0]
    catalog = Table(names=("ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id"))
    out, stats = cross_match(detected, catalog, w, tolerance_px=3.0)
    assert stats.n_matched == 0
    assert bool(out["matched"][0]) is False
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_match.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.match`.

- [ ] **Step 3: Implement `match.py`**

```python
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from astropy.table import Table
from scipy.spatial import cKDTree


@dataclass(frozen=True)
class MatchStats:
    n_detected: int
    n_matched: int
    match_rate: float
    median_offset_px: float


def cross_match(detected: Table, catalog: Table, wcs, tolerance_px: float):
    out = detected.copy()
    n = len(out)
    out["matched"] = np.zeros(n, dtype=bool)
    for col in ("r_med_geo", "r_lo_geo", "r_hi_geo"):
        out[col] = np.full(n, np.nan, dtype=float)

    if n == 0 or len(catalog) == 0:
        return out, MatchStats(n, 0, 0.0 if n else 0.0, float("nan"))

    cat_x, cat_y = wcs.world_to_pixel_values(catalog["ra"], catalog["dec"])
    det_xy = np.column_stack([np.asarray(out["x"]), np.asarray(out["y"])])
    tree = cKDTree(det_xy)
    dist, idx = tree.query(np.column_stack([cat_x, cat_y]), k=1)

    # each detection keeps its closest catalog source within tolerance
    best = {}
    for ci, (d, di) in enumerate(zip(dist, idx)):
        if d > tolerance_px:
            continue
        if di not in best or d < best[di][0]:
            best[di] = (d, ci)

    offsets = []
    for di, (d, ci) in best.items():
        out["matched"][di] = True
        out["r_med_geo"][di] = catalog["r_med_geo"][ci]
        out["r_lo_geo"][di] = catalog["r_lo_geo"][ci]
        out["r_hi_geo"][di] = catalog["r_hi_geo"][ci]
        offsets.append(d)

    n_matched = len(best)
    return out, MatchStats(
        n_detected=n,
        n_matched=n_matched,
        match_rate=n_matched / n,
        median_offset_px=float(np.median(offsets)) if offsets else float("nan"),
    )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest tests/test_match.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Commit**

```bash
git add src/gaia_depth_grade/match.py tests/test_match.py
git commit -m "feat: cross-match detections to Gaia catalog with QA stats"
```

---

