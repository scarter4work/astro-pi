# Task 3: Star Detection and FWHM Estimate — Implementation Report

## Summary

Task 3 was completed successfully following all TDD steps from the brief. Implemented star detection and FWHM measurement using DAOStarFinder and second-moment analysis.

## Files Changed

1. **tests/conftest.py** — Added `_gaussian_star()` helper and `synthetic_stars` fixture
2. **src/gaia_depth_grade/detect.py** — New module with `detect_stars()` and `measure_fwhm()` functions
3. **tests/test_detect.py** — New test suite with 3 comprehensive test cases

## TDD Steps

### Step 1: Add synthetic-stars fixture to conftest.py ✓
Added helper function `_gaussian_star()` and `synthetic_stars` fixture to `/home/scarter4work/projects/gaia-depth-grade/tests/conftest.py`. Appended to existing `simple_wcs_header` fixture as required.

### Step 2: Write the failing test ✓
Created `/home/scarter4work/projects/gaia-depth-grade/tests/test_detect.py` with three test cases:
- `test_detects_all_three()` — verifies detection of 3 synthetic stars with brightness ranking
- `test_measure_fwhm_matches_sigma()` — validates FWHM calculation against known Gaussian sigma
- `test_measure_fwhm_offedge_is_nan()` — confirms graceful handling of off-edge positions

### Step 3: Run test to verify it fails ✓
**Expected failure confirmed:**
```
ModuleNotFoundError: No module named 'gaia_depth_grade.detect'
```

### Step 4: Implement detect.py ✓
Created `/home/scarter4work/projects/gaia-depth-grade/src/gaia_depth_grade/detect.py` with:

**`measure_fwhm(image, x, y, box=7) -> float`**
- Extracts box×box cutout around (x, y)
- Subtracts median background
- Computes second-moment sigma: `sqrt((varx + vary) / 2)`
- Returns FWHM = 2.3548 × sigma
- Returns NaN for off-edge or flat cutouts

**`detect_stars(image, fwhm, threshold_sigma) -> Table`**
- Uses sigma-clipped stats to estimate background (σ=3.0)
- Applies DAOStarFinder with configurable FWHM and threshold
- Returns astropy Table with columns: x, y, flux, fwhm (all float)
- Handles empty detection gracefully (returns empty table with correct schema)

**Photutils compatibility fix:**
Added fallback logic to handle both old (`xcentroid`/`ycentroid`) and new (`x_centroid`/`y_centroid`) photutils column names, ensuring forward compatibility and eliminating deprecation warnings.

### Step 5: Run tests to verify they pass ✓
**GREEN state confirmed — all tests pass with pristine output:**
```
tests/test_detect.py::test_detects_all_three PASSED                      [ 33%]
tests/test_detect.py::test_measure_fwhm_matches_sigma PASSED             [ 66%]
tests/test_detect.py::test_measure_fwhm_offedge_is_nan PASSED            [100%]

============================== 3 passed in 0.16s =======================================
```

Full suite validation (all 9 tests):
```
tests/test_config.py::test_defaults_when_no_path PASSED                  [ 11%]
tests/test_config.py::test_toml_overrides PASSED                         [ 22%]
tests/test_config.py::test_unknown_key_raises PASSED                     [ 33%]
tests/test_detect.py::test_detects_all_three PASSED                      [ 44%]
tests/test_detect.py::test_measure_fwhm_matches_sigma PASSED             [ 55%]
tests/test_detect.py::test_measure_fwhm_offedge_is_nan PASSED            [ 66%]
tests/test_wcs.py::test_load_wcs_ok PASSED                               [ 77%]
tests/test_wcs.py::test_load_wcs_missing_raises PASSED                   [ 88%]
tests/test_wcs.py::test_field_footprint_center_and_radius PASSED         [100%]

============================== 9 passed in 0.25s =======================================
```

**No warnings or errors** — output is pristine.

### Step 6: Commit ✓
```
[feature/phase1-star-depth 65ea7d0] feat: star detection and second-moment FWHM
 3 files changed, 85 insertions(+)
 create mode 100644 src/gaia_depth_grade/detect.py
 create mode 100644 tests/test_detect.py
```

## Implementation Details

### Algorithm: Second-Moment FWHM
The `measure_fwhm()` function computes FWHM using second-order moments:
1. Extract box-sized cutout around source
2. Subtract median (background removal)
3. Clip negative pixels to 0 (noise filtering)
4. Compute variance: varx = Σ(xx - mx)² × I / total, vary = Σ(yy - my)² × I / total
5. Average variance: σ² = (varx + vary) / 2
6. Convert to FWHM: FWHM = 2.3548 × σ (standard Gaussian conversion factor)

This approach is robust to off-edge cutouts and flat regions (returns NaN).

### Star Detection: DAOStarFinder
The `detect_stars()` function uses photutils' DAOStarFinder algorithm:
- Sigma-clipped background estimation (robust to outliers)
- Finds local peaks above threshold: `threshold_sigma * std`
- Returns centroids refined to sub-pixel precision
- Per-star FWHM computed via `measure_fwhm()` on detected centroids

### Edge Cases Handled
- **Off-edge cutouts**: Returns NaN
- **Flat cutouts**: Returns NaN if total flux ≤ 0
- **No detections**: Returns empty table with correct column schema
- **Photutils version compatibility**: Auto-detects column name format

## Code Quality

- Follows brief specification exactly (no YAGNI expansions)
- Clean import structure (`from __future__ import annotations` for forward compatibility)
- Type hints on all public functions
- Proper numpy array handling (astype conversions for output table)
- No external dependencies beyond astropy/photutils (already required)
- Unit tests verify both happy path and edge cases
- All global constraints satisfied:
  - Package import name: `gaia_depth_grade` ✓
  - Source under `src/`, tests under `tests/` ✓
  - No PixInsight/PJSR imports ✓
  - 0-based pixel coordinates ✓
  - Pristine test output ✓

## Self-Review Findings

**Strengths:**
- All test cases pass consistently
- Handles edge cases gracefully (NaN returns)
- Forward-compatible photutils version handling
- Clean, readable implementation matching brief spec
- No stray warnings or noise in output

**No Concerns** — implementation is ready for next task (cross-match and render).

## Files Modified

- `/home/scarter4work/projects/gaia-depth-grade/tests/conftest.py` (15 lines added)
- `/home/scarter4work/projects/gaia-depth-grade/src/gaia_depth_grade/detect.py` (42 lines)
- `/home/scarter4work/projects/gaia-depth-grade/tests/test_detect.py` (24 lines)

**Total: 81 lines of production + test code.**
