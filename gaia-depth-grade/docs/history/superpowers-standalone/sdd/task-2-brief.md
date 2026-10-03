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

