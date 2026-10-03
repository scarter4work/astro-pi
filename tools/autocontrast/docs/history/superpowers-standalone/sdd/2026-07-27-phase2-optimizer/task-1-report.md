# Task 1: Action Space — Implementation Report

## Summary

Implemented the discretized action space (design SS6.2) for the Phase 2 optimizer. The implementation enforces band-limiting at construction time to prevent the optimizer from proposing unresolvable detail sharpening (SS2.2). Chroma actions are gated by palette compatibility (SS2.3). Once-only actions (star_split, background_neutralize) are withheld after application.

## Files Created

- `src/autocontrast/optimize/__init__.py` — module docstring
- `src/autocontrast/optimize/actions.py` — Action dataclass, layer/scale conversion, available_actions factory
- `tests/test_optimize_actions.py` — test suite (6 tests)

## Implementation Details

### Key Components

1. **Action dataclass** (frozen, hashable):
   - Fields: `kind`, `level`, `scale_arcsec`, `params`
   - Properties: `key` (stable recipe identity), `strength` (unitless magnitude)

2. **Scale/layer conversion**:
   - `scale_for_layer(layer, pixel_scale_arcsec)` → 2^layer * pixel_scale (matches energy.py convention)
   - `layer_for_scale(scale_arcsec, pixel_scale_arcsec)` → inverse, with guard against sub-pixel scales

3. **Action generation** (`available_actions`):
   - Filters bands >= PSF FWHM (SS2.2 band limit enforcement)
   - Emits 2 scale-denominated kinds (local_contrast, local_equalize) × scales × 3 levels
   - Emits 4 scale-free kinds (tonal_reshape, black_point, core_hdr, chroma) × 3 levels
   - Emits 2 once-only modes (background_neutralize, star_split) unless already applied
   - Chroma actions withheld when `palette_compatible=False` (SS2.3)

### Deviation from Brief

The brief's final return statement contained a no-op filter:
```python
return [a for a in actions if a.kind not in applied_kinds or a.kind not in ONCE_ONLY]
```

Per instruction, replaced with:
```python
return actions
```

This is correct because:
- ONCE_ONLY actions are never appended if they're in `applied_kinds` (guard at append time)
- Non-ONCE_ONLY kinds always satisfy the filter (second clause is unconditionally True)
- Filter is logically redundant; the guard achieves the intent

## Test Execution

### Failing run (Step 2)
```
ERROR collecting tests/test_optimize_actions.py
ModuleNotFoundError: No module named 'autocontrast.optimize'
```

### Passing run (Step 4)
```
tests/test_optimize_actions.py::test_layer_and_scale_are_inverse PASSED  [ 16%]
tests/test_optimize_actions.py::test_action_key_is_stable_and_distinguishing PASSED [ 33%]
tests/test_optimize_actions.py::test_no_action_is_offered_below_the_psf_limit PASSED [ 50%]
tests/test_optimize_actions.py::test_chroma_actions_are_withheld_when_palette_is_incompatible PASSED [ 66%]
tests/test_optimize_actions.py::test_once_only_actions_are_not_reoffered PASSED [ 83%]
tests/test_optimize_actions.py::test_scale_below_pixel_scale_is_rejected_outright PASSED [100%]

============================== 6 passed in 0.01s ===============================
```

## Commit

```
[phase2-optimizer b6c8169] optimize: band-limited action space refuses unresolvable scales
 3 files changed, 161 insertions(+)
 create mode 100644 src/autocontrast/optimize/__init__.py
 create mode 100644 src/autocontrast/optimize/actions.py
 create mode 100644 tests/test_optimize_actions.py
```

**Commit SHA:** `b6c8169`

## Self-Review

- ✓ All 6 tests pass on first run
- ✓ Action frozen dataclass enforces immutability for recipe caching
- ✓ Scale/layer rounding matches energy.py convention (plane i at 2^i * pixel_scale)
- ✓ PSF band limit enforcement is structural (actions not appended if scale < FWHM)
- ✓ Chroma gating works correctly via `if palette_compatible` guard
- ✓ Once-only actions properly withheld via guard-at-append-time (not filter-at-return)
- ✓ ValueError raised for sub-pixel scales (layer_for_scale guard)
- ✓ No dependencies added; uses only stdlib math and dataclasses

No concerns. Implementation is complete and correct.

---

## Fix Round 1: Review Findings

### Finding 1 — Silent fallback in `Action.strength`

**Issue:** The `strength` property used `_STRENGTH.get(self.level, 1.0)`, which silently returned 1.0 for unrecognized levels (e.g., typo "moderatte"), violating the no-silent-fallbacks constraint (§12).

**Fix:** Rewrote `strength` property to:
- Return 1.0 explicitly for `level == ""` (legitimate for mode-change actions)
- Raise `ValueError` for any other level not in `_STRENGTH`, naming the valid levels

**Tests added:**
- `test_empty_level_is_valid_for_mode_changes()` — asserts `level=""` returns 1.0
- `test_invalid_level_raises_value_error()` — asserts invalid level raises ValueError

### Finding 2 — Vacuous test `test_once_only_actions_are_not_reoffered`

**Issue:** Only tested negative assertion (actions absent when spent). Did not verify these actions are present when unspent, so an implementation that omitted them entirely would still pass.

**Fix:** Added `test_once_only_actions_are_offered_when_unspent()` to assert both `star_split` and `background_neutralize` appear exactly once each when `applied_kinds=frozenset()`.

### Test Run

```
.venv/bin/python -m pytest tests/test_optimize_actions.py -v
```

**Output:**
```
tests/test_optimize_actions.py::test_layer_and_scale_are_inverse PASSED  [ 11%]
tests/test_optimize_actions.py::test_action_key_is_stable_and_distinguishing PASSED [ 22%]
tests/test_optimize_actions.py::test_no_action_is_offered_below_the_psf_limit PASSED [ 33%]
tests/test_optimize_actions.py::test_chroma_actions_are_withheld_when_palette_is_incompatible PASSED [ 44%]
tests/test_optimize_actions.py::test_once_only_actions_are_not_reoffered PASSED [ 55%]
tests/test_optimize_actions.py::test_once_only_actions_are_offered_when_unspent PASSED [ 66%]
tests/test_optimize_actions.py::test_scale_below_pixel_scale_is_rejected_outright PASSED [ 77%]
tests/test_optimize_actions.py::test_empty_level_is_valid_for_mode_changes PASSED [ 88%]
tests/test_optimize_actions.py::test_invalid_level_raises_value_error PASSED [100%]

============================== 9 passed in 0.01s ===============================
```

### Commit

```
[phase2-optimizer d01489a] optimize: fix two Important review findings
 2 files changed, 34 insertions(+)
```

**Commit SHA:** `d01489a`
