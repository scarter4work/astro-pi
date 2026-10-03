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

