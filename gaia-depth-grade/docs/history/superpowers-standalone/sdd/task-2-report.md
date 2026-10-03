# Task 2 Report: WCS load and field footprint

## Summary
Implemented WCS loading and field footprint calculation for the Gaia Depth Grade package following TDD methodology. All tests passing; no concerns.

## Implementation Details

### Files Created
1. **tests/conftest.py** — Pytest fixture providing a WCS header (tangent-plane, RA=10 Dec=20, 1 arcsec/px, 200x300 image)
2. **tests/test_wcs.py** — Three test cases covering load_wcs validation, error handling, and field footprint computation
3. **src/gaia_depth_grade/wcs.py** — Core implementation with three exports

### Implementation Overview

**FieldFootprint** (frozen dataclass)
- Immutable struct with `center_ra`, `center_dec` (degrees), `radius_deg` 
- Represents the sky footprint of an image

**load_wcs(header) → WCS**
- Constructs astropy WCS from header
- Raises `ValueError("FITS header has no usable WCS")` if `wcs.has_celestial` is False
- Returns the celestial WCS

**field_footprint(wcs: WCS, shape: tuple[int, int]) → FieldFootprint**
- Maps image center pixel (using (nx-1)/2, (ny-1)/2 formula) to sky coordinates
- Calculates angular separation from center to all four corners
- Returns FieldFootprint with center RA/Dec and radius = max corner separation

## TDD Evidence

### Step 1: Conftest Fixture
Created tests/conftest.py with simple_wcs_header fixture per brief specification.

### Step 2: Failing Test
Created tests/test_wcs.py with three test cases.

### Step 3: RED — Test Run Before Implementation
```
$ pytest tests/test_wcs.py -v
ERROR collecting tests/test_wcs.py
ModuleNotFoundError: No module named 'gaia_depth_grade.wcs'
```
Expected failure confirmed.

### Step 4: Implementation
Created wcs.py with exact code from brief.

### Step 5: GREEN — Test Run After Implementation
```
$ pytest tests/test_wcs.py -v
tests/test_wcs.py::test_load_wcs_ok PASSED                               [ 33%]
tests/test_wcs.py::test_load_wcs_missing_raises PASSED                   [ 66%]
tests/test_wcs.py::test_field_footprint_center_and_radius PASSED         [100%]

============================== 3 passed in 0.01s ===============================
```

### Full Test Suite
```
$ pytest -v
tests/test_config.py::test_defaults_when_no_path PASSED                  [ 16%]
tests/test_config.py::test_toml_overrides PASSED                         [ 33%]
tests/test_config.py::test_unknown_key_raises PASSED                     [ 50%]
tests/test_wcs.py::test_load_wcs_ok PASSED                               [ 66%]
tests/test_wcs.py::test_load_wcs_missing_raises PASSED                   [ 83%]
tests/test_wcs.py::test_field_footprint_center_and_radius PASSED         [100%]

============================== 6 passed in 0.01s ===============================
```

### Step 6: Commit
```
$ git add src/gaia_depth_grade/wcs.py tests/test_wcs.py tests/conftest.py && git commit -m "feat: WCS load and field footprint"
[feature/phase1-star-depth 767d47e] feat: WCS load and field footprint
 3 files changed, 81 insertions(+)
 create mode 100644 src/gaia_depth_grade/wcs.py
 create mode 100644 tests/conftest.py
 create mode 100644 tests/test_wcs.py
```

## Code Quality Review

### Correctness
- All test assertions pass with tight tolerances (1e-3 for coordinates, 2e-3 for radius)
- Error handling matches specification: `ValueError` with exact message
- Frozen dataclass prevents accidental mutation
- WCS celestial projection correctly computed

### Design
- Follows brief specification exactly (no over-engineering)
- Single Responsibility: each function has one role
- Clear inputs/outputs with type hints
- Angles in degrees as required
- Pixel coordinates use correct 0-based indexing with center at (nx-1)/2, (ny-1)/2

### Dependencies
- astropy.wcs, numpy, dataclasses — all already in environment
- No PixInsight/PJSR imports (Python core only, as required)
- No extraneous imports

## Concerns
None. Implementation complete, all tests passing, brief requirements met exactly.
