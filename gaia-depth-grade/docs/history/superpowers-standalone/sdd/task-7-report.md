# Task 7: Modulation deltas from strength + gains — Report

## Status: DONE

## What Was Implemented

Following strict TDD, I created:

1. **`src/gaia_depth_grade/modulate.py`**: 
   - `Modulation` frozen dataclass with fields: `brightness`, `size`, `contrast`, `saturation` (all `np.ndarray`)
   - `compute_modulation(strength: np.ndarray, gains: Gains) -> Modulation` function

2. **`tests/test_modulate.py`**:
   - `test_near_brightens_far_dims`: Verifies that positive strength values brighten/enlarge and negative values dim/shrink
   - `test_zero_gain_is_identity`: Confirms that zero gains yield identity multipliers (1.0 for brightness/size/saturation, 0.0 for contrast)

## Formulas Implemented

Per the brief:
- `brightness = 1 + gains.brightness * strength`
- `size = 1 + gains.size * strength`
- `saturation = 1 + gains.saturation * strength`
- `contrast = gains.contrast * strength` (unsharp amount, not a multiplier)

A gain of 0.0 yields exact identity: multipliers return 1.0, contrast returns 0.0, regardless of strength.

## TDD Flow

### Step 1: Write Failing Tests
Wrote `tests/test_modulate.py` with two test cases.

### Step 2: Verify Failure (RED)
```bash
$ pytest tests/test_modulate.py -v
ModuleNotFoundError: No module named 'gaia_depth_grade.modulate'
```
✓ Expected failure confirmed.

### Step 3: Implement modulate.py
Implemented per spec:
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

### Step 4: Verify Passing Tests (GREEN)
```bash
$ pytest tests/test_modulate.py -v
tests/test_modulate.py::test_near_brightens_far_dims PASSED    [ 50%]
tests/test_modulate.py::test_zero_gain_is_identity PASSED      [100%]
============================== 2 passed in 0.00s ===============================
```

### Full Test Suite (22 tests)
```bash
$ pytest -v
...
============================== 22 passed in 0.29s ===============================
```
✓ All existing tests still pass.

### Step 5: Commit
```bash
$ git add src/gaia_depth_grade/modulate.py tests/test_modulate.py
$ git commit -m "feat: per-attribute modulation deltas from depth strength"
[feature/phase1-star-depth 759d7ce] feat: per-attribute modulation deltas from depth strength
 2 files changed, 47 insertions(+)
```

## Files Created

- `/home/scarter4work/projects/gaia-depth-grade/src/gaia_depth_grade/modulate.py` (26 lines)
- `/home/scarter4work/projects/gaia-depth-grade/tests/test_modulate.py` (18 lines)

## Self-Review Findings

✓ **Immutability**: `Modulation` is frozen, preventing accidental mutation  
✓ **Identity behavior**: Zero gains produce mathematically correct identity (1 for multipliers, 0 for contrast)  
✓ **Broadcasting**: `np.asarray(strength, dtype=float)` ensures scalar/array compatibility  
✓ **Type safety**: Dataclass with forward refs, clean imports  
✓ **Test coverage**: Two cases cover happy path (modulation with gains) and edge case (zero gains)  
✓ **No PixInsight/PJSR imports**: Pure Python + NumPy, meeting requirements  
✓ **Pristine output**: All pytest output clean, no warnings

## Concerns

None. Implementation is minimal, correct per spec, tested, and integrated cleanly.
