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

