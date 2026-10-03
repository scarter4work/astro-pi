# Final-Review Fixes Report

Date: 2026-06-23  
Branch: feature/phase1-star-depth  
Commit: f310c85

---

## Changes Made

### 1. `debug` subcommand fails loudly (cli.py)
Inserted after `args = p.parse_args(argv)` and before `logging.basicConfig(...)`:
```python
if args.cmd == "debug":
    raise SystemExit("debug mode (depth-colored overlay) is not implemented in Phase 1")
```
No silent fallback — running `gaia_depth_grade debug ...` now exits immediately with a clear message.

### 2. TAP-failure test added (tests/test_distances.py)
New test `test_tap_failure_raises_runtimeerror`: monkeypatches `_run_query` to raise `ConnectionError("network down")`, asserts `RuntimeError` is raised and `"network down"` appears in the message (verifying the cause is preserved via `raise ... from exc`).

### 3. Render no-mutation test strengthened (tests/test_render.py)
In `test_brightness_up_increases_peak`: captured `img_before = img.copy()` before the render call, and added:
```python
assert np.array_equal(img, img_before)  # input values untouched, not just non-shared
```
This checks value equality in addition to the existing `np.shares_memory` check.

### 4. Dead conditional removed (match.py)
Changed `0.0 if n else 0.0` to `0.0` — the conditional was always evaluating to `0.0`.

### 5. Unused imports removed
- `wcs.py`: removed `from astropy.coordinates import SkyCoord` — confirmed `SkyCoord` was not referenced anywhere in the file.
- `detect.py`: changed `mean, median, std = sigma_clipped_stats(...)` to `_, median, std = sigma_clipped_stats(...)` — `mean` was unpacked but never used.

### 6. sha1 intent flagged (distances.py)
Changed `hashlib.sha1(key.encode())` to `hashlib.sha1(key.encode(), usedforsecurity=False)` — makes intent explicit that sha1 is used as a cache key, not for security.

---

## Test Run

Command: `. .venv/bin/activate && pytest -v`

```
============================= test session starts ==============================
platform linux -- Python 3.14.3, pytest-9.1.1, pluggy-1.6.0
collected 29 items

tests/test_config.py::test_defaults_when_no_path PASSED
tests/test_config.py::test_toml_overrides PASSED
tests/test_config.py::test_unknown_key_raises PASSED
tests/test_detect.py::test_detects_all_three PASSED
tests/test_detect.py::test_measure_fwhm_matches_sigma PASSED
tests/test_detect.py::test_measure_fwhm_offedge_is_nan PASSED
tests/test_distances.py::test_build_adql_contains_join_and_cone PASSED
tests/test_distances.py::test_gaiastarsource_caches PASSED
tests/test_distances.py::test_empty_query_raises PASSED
tests/test_distances.py::test_tap_failure_raises_runtimeerror PASSED   ← new
tests/test_distances.py::test_is_distance_source PASSED
tests/test_e2e_synthetic.py::test_near_brightens_far_dims_end_to_end PASSED
tests/test_e2e_synthetic.py::test_write_fits_has_honesty_tag PASSED
tests/test_match.py::test_match_within_tolerance PASSED
tests/test_match.py::test_no_catalog_zero_matches PASSED
tests/test_modulate.py::test_near_brightens_far_dims PASSED
tests/test_modulate.py::test_zero_gain_is_identity PASSED
tests/test_render.py::test_brightness_up_increases_peak PASSED         ← strengthened
tests/test_render.py::test_brightness_down_decreases_peak PASSED
tests/test_render.py::test_size_up_widens_footprint PASSED
tests/test_render.py::test_output_clipped_and_color_shape_preserved PASSED
tests/test_transform.py::test_near_is_plus_one_far_is_minus_one PASSED
tests/test_transform.py::test_nan_maps_to_neutral PASSED
tests/test_transform.py::test_confidence_tight_vs_loose PASSED
tests/test_transform.py::test_effective_strength_attenuates_noisy PASSED
tests/test_transform.py::test_degenerate_equal_distances_are_neutral PASSED
tests/test_wcs.py::test_load_wcs_ok PASSED
tests/test_wcs.py::test_load_wcs_missing_raises PASSED
tests/test_wcs.py::test_field_footprint_center_and_radius PASSED

============================== 29 passed in 0.19s ==============================
```

---

## Notable

- Instructions said "expect 30 total" — actual count is 29. The render test was strengthened (assertions added) but not split into a separate test, so no net new test there. The only genuinely new test is `test_tap_failure_raises_runtimeerror` (+1 from 28 → 29). All 29 pass cleanly.
- `SkyCoord` removal was safe: grep confirmed it appears only on the import line.
- No improvisation beyond the specified list.
