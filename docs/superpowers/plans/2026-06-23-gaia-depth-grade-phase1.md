# Gaia Depth Grade — Phase 1 (Star-Field Depth) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Python pipeline that takes a star-layer FITS (+WCS), assigns each star a measured Gaia distance, and renders a depth-modulated star layer — plus a thin PJSR wrapper that drives it inside the headless PixInsight harness.

**Architecture:** Hybrid. A PI-free Python core does all science and rendering behind a FITS-in/FITS-out boundary (`detect → distances → match → transform → modulate → render`, orchestrated by a CLI). A thin PJSR wrapper plate-solves, splits stars via StarXTerminator, shells out to the Python CLI, and recombines.

**Tech Stack:** Python 3.14 (venv via `uv`), numpy 2.x, scipy, astropy 7.x, photutils, astroquery (Gaia TAP), pytest. PJSR (JavaScript) for the PI harness wrapper.

## Global Constraints

- Python core MUST NOT import or depend on any PixInsight/PJSR API. The only cross-boundary artifacts are FITS files (pixels + WCS header) and a JSON QA/config sidecar.
- Distances MUST come from Bailer-Jones geometric distances (`r_med_geo`, `r_lo_geo`, `r_hi_geo`); NEVER raw `1/parallax`.
- Errors surface loudly. No silent/mock fallbacks. A used-but-stale cache hit MUST be logged, never masked. Missing WCS is a hard error. Match rate below threshold emits a loud QA warning.
- Output FITS MUST carry the honesty tag, verbatim: `GAIA depth-derived; physically motivated, not per-pixel correct for gas`
- Package import name: `gaia_depth_grade`. Source under `src/gaia_depth_grade/`. Tests under `tests/`.
- Every modulation gain MUST support being set to `0.0` as an exact no-op for its attribute.
- All angles in degrees; pixel coordinates are 0-based (numpy convention) internally.

---

## File Structure

```
gaia-depth-grade/
├── pyproject.toml                      # package + deps + pytest config
├── src/gaia_depth_grade/
│   ├── __init__.py
│   ├── config.py        # Gains, GradeConfig dataclasses + TOML loader
│   ├── wcs.py           # WCS load + field footprint
│   ├── detect.py        # star detection + per-star FWHM estimate
│   ├── distances.py     # DistanceSource ABC + GaiaStarSource (TAP + cache)
│   ├── match.py         # cross-match detected ↔ catalog + MatchStats
│   ├── transform.py     # depth_strength + confidence
│   ├── modulate.py      # Modulation deltas from strength + gains
│   ├── render.py        # composite modulated star layer
│   └── cli.py           # `grade` / `debug` orchestration, FITS I/O, metadata
├── tests/
│   ├── conftest.py      # synthetic-frame + fake-catalog fixtures
│   ├── test_config.py
│   ├── test_wcs.py
│   ├── test_detect.py
│   ├── test_distances.py
│   ├── test_match.py
│   ├── test_transform.py
│   ├── test_modulate.py
│   ├── test_render.py
│   └── test_e2e_synthetic.py
└── pjsr/
    └── gaia_depth_grade.js             # thin PI harness wrapper
```

---

### Task 1: Project scaffolding, env, and config

**Files:**
- Create: `pyproject.toml`
- Create: `src/gaia_depth_grade/__init__.py`
- Create: `src/gaia_depth_grade/config.py`
- Test: `tests/test_config.py`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `Gains(brightness: float=0.5, size: float=0.4, contrast: float=0.3, saturation: float=0.3)` — dataclass.
  - `GradeConfig(gains: Gains, p_low: float=5.0, p_high: float=95.0, match_tolerance_px: float=3.0, min_match_rate: float=0.3, detect_fwhm: float=3.0, detect_threshold_sigma: float=5.0, base_sigma_px: float=2.0, cache_dir: str=".cache", neutral_strength: float=0.0)` — dataclass.
  - `load_config(path: str | None) -> GradeConfig` — returns defaults if `path is None`, else parses TOML (`tomllib`), overriding any provided keys; unknown keys raise `ValueError`.

- [ ] **Step 1: Create the virtualenv and install deps**

Run:
```bash
cd /home/scarter4work/projects/gaia-depth-grade
uv venv --python 3.14 .venv
. .venv/bin/activate
uv pip install "numpy>=2" scipy "astropy>=7" photutils astroquery pytest
```
Expected: all install without error. If `photutils` has no 3.14 wheel, fall back: `uv venv --python 3.13 .venv` and re-run (note the chosen version in the commit message).

- [ ] **Step 2: Write `pyproject.toml`**

```toml
[project]
name = "gaia-depth-grade"
version = "0.1.0"
description = "Physically-grounded depth grade for astrophotographs using Gaia distances"
requires-python = ">=3.13"
dependencies = ["numpy>=2", "scipy", "astropy>=7", "photutils", "astroquery"]

[build-system]
requires = ["setuptools>=68"]
build-backend = "setuptools.build_meta"

[tool.setuptools.packages.find]
where = ["src"]

[tool.pytest.ini_options]
pythonpath = ["src"]
testpaths = ["tests"]
```

- [ ] **Step 3: Write the failing test**

`tests/test_config.py`:
```python
import textwrap
import pytest
from gaia_depth_grade.config import Gains, GradeConfig, load_config


def test_defaults_when_no_path():
    cfg = load_config(None)
    assert isinstance(cfg, GradeConfig)
    assert cfg.gains.brightness == 0.5
    assert cfg.p_low == 5.0 and cfg.p_high == 95.0


def test_toml_overrides(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text(textwrap.dedent("""
        p_low = 10.0
        [gains]
        brightness = 1.0
        saturation = 0.0
    """))
    cfg = load_config(str(p))
    assert cfg.p_low == 10.0
    assert cfg.gains.brightness == 1.0
    assert cfg.gains.saturation == 0.0
    assert cfg.gains.size == 0.4  # untouched default


def test_unknown_key_raises(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text("bogus_key = 1\n")
    with pytest.raises(ValueError):
        load_config(str(p))
```

- [ ] **Step 4: Run test to verify it fails**

Run: `pytest tests/test_config.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.config`.

- [ ] **Step 5: Implement `config.py`**

`src/gaia_depth_grade/__init__.py`:
```python
__all__ = ["__version__"]
__version__ = "0.1.0"
```

`src/gaia_depth_grade/config.py`:
```python
from __future__ import annotations

import tomllib
from dataclasses import dataclass, fields, replace


@dataclass(frozen=True)
class Gains:
    brightness: float = 0.5
    size: float = 0.4
    contrast: float = 0.3
    saturation: float = 0.3


@dataclass(frozen=True)
class GradeConfig:
    gains: Gains = Gains()
    p_low: float = 5.0
    p_high: float = 95.0
    match_tolerance_px: float = 3.0
    min_match_rate: float = 0.3
    detect_fwhm: float = 3.0
    detect_threshold_sigma: float = 5.0
    base_sigma_px: float = 2.0
    cache_dir: str = ".cache"
    neutral_strength: float = 0.0


def _apply(obj, data: dict):
    valid = {f.name for f in fields(obj)}
    unknown = set(data) - valid
    if unknown:
        raise ValueError(f"Unknown config keys: {sorted(unknown)}")
    return data


def load_config(path: str | None) -> GradeConfig:
    if path is None:
        return GradeConfig()
    with open(path, "rb") as fh:
        raw = tomllib.load(fh)
    gains_raw = raw.pop("gains", {})
    _apply(GradeConfig(), raw)
    _apply(Gains(), gains_raw)
    gains = replace(Gains(), **gains_raw)
    return replace(GradeConfig(), gains=gains, **raw)
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `pytest tests/test_config.py -v`
Expected: PASS (3 passed).

- [ ] **Step 7: Commit**

```bash
git add pyproject.toml src/gaia_depth_grade/__init__.py src/gaia_depth_grade/config.py tests/test_config.py
git commit -m "feat: scaffold package, env, and config loader"
```

---

### Task 2: WCS load and field footprint

**Files:**
- Create: `src/gaia_depth_grade/wcs.py`
- Test: `tests/test_wcs.py`
- Create: `tests/conftest.py`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `FieldFootprint(center_ra: float, center_dec: float, radius_deg: float)` — frozen dataclass (degrees).
  - `load_wcs(header) -> astropy.wcs.WCS` — raises `ValueError("FITS header has no usable WCS")` if the header lacks celestial WCS (`wcs.has_celestial` is False).
  - `field_footprint(wcs, shape: tuple[int, int]) -> FieldFootprint` — `shape` is `(ny, nx)`; center = image center pixel mapped to sky; radius = angular distance from center to the farthest image corner.

- [ ] **Step 1: Write `tests/conftest.py` with a WCS fixture**

```python
import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS


@pytest.fixture
def simple_wcs_header():
    """A tangent-plane WCS centered at RA=10, Dec=20, 1 arcsec/px, 200x300."""
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crpix = [150.0, 100.0]      # center of 300(x) x 200(y)
    w.wcs.crval = [10.0, 20.0]
    w.wcs.cdelt = [-1.0 / 3600.0, 1.0 / 3600.0]
    hdr = w.to_header()
    hdr["NAXIS"] = 2
    hdr["NAXIS1"] = 300
    hdr["NAXIS2"] = 200
    return hdr
```

- [ ] **Step 2: Write the failing test**

`tests/test_wcs.py`:
```python
import numpy as np
import pytest
from astropy.io import fits
from gaia_depth_grade.wcs import load_wcs, field_footprint, FieldFootprint


def test_load_wcs_ok(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    assert w.has_celestial


def test_load_wcs_missing_raises():
    hdr = fits.Header()
    hdr["NAXIS"] = 2
    with pytest.raises(ValueError):
        load_wcs(hdr)


def test_field_footprint_center_and_radius(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    fp = field_footprint(w, (200, 300))
    assert isinstance(fp, FieldFootprint)
    assert fp.center_ra == pytest.approx(10.0, abs=1e-3)
    assert fp.center_dec == pytest.approx(20.0, abs=1e-3)
    # half-diagonal of 300x200 px at 1"/px ≈ 180.3" ≈ 0.0501 deg
    assert fp.radius_deg == pytest.approx(0.0501, abs=2e-3)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `pytest tests/test_wcs.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.wcs`.

- [ ] **Step 4: Implement `wcs.py`**

```python
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from astropy.coordinates import SkyCoord
from astropy.wcs import WCS


@dataclass(frozen=True)
class FieldFootprint:
    center_ra: float
    center_dec: float
    radius_deg: float


def load_wcs(header) -> WCS:
    w = WCS(header)
    if not w.has_celestial:
        raise ValueError("FITS header has no usable WCS")
    return w.celestial


def field_footprint(wcs: WCS, shape: tuple[int, int]) -> FieldFootprint:
    ny, nx = shape
    cx, cy = (nx - 1) / 2.0, (ny - 1) / 2.0
    center = wcs.pixel_to_world(cx, cy)
    corners_x = [0, nx - 1, 0, nx - 1]
    corners_y = [0, 0, ny - 1, ny - 1]
    corners = wcs.pixel_to_world(corners_x, corners_y)
    sep = center.separation(corners).deg
    return FieldFootprint(
        center_ra=float(center.ra.deg),
        center_dec=float(center.dec.deg),
        radius_deg=float(np.max(sep)),
    )
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pytest tests/test_wcs.py -v`
Expected: PASS (3 passed).

- [ ] **Step 6: Commit**

```bash
git add src/gaia_depth_grade/wcs.py tests/test_wcs.py tests/conftest.py
git commit -m "feat: WCS load and field footprint"
```

---

### Task 3: Star detection and FWHM estimate

**Files:**
- Create: `src/gaia_depth_grade/detect.py`
- Test: `tests/test_detect.py`
- Modify: `tests/conftest.py` (add synthetic-stars fixture)

**Interfaces:**
- Consumes: `GradeConfig` (uses `detect_fwhm`, `detect_threshold_sigma`).
- Produces:
  - `detect_stars(image: np.ndarray, fwhm: float, threshold_sigma: float) -> astropy.table.Table` with float columns `x`, `y` (0-based pixel centroids), `flux`, `fwhm` (per-star estimate). `image` is 2-D (luminance); callers pass a single channel.
  - `measure_fwhm(image: np.ndarray, x: float, y: float, box: int = 7) -> float` — second-moment FWHM in pixels over a `box`×`box` cutout; returns `nan` if the cutout is off-edge or flat.

- [ ] **Step 1: Add a synthetic-stars fixture to `tests/conftest.py`**

```python
def _gaussian_star(img, x, y, flux, sigma):
    ny, nx = img.shape
    yy, xx = np.mgrid[0:ny, 0:nx]
    img += flux * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma**2))


@pytest.fixture
def synthetic_stars():
    """64x64 frame, 3 stars at known positions/fluxes, sigma=2 px, faint noise."""
    rng = np.random.default_rng(0)
    img = rng.normal(0.0, 0.001, size=(64, 64)).astype(np.float64)
    truth = [(16.0, 16.0, 1.0), (48.0, 20.0, 0.6), (32.0, 50.0, 0.3)]
    for x, y, f in truth:
        _gaussian_star(img, x, y, f, sigma=2.0)
    return img, truth
```

- [ ] **Step 2: Write the failing test**

`tests/test_detect.py`:
```python
import numpy as np
import pytest
from gaia_depth_grade.detect import detect_stars, measure_fwhm


def test_detects_all_three(synthetic_stars):
    img, truth = synthetic_stars
    tbl = detect_stars(img, fwhm=3.0, threshold_sigma=5.0)
    assert len(tbl) >= 3
    # brightest detected source should sit near the brightest truth star (16,16)
    tbl.sort("flux", reverse=True)
    assert tbl["x"][0] == pytest.approx(16.0, abs=1.0)
    assert tbl["y"][0] == pytest.approx(16.0, abs=1.0)


def test_measure_fwhm_matches_sigma(synthetic_stars):
    img, _ = synthetic_stars
    f = measure_fwhm(img, 16.0, 16.0, box=11)
    # FWHM = 2.355 * sigma ; sigma=2 -> ~4.71 px
    assert f == pytest.approx(4.71, abs=1.2)


def test_measure_fwhm_offedge_is_nan(synthetic_stars):
    img, _ = synthetic_stars
    assert np.isnan(measure_fwhm(img, 1.0, 1.0, box=11))
```

- [ ] **Step 3: Run test to verify it fails**

Run: `pytest tests/test_detect.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.detect`.

- [ ] **Step 4: Implement `detect.py`**

```python
from __future__ import annotations

import numpy as np
from astropy.stats import sigma_clipped_stats
from astropy.table import Table
from photutils.detection import DAOStarFinder


def measure_fwhm(image: np.ndarray, x: float, y: float, box: int = 7) -> float:
    half = box // 2
    xi, yi = int(round(x)), int(round(y))
    if xi - half < 0 or yi - half < 0 or xi + half >= image.shape[1] or yi + half >= image.shape[0]:
        return float("nan")
    cut = image[yi - half : yi + half + 1, xi - half : xi + half + 1].astype(float)
    cut = cut - np.median(cut)
    cut[cut < 0] = 0.0
    total = cut.sum()
    if total <= 0:
        return float("nan")
    yy, xx = np.mgrid[0 : cut.shape[0], 0 : cut.shape[1]]
    mx = (xx * cut).sum() / total
    my = (yy * cut).sum() / total
    varx = ((xx - mx) ** 2 * cut).sum() / total
    vary = ((yy - my) ** 2 * cut).sum() / total
    sigma = np.sqrt(max((varx + vary) / 2.0, 0.0))
    return float(2.3548 * sigma)


def detect_stars(image: np.ndarray, fwhm: float, threshold_sigma: float) -> Table:
    mean, median, std = sigma_clipped_stats(image, sigma=3.0)
    finder = DAOStarFinder(fwhm=fwhm, threshold=threshold_sigma * std)
    found = finder(image - median)
    if found is None or len(found) == 0:
        return Table(names=("x", "y", "flux", "fwhm"), dtype=(float, float, float, float))
    out = Table()
    out["x"] = np.asarray(found["xcentroid"], dtype=float)
    out["y"] = np.asarray(found["ycentroid"], dtype=float)
    out["flux"] = np.asarray(found["flux"], dtype=float)
    out["fwhm"] = [measure_fwhm(image, xx, yy) for xx, yy in zip(out["x"], out["y"])]
    return out
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `pytest tests/test_detect.py -v`
Expected: PASS (3 passed).

- [ ] **Step 6: Commit**

```bash
git add src/gaia_depth_grade/detect.py tests/test_detect.py tests/conftest.py
git commit -m "feat: star detection and second-moment FWHM"
```

---

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

### Task 5: Cross-match detected stars to catalog

**Files:**
- Create: `src/gaia_depth_grade/match.py`
- Test: `tests/test_match.py`

**Interfaces:**
- Consumes: detection `Table` (cols `x, y, flux, fwhm`) from Task 3; catalog `Table` (cols `ra, dec, r_*_geo, source_id`) from Task 4; `astropy.wcs.WCS` from Task 2.
- Produces:
  - `MatchStats(n_detected: int, n_matched: int, match_rate: float, median_offset_px: float)` — frozen dataclass.
  - `cross_match(detected, catalog, wcs, tolerance_px) -> tuple[Table, MatchStats]`. Returns `detected` plus columns `matched` (bool), `r_med_geo`, `r_lo_geo`, `r_hi_geo` (nan where unmatched). (Neutral depth for unmatched stars is applied downstream in `transform.effective_strength` via `config.neutral_strength`, not here.) Matching projects catalog `ra/dec` to pixels via `wcs`, then nearest-neighbor within `tolerance_px` (scipy `cKDTree`). Each catalog source matches at most one detection (closest wins).

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

### Task 6: Depth transform (distance → strength) and confidence

**Files:**
- Create: `src/gaia_depth_grade/transform.py`
- Test: `tests/test_transform.py`

**Interfaces:**
- Consumes: arrays of `r_med_geo`, `r_lo_geo`, `r_hi_geo` (parsecs; may contain nan for unmatched).
- Produces:
  - `depth_strength(r_med, p_low=5.0, p_high=95.0, neutral=0.0) -> np.ndarray` in `[-1, +1]`: `+1` = nearest (smallest distance), `-1` = farthest. Uses `log10` of distance, clipped to the `[p_low, p_high]` percentile band of the finite values, linearly mapped so smallest→+1, largest→−1. nan entries map to `neutral`.
  - `confidence(r_med, r_lo, r_hi) -> np.ndarray` in `[0, 1]`: `1 - clip(((r_hi - r_lo) / (2*r_med)), 0, 1)`; nan → `0.0`.
  - `effective_strength(r_med, r_lo, r_hi, p_low, p_high, neutral) -> np.ndarray`: `depth_strength * confidence`, with neutral entries left at `neutral`.

- [ ] **Step 1: Write the failing test**

`tests/test_transform.py`:
```python
import numpy as np
import pytest
from gaia_depth_grade.transform import depth_strength, confidence, effective_strength


def test_near_is_plus_one_far_is_minus_one():
    r = np.array([10.0, 100.0, 1000.0])
    s = depth_strength(r, p_low=0, p_high=100)
    assert s[0] == pytest.approx(1.0, abs=1e-6)   # nearest
    assert s[-1] == pytest.approx(-1.0, abs=1e-6)  # farthest
    assert s[0] > s[1] > s[2]                      # monotonic decreasing


def test_nan_maps_to_neutral():
    r = np.array([10.0, np.nan, 1000.0])
    s = depth_strength(r, p_low=0, p_high=100, neutral=0.0)
    assert s[1] == 0.0


def test_confidence_tight_vs_loose():
    r_med = np.array([100.0, 100.0])
    r_lo = np.array([98.0, 50.0])
    r_hi = np.array([102.0, 150.0])
    c = confidence(r_med, r_lo, r_hi)
    assert c[0] > c[1]                 # tight error -> higher confidence
    assert 0.0 <= c[1] <= c[0] <= 1.0


def test_effective_strength_attenuates_noisy():
    r_med = np.array([10.0, 10.0])
    r_lo = np.array([9.8, 1.0])
    r_hi = np.array([10.2, 30.0])
    e = effective_strength(r_med, r_lo, r_hi, p_low=0, p_high=100, neutral=0.0)
    # both at same (nearest) distance; the noisier one is pulled toward 0
    assert abs(e[0]) > abs(e[1])
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_transform.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.transform`.

- [ ] **Step 3: Implement `transform.py`**

```python
from __future__ import annotations

import numpy as np


def depth_strength(r_med, p_low=5.0, p_high=95.0, neutral=0.0) -> np.ndarray:
    r = np.asarray(r_med, dtype=float)
    out = np.full(r.shape, float(neutral), dtype=float)
    finite = np.isfinite(r) & (r > 0)
    if not np.any(finite):
        return out
    logr = np.log10(r[finite])
    lo = np.percentile(logr, p_low)
    hi = np.percentile(logr, p_high)
    if hi <= lo:
        out[finite] = 0.0
        return out
    norm = (np.clip(logr, lo, hi) - lo) / (hi - lo)  # 0=near..1=far
    out[finite] = 1.0 - 2.0 * norm                    # +1 near .. -1 far
    return out


def confidence(r_med, r_lo, r_hi) -> np.ndarray:
    rm = np.asarray(r_med, dtype=float)
    rl = np.asarray(r_lo, dtype=float)
    rh = np.asarray(r_hi, dtype=float)
    with np.errstate(invalid="ignore", divide="ignore"):
        frac = (rh - rl) / (2.0 * rm)
    c = 1.0 - np.clip(frac, 0.0, 1.0)
    c[~np.isfinite(c)] = 0.0
    return c


def effective_strength(r_med, r_lo, r_hi, p_low=5.0, p_high=95.0, neutral=0.0) -> np.ndarray:
    s = depth_strength(r_med, p_low, p_high, neutral)
    c = confidence(r_med, r_lo, r_hi)
    rm = np.asarray(r_med, dtype=float)
    e = s * c
    nanmask = ~np.isfinite(rm)
    e[nanmask] = neutral
    return e
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest tests/test_transform.py -v`
Expected: PASS (4 passed).

- [ ] **Step 5: Commit**

```bash
git add src/gaia_depth_grade/transform.py tests/test_transform.py
git commit -m "feat: log-distance depth transform with confidence weighting"
```

---

### Task 7: Modulation deltas from strength + gains

**Files:**
- Create: `src/gaia_depth_grade/modulate.py`
- Test: `tests/test_modulate.py`

**Interfaces:**
- Consumes: effective-strength array (Task 6); `Gains` (Task 1).
- Produces:
  - `Modulation(brightness: np.ndarray, size: np.ndarray, contrast: np.ndarray, saturation: np.ndarray)` — frozen dataclass; each is a per-star multiplier/amount array.
  - `compute_modulation(strength: np.ndarray, gains: Gains) -> Modulation`:
    - `brightness = 1 + gains.brightness * strength`
    - `size = 1 + gains.size * strength`
    - `saturation = 1 + gains.saturation * strength`
    - `contrast = gains.contrast * strength` (an unsharp *amount*, not a multiplier)
  - A gain of `0.0` yields the identity (`1` for multipliers, `0` for contrast) regardless of strength.

- [ ] **Step 1: Write the failing test**

`tests/test_modulate.py`:
```python
import numpy as np
import pytest
from gaia_depth_grade.config import Gains
from gaia_depth_grade.modulate import compute_modulation, Modulation


def test_near_brightens_far_dims():
    s = np.array([1.0, -1.0])
    m = compute_modulation(s, Gains(brightness=0.5, size=0.4, contrast=0.3, saturation=0.3))
    assert isinstance(m, Modulation)
    assert m.brightness[0] == pytest.approx(1.5)
    assert m.brightness[1] == pytest.approx(0.5)
    assert m.size[0] > 1.0 and m.size[1] < 1.0


def test_zero_gain_is_identity():
    s = np.array([1.0, -0.7, 0.3])
    m = compute_modulation(s, Gains(brightness=0.0, size=0.0, contrast=0.0, saturation=0.0))
    assert np.allclose(m.brightness, 1.0)
    assert np.allclose(m.size, 1.0)
    assert np.allclose(m.saturation, 1.0)
    assert np.allclose(m.contrast, 0.0)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_modulate.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.modulate`.

- [ ] **Step 3: Implement `modulate.py`**

```python
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .config import Gains


@dataclass(frozen=True)
class Modulation:
    brightness: np.ndarray
    size: np.ndarray
    contrast: np.ndarray
    saturation: np.ndarray


def compute_modulation(strength: np.ndarray, gains: Gains) -> Modulation:
    s = np.asarray(strength, dtype=float)
    return Modulation(
        brightness=1.0 + gains.brightness * s,
        size=1.0 + gains.size * s,
        contrast=gains.contrast * s,
        saturation=1.0 + gains.saturation * s,
    )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest tests/test_modulate.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Commit**

```bash
git add src/gaia_depth_grade/modulate.py tests/test_modulate.py
git commit -m "feat: per-attribute modulation deltas from depth strength"
```

---

### Task 8: Render the modulated star layer

**Files:**
- Create: `src/gaia_depth_grade/render.py`
- Test: `tests/test_render.py`

**Interfaces:**
- Consumes: stars-layer image, detection `Table` (`x, y, flux, fwhm`), `Modulation` (Task 7), `base_sigma_px` from config.
- Produces:
  - `render_stars(stars_layer: np.ndarray, detected, modulation: Modulation, base_sigma_px: float) -> np.ndarray`. Image may be 2-D (mono) or 3-D `(ny, nx, 3)` (color). For each star, within a window of radius `ceil(4*base_sigma_px)`:
    - **brightness:** multiply the window pixels by `modulation.brightness[i]`.
    - **size/glow:** add a Gaussian halo of integrated weight `(size[i] - 1)` × local flux, sigma `base_sigma_px * size[i]`.
    - **local contrast:** unsharp the window by `modulation.contrast[i]` (`win += amount*(win - blur(win))`).
    - **saturation (color only):** push window chroma around its luminance by `saturation[i]`.
  - Output is clipped to `[0, 1]`. The function never mutates the input array.

- [ ] **Step 1: Write the failing test**

`tests/test_render.py`:
```python
import numpy as np
import pytest
from astropy.table import Table
from gaia_depth_grade.modulate import Modulation
from gaia_depth_grade.render import render_stars


def _one_star_layer(flux=0.5, x=32, y=32, sigma=2.0):
    ny = nx = 64
    yy, xx = np.mgrid[0:ny, 0:nx]
    img = flux * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma**2))
    det = Table()
    det["x"] = [float(x)]; det["y"] = [float(y)]
    det["flux"] = [flux]; det["fwhm"] = [2.355 * sigma]
    return img, det


def test_brightness_up_increases_peak():
    img, det = _one_star_layer()
    base_peak = img.max()
    m = Modulation(brightness=np.array([1.5]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert out.max() > base_peak
    assert np.shares_memory(out, img) is False  # input not mutated


def test_brightness_down_decreases_peak():
    img, det = _one_star_layer()
    base_peak = img.max()
    m = Modulation(brightness=np.array([0.5]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert out.max() < base_peak


def test_size_up_widens_footprint():
    img, det = _one_star_layer()
    def above_half(a):
        return int((a > a.max() * 0.5).sum())
    base_area = above_half(img)
    m = Modulation(brightness=np.array([1.0]), size=np.array([1.6]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert above_half(out) > base_area


def test_output_clipped_and_color_shape_preserved():
    img, det = _one_star_layer()
    color = np.stack([img, img * 0.5, img * 0.2], axis=-1)
    m = Modulation(brightness=np.array([2.0]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.3]))
    out = render_stars(color, det, m, base_sigma_px=2.0)
    assert out.shape == color.shape
    assert out.max() <= 1.0 and out.min() >= 0.0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_render.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.render`.

- [ ] **Step 3: Implement `render.py`**

```python
from __future__ import annotations

import math

import numpy as np
from scipy.ndimage import gaussian_filter


def _window_slice(x, y, rad, shape):
    ny, nx = shape[0], shape[1]
    x0, x1 = max(0, int(x) - rad), min(nx, int(x) + rad + 1)
    y0, y1 = max(0, int(y) - rad), min(ny, int(y) + rad + 1)
    return slice(y0, y1), slice(x0, x1)


def _gaussian_stamp(h, w, cx, cy, sigma):
    yy, xx = np.mgrid[0:h, 0:w]
    return np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * sigma**2))


def render_stars(stars_layer, detected, modulation, base_sigma_px):
    out = np.array(stars_layer, dtype=float, copy=True)
    is_color = out.ndim == 3
    rad = int(math.ceil(4 * base_sigma_px))

    for i in range(len(detected)):
        x, y = float(detected["x"][i]), float(detected["y"][i])
        flux = float(detected["flux"][i])
        b = float(modulation.brightness[i])
        zsize = float(modulation.size[i])
        camount = float(modulation.contrast[i])
        sat = float(modulation.saturation[i])

        ys, xs = _window_slice(x, y, rad, out.shape)
        win = out[ys, xs]
        if win.size == 0:
            continue

        # brightness
        win *= b

        # size/glow: add a Gaussian halo of extra integrated weight
        if abs(zsize - 1.0) > 1e-9:
            h, w = win.shape[0], win.shape[1]
            cx, cy = x - xs.start, y - ys.start
            stamp = _gaussian_stamp(h, w, cx, cy, base_sigma_px * zsize)
            stamp /= stamp.sum() if stamp.sum() > 0 else 1.0
            extra = (zsize - 1.0) * flux
            if is_color:
                for c in range(win.shape[2]):
                    win[..., c] += extra * stamp
            else:
                win += extra * stamp

        # local contrast (unsharp)
        if abs(camount) > 1e-9:
            blur = gaussian_filter(win, sigma=base_sigma_px, axes=(0, 1) if is_color else None)
            win += camount * (win - blur)

        # saturation (color only)
        if is_color and abs(sat - 1.0) > 1e-9:
            lum = win.mean(axis=2, keepdims=True)
            win[:] = lum + sat * (win - lum)

        out[ys, xs] = win

    np.clip(out, 0.0, 1.0, out=out)
    return out
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest tests/test_render.py -v`
Expected: PASS (4 passed).

- [ ] **Step 5: Commit**

```bash
git add src/gaia_depth_grade/render.py tests/test_render.py
git commit -m "feat: render depth-modulated star layer (brightness/size/contrast/saturation)"
```

---

### Task 9: CLI orchestration with FITS I/O and honesty metadata

**Files:**
- Create: `src/gaia_depth_grade/cli.py`
- Test: `tests/test_e2e_synthetic.py`

**Interfaces:**
- Consumes: everything above; `DistanceSource` (injectable for tests).
- Produces:
  - `grade_array(image, header, config, source: DistanceSource) -> tuple[np.ndarray, dict]`. Returns `(graded_image, qa)` where `qa` includes `n_detected`, `n_matched`, `match_rate`, `median_offset_px`, `low_match_warning` (bool). Pipeline: pick luminance (mono image, or mean over channels for color) for detection → `field_footprint` → `source.distances_for` → `cross_match` → `effective_strength` → `compute_modulation` → `render_stars`. If `match_rate < config.min_match_rate`, set `low_match_warning=True` and log a loud warning.
  - `write_fits(path, image, header, qa)` — writes the graded image, appends the verbatim honesty `HISTORY` line and `DEPTHTAG` keyword, and writes `<path>.qa.json`.
  - `main(argv=None)` — argparse: `grade <in.fits> <out.fits> [--config c.toml]` and `debug <in.fits> <overlay.fits> [--config]`. `grade` uses `GaiaStarSource(config.cache_dir)`.
- Honesty tag verbatim: `GAIA depth-derived; physically motivated, not per-pixel correct for gas`.

- [ ] **Step 1: Write the failing E2E test (injected fake source, no network)**

`tests/test_e2e_synthetic.py`:
```python
import json
import numpy as np
import pytest
from astropy.io import fits
from astropy.table import Table
from gaia_depth_grade.config import GradeConfig, Gains
from gaia_depth_grade.distances import DistanceSource
from gaia_depth_grade.cli import grade_array, write_fits

HONESTY = "GAIA depth-derived; physically motivated, not per-pixel correct for gas"


class FakeSource(DistanceSource):
    """Two stars: a near one (100pc) and a far one (2000pc)."""
    def __init__(self, wcs, near_xy, far_xy):
        self._w = wcs; self._near = near_xy; self._far = far_xy

    def distances_for(self, footprint):
        sky = self._w.pixel_to_world(
            [self._near[0], self._far[0]], [self._near[1], self._far[1]])
        t = Table()
        t["ra"] = sky.ra.deg; t["dec"] = sky.dec.deg
        t["r_med_geo"] = [100.0, 2000.0]
        t["r_lo_geo"] = [98.0, 1900.0]; t["r_hi_geo"] = [102.0, 2100.0]
        t["source_id"] = [1, 2]
        return t


def _two_star_frame(simple_wcs_header):
    from gaia_depth_grade.wcs import load_wcs
    w = load_wcs(simple_wcs_header)
    ny, nx = 200, 300
    yy, xx = np.mgrid[0:ny, 0:nx]
    near = (90.0, 100.0); far = (210.0, 100.0)
    img = np.zeros((ny, nx))
    for (x, y) in (near, far):
        img += 0.4 * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * 2.0**2))
    return w, img, near, far


def test_near_brightens_far_dims_end_to_end(simple_wcs_header):
    w, img, near, far = _two_star_frame(simple_wcs_header)
    cfg = GradeConfig(gains=Gains(brightness=0.6, size=0.0, contrast=0.0, saturation=0.0),
                      p_low=0, p_high=100, min_match_rate=0.0)
    src = FakeSource(w, near, far)
    graded, qa = grade_array(img, simple_wcs_header, cfg, src)
    near_peak = graded[96:104, 86:94].max()
    far_peak = graded[96:104, 206:214].max()
    assert near_peak > 0.4   # near star brightened above original 0.4
    assert far_peak < 0.4    # far star dimmed
    assert qa["n_matched"] == 2
    assert qa["low_match_warning"] is False


def test_write_fits_has_honesty_tag(tmp_path, simple_wcs_header):
    w, img, near, far = _two_star_frame(simple_wcs_header)
    out = tmp_path / "g.fits"
    write_fits(str(out), img, simple_wcs_header, {"match_rate": 1.0})
    hdr = fits.getheader(str(out))
    assert HONESTY in str(hdr.get("DEPTHTAG", "")) or any(HONESTY in str(c) for c in hdr["HISTORY"])
    qa = json.loads((tmp_path / "g.fits.qa.json").read_text())
    assert qa["match_rate"] == 1.0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_e2e_synthetic.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.cli`.

- [ ] **Step 3: Implement `cli.py`**

```python
from __future__ import annotations

import argparse
import json
import logging

import numpy as np
from astropy.io import fits

from .config import GradeConfig, load_config
from .detect import detect_stars
from .distances import DistanceSource, GaiaStarSource
from .match import cross_match
from .modulate import compute_modulation
from .render import render_stars
from .transform import effective_strength
from .wcs import field_footprint, load_wcs

log = logging.getLogger(__name__)
HONESTY = "GAIA depth-derived; physically motivated, not per-pixel correct for gas"


def _luminance(image: np.ndarray) -> np.ndarray:
    return image.mean(axis=2) if image.ndim == 3 else image


def grade_array(image, header, config: GradeConfig, source: DistanceSource):
    wcs = load_wcs(header)
    lum = _luminance(image)
    detected = detect_stars(lum, config.detect_fwhm, config.detect_threshold_sigma)
    fp = field_footprint(wcs, lum.shape)
    catalog = source.distances_for(fp)
    matched, stats = cross_match(detected, catalog, wcs, config.match_tolerance_px)
    strength = effective_strength(
        np.asarray(matched["r_med_geo"]), np.asarray(matched["r_lo_geo"]),
        np.asarray(matched["r_hi_geo"]), config.p_low, config.p_high, config.neutral_strength)
    modulation = compute_modulation(strength, config.gains)
    graded = render_stars(image, matched, modulation, config.base_sigma_px)

    low = stats.match_rate < config.min_match_rate
    if low:
        log.warning("LOW MATCH RATE %.2f < %.2f — depth grade is unreliable",
                    stats.match_rate, config.min_match_rate)
    qa = {
        "n_detected": stats.n_detected, "n_matched": stats.n_matched,
        "match_rate": stats.match_rate, "median_offset_px": stats.median_offset_px,
        "low_match_warning": bool(low),
    }
    return graded, qa


def write_fits(path, image, header, qa):
    data = np.moveaxis(image, -1, 0) if image.ndim == 3 else image
    hdr = header.copy()
    hdr["DEPTHTAG"] = HONESTY[:68]
    hdr.add_history(HONESTY)
    fits.PrimaryHDU(data=data.astype(np.float32), header=hdr).writeto(path, overwrite=True)
    with open(path + ".qa.json", "w") as fh:
        json.dump(qa, fh, indent=2)


def _read_image(path):
    with fits.open(path) as hdul:
        data = hdul[0].data.astype(float)
        header = hdul[0].header
    if data.ndim == 3:               # FITS stores color as (3, ny, nx)
        data = np.moveaxis(data, 0, -1)
    return data, header


def main(argv=None):
    p = argparse.ArgumentParser("gaia_depth_grade")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("grade", "debug"):
        sp = sub.add_parser(name)
        sp.add_argument("input"); sp.add_argument("output")
        sp.add_argument("--config", default=None)
    args = p.parse_args(argv)

    logging.basicConfig(level=logging.INFO)
    cfg = load_config(args.config)
    image, header = _read_image(args.input)
    source = GaiaStarSource(cfg.cache_dir)
    graded, qa = grade_array(image, header, cfg, source)
    write_fits(args.output, graded, header, qa)
    log.info("wrote %s (qa: %s)", args.output, qa)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pytest tests/test_e2e_synthetic.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Run the full suite**

Run: `pytest -v`
Expected: all tasks' tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/gaia_depth_grade/cli.py tests/test_e2e_synthetic.py
git commit -m "feat: CLI orchestration, FITS I/O, honesty metadata, synthetic E2E"
```

---

### Task 10: PJSR harness wrapper

**Files:**
- Create: `pjsr/gaia_depth_grade.js`

**Interfaces:**
- Consumes: the installed `gaia_depth_grade` CLI (`python -m gaia_depth_grade.cli grade <in> <out>`); PI processes ImageSolver and StarXTerminator.
- Produces: a graded XISF on disk. This task is validated manually in the PI harness (GPU + StarXTerminator are not in CI), so it has no pytest step; its "test" is the documented harness run.

- [ ] **Step 1: Write the PJSR wrapper**

`pjsr/gaia_depth_grade.js`:
```javascript
// Gaia Depth Grade — thin PJSR harness wrapper.
// Orchestration only: solve, split stars, hand pixels to the Python core, recombine.
#include <pjsr/StdButton.jsh>

#define PY_BIN   "/home/scarter4work/projects/gaia-depth-grade/.venv/bin/python"
#define PKG      "gaia_depth_grade.cli"
#define TMP_DIR  "/tmp/gaia_depth_grade"

function run(cmd) {
   var p = new ExternalProcess(cmd);
   p.waitForFinished();
   if (p.exitStatus != ProcessExitStatus_NormalExit || p.exitCode != 0)
      throw new Error("command failed: " + cmd + "\n" + p.stderr);
}

function gradeWindow(view) {
   File.createDirectory(TMP_DIR, true);

   // 1. Plate-solve (writes WCS keywords into the view).
   var solver = new ImageSolver;
   solver.SolveImage(view);

   // 2. Split stars/starless with StarXTerminator.
   var sxt = new StarXTerminator;
   sxt.unscreen = true;          // produce a linear stars image
   sxt.executeOn(view);          // creates a "<id>_stars" window per SXT settings
   var stars = View.viewById(view.id + "_stars");
   var starless = view;          // SXT leaves starless in place

   // 3. Export stars layer (+WCS) to FITS for the Python core.
   var inPath  = TMP_DIR + "/stars_in.fits";
   var outPath = TMP_DIR + "/stars_out.fits";
   stars.window.saveAs(inPath, false, false, false, false);

   // 4. Run the Python depth grade.
   run([PY_BIN, "-m", PKG, "grade", inPath, outPath]);

   // 5. Read the modulated stars layer back.
   var graded = ImageWindow.open(outPath)[0];

   // 6. Recombine (screen) modulated stars over starless.
   var PM = new PixelMath;
   PM.expression = "combine(" + starless.id + ", " + graded.mainView.id + ", op_screen)";
   PM.createNewImage = true;
   PM.newImageId = view.id + "_depthgraded";
   PM.executeOn(starless);
}

gradeWindow(ImageWindow.activeWindow.mainView);
```

- [ ] **Step 2: Document the manual harness run**

Add to `pjsr/gaia_depth_grade.js` header comment (and the project README) the invocation, mirroring the NukeX harness pattern:
```bash
xvfb-run -a /opt/PixInsight/bin/PixInsight.sh --automation-mode \
  --run=/home/scarter4work/projects/gaia-depth-grade/pjsr/gaia_depth_grade.js
```

- [ ] **Step 3: Commit**

```bash
git add pjsr/gaia_depth_grade.js
git commit -m "feat: PJSR harness wrapper (solve, SXT split, python grade, recombine)"
```

---

## Self-Review

**Spec coverage:**
- §3 hybrid architecture → Tasks 9 (CLI/FITS boundary) + 10 (PJSR). ✓
- §4 modules: wcs(T2), detect(T3), distances+DistanceSource(T4), match(T5), transform(T6), modulate(T7), render(T8), cli(T9), config(T1). ✓
- §5 data flow → T9 `grade_array` chains the modules in spec order. ✓
- §6 modulation model → T7 formulas + T8 application. ✓
- §7 loud errors: missing WCS (T2), Gaia failure/empty (T4), stale-cache log (T4), low match rate (T9), honesty tag (T9). ✓
- §8 testing: unit (T2,T3,T6,T7,T8), component-distances against injected source (T4), synthetic E2E (T9), manual PI (T10). ✓
- §9 inputs/layout → file structure + T1 package. Configurable input path handled via CLI `input` arg + default documented in README (note: wire the `~/projects/processing/master` default into the PJSR/README, not the Python CLI, which takes an explicit path). ✓
- §10 data sources → Gaia/Bailer-Jones (T4), plate-solve + SXT (T10). ✓
- Phase 2 (dust) is intentionally out of this plan; `DistanceSource` ABC (T4) is the seam it will extend. ✓

**Placeholder scan:** No TBD/TODO; every code step has complete code. ✓

**Type consistency:** `FieldFootprint` (T2) consumed by T4/T9; detection columns `x,y,flux,fwhm` produced T3, consumed T5/T8; `MatchStats` fields used in T9 qa dict; `Modulation` fields (brightness/size/contrast/saturation) produced T7, consumed T8; `DistanceSource.distances_for` signature consistent T4→T9 (FakeSource). Honesty string identical in T9 impl and T9 test. ✓
