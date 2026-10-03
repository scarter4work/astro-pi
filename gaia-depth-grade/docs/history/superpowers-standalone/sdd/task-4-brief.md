### Task 4: Distance source — interface, ADQL, Gaia/Bailer-Jones query with cache

**Files:**
- Create: `src/gaia_depth_grade/distances.py`
- Test: `tests/test_distances.py`

**Interfaces:**
- Consumes: `FieldFootprint`.
- Produces:
  - `DistanceSource` — ABC with `distances_for(self, footprint: FieldFootprint) -> astropy.table.Table`. Returned columns: `ra`, `dec`, `r_med_geo`, `r_lo_geo`, `r_hi_geo`, `source_id`.
  - `build_adql(footprint: FieldFootprint) -> str` — cone-search ADQL joining `gaiadr3.gaia_source` to `gaiadr3.distances` (Bailer-Jones geometric).
  - `GaiaStarSource(cache_dir: str)` implementing `DistanceSource`. Caches results as ECSV keyed by footprint; on a hit it logs `"using cached Gaia result <path>"`. `_run_query(adql: str) -> Table` performs the live TAP query (overridable in tests). Network/empty failures raise `RuntimeError` with the underlying message — never returns mock data.

- [ ] **Step 1: Write the failing test**

`tests/test_distances.py`:
```python
import numpy as np
import pytest
from astropy.table import Table
from gaia_depth_grade.wcs import FieldFootprint
from gaia_depth_grade.distances import build_adql, GaiaStarSource, DistanceSource


def _fake_catalog():
    t = Table()
    t["ra"] = [10.0, 10.01]
    t["dec"] = [20.0, 20.01]
    t["r_med_geo"] = [100.0, 500.0]
    t["r_lo_geo"] = [95.0, 400.0]
    t["r_hi_geo"] = [105.0, 650.0]
    t["source_id"] = [1, 2]
    return t


def test_build_adql_contains_join_and_cone():
    fp = FieldFootprint(10.0, 20.0, 0.05)
    q = build_adql(fp).lower()
    assert "gaiadr3.distances" in q
    assert "r_med_geo" in q
    assert "1/parallax" not in q
    assert "circle" in q and "10.0" in q and "20.0" in q


def test_gaiastarsource_caches(tmp_path, monkeypatch):
    src = GaiaStarSource(cache_dir=str(tmp_path))
    calls = {"n": 0}

    def fake_run(adql):
        calls["n"] += 1
        return _fake_catalog()

    monkeypatch.setattr(src, "_run_query", fake_run)
    fp = FieldFootprint(10.0, 20.0, 0.05)
    t1 = src.distances_for(fp)
    t2 = src.distances_for(fp)  # second call must hit cache, not _run_query
    assert calls["n"] == 1
    assert len(t1) == len(t2) == 2
    assert set(t1.colnames) >= {"ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id"}


def test_empty_query_raises(tmp_path, monkeypatch):
    src = GaiaStarSource(cache_dir=str(tmp_path))
    monkeypatch.setattr(src, "_run_query", lambda adql: Table(
        names=("ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id")))
    with pytest.raises(RuntimeError):
        src.distances_for(FieldFootprint(10.0, 20.0, 0.05))


def test_is_distance_source():
    assert issubclass(GaiaStarSource, DistanceSource)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_distances.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.distances`.

- [ ] **Step 3: Implement `distances.py`**

```python
from __future__ import annotations

import abc
import hashlib
import logging
import os

from astropy.table import Table

from .wcs import FieldFootprint

log = logging.getLogger(__name__)

_REQUIRED = ("ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id")


class DistanceSource(abc.ABC):
    @abc.abstractmethod
    def distances_for(self, footprint: FieldFootprint) -> Table: ...


def build_adql(footprint: FieldFootprint) -> str:
    ra, dec, r = footprint.center_ra, footprint.center_dec, footprint.radius_deg
    return (
        "SELECT g.ra, g.dec, d.r_med_geo, d.r_lo_geo, d.r_hi_geo, g.source_id "
        "FROM gaiadr3.gaia_source AS g "
        "JOIN gaiadr3.distances AS d ON g.source_id = d.source_id "
        "WHERE 1 = CONTAINS(POINT('ICRS', g.ra, g.dec), "
        f"CIRCLE('ICRS', {ra}, {dec}, {r})) "
        "AND d.r_med_geo IS NOT NULL"
    )


class GaiaStarSource(DistanceSource):
    def __init__(self, cache_dir: str):
        self.cache_dir = cache_dir
        os.makedirs(cache_dir, exist_ok=True)

    def _cache_path(self, footprint: FieldFootprint) -> str:
        key = f"{footprint.center_ra:.6f}_{footprint.center_dec:.6f}_{footprint.radius_deg:.6f}"
        digest = hashlib.sha1(key.encode()).hexdigest()[:16]
        return os.path.join(self.cache_dir, f"gaia_{digest}.ecsv")

    def _run_query(self, adql: str) -> Table:
        from astroquery.gaia import Gaia

        job = Gaia.launch_job_async(adql)
        return job.get_results()

    def distances_for(self, footprint: FieldFootprint) -> Table:
        path = self._cache_path(footprint)
        if os.path.exists(path):
            log.warning("using cached Gaia result %s", path)
            return Table.read(path, format="ascii.ecsv")
        try:
            tbl = self._run_query(build_adql(footprint))
        except Exception as exc:  # surface the real failure, never mock
            raise RuntimeError(f"Gaia TAP query failed: {exc}") from exc
        missing = set(_REQUIRED) - set(tbl.colnames)
        if missing:
            raise RuntimeError(f"Gaia result missing columns: {sorted(missing)}")
        if len(tbl) == 0:
            raise RuntimeError("Gaia query returned zero rows for this field")
        tbl = tbl[list(_REQUIRED)]
        tbl.write(path, format="ascii.ecsv", overwrite=True)
        return tbl
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest tests/test_distances.py -v`
Expected: PASS (5 passed).

- [ ] **Step 5: Commit**

```bash
git add src/gaia_depth_grade/distances.py tests/test_distances.py
git commit -m "feat: DistanceSource interface and Gaia/Bailer-Jones query with cache"
```

---

