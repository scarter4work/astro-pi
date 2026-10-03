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

