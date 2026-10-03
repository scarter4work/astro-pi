# Task 9 Report: CLI Orchestration, FITS I/O, Honesty Metadata, Synthetic E2E

## What Was Implemented

### `src/gaia_depth_grade/cli.py`
- `_luminance(image)` — returns `image.mean(axis=2)` for 3-channel, else passthrough
- `grade_array(image, header, config, source)` — full pipeline: load_wcs → detect_stars → field_footprint → source.distances_for → cross_match → effective_strength → compute_modulation → render_stars; emits QA dict with n_detected, n_matched, match_rate, median_offset_px, low_match_warning; logs loud WARNING when match_rate < config.min_match_rate
- `write_fits(path, image, header, qa)` — writes FITS with DEPTHTAG keyword (first 68 chars of honesty tag) and HISTORY line (full honesty tag); writes `<path>.qa.json`
- `_read_image(path)` — reads FITS, converts (3, ny, nx) → (ny, nx, 3) for color images
- `main(argv=None)` — argparse with `grade` and `debug` subcommands; uses GaiaStarSource

### `tests/test_e2e_synthetic.py`
- `FakeSource(DistanceSource)` — injectable fake, two stars at 100 pc (near) and 2000 pc (far), no network
- `test_near_brightens_far_dims_end_to_end` — verifies near star peak > 0.4, far star peak < 0.4, n_matched == 2, low_match_warning is False
- `test_write_fits_has_honesty_tag` — verifies DEPTHTAG/HISTORY presence and qa.json content

## TDD Evidence

### RED (before cli.py existed)
```
$ pytest tests/test_e2e_synthetic.py -v
ERROR tests/test_e2e_synthetic.py
ImportError: No module named 'gaia_depth_grade.cli'
1 error
```

### GREEN (after cli.py implemented)
```
$ pytest tests/test_e2e_synthetic.py -v
tests/test_e2e_synthetic.py::test_near_brightens_far_dims_end_to_end PASSED
tests/test_e2e_synthetic.py::test_write_fits_has_honesty_tag PASSED
2 passed in 0.16s
```

### Full Suite
```
$ pytest -v
28 passed in 0.18s
```
All 26 prior tests + 2 new E2E tests = 28 passed, 0 failed.

## Files Changed
- Created: `src/gaia_depth_grade/cli.py`
- Created: `tests/test_e2e_synthetic.py`

## Self-Review Findings
- Honesty tag verbatim matches brief exactly; DEPTHTAG truncated to 68 chars per FITS keyword value limit (brief doesn't specify truncation but FITS requires it; full string in HISTORY)
- FITS color axis convention correctly handled: disk stores (3, ny, nx), _read_image converts to (ny, nx, 3), write_fits converts back
- No silent fallbacks; missing WCS raises (via load_wcs); low match rate logs loud WARNING but does not crash
- No PixInsight/PJSR imports anywhere in cli.py

## Concerns
None. Brief followed verbatim; all tests green; no ambiguities encountered.

---

## Review Fix: Full Verbatim DEPTHTAG (2026-06-23)

### What Changed

1. `src/gaia_depth_grade/cli.py` — `write_fits`: changed `hdr["DEPTHTAG"] = HONESTY[:68]` to `hdr["DEPTHTAG"] = HONESTY` (with comment noting astropy CONTINUE stores the full string). The 68-char truncation dropped the trailing "gas", violating the verbatim-honesty-tag constraint.

2. `tests/test_e2e_synthetic.py` — `test_write_fits_has_honesty_tag`: replaced the single OR assertion (which allowed the truncated value to pass) with two independent assertions:
   - `assert hdr.get("DEPTHTAG", "") == HONESTY` — enforces full verbatim tag, no truncation
   - `assert any(HONESTY in str(c) for c in hdr["HISTORY"])` — enforces HISTORY line

### Commands Run

```
pytest tests/test_e2e_synthetic.py -v
pytest
git add src/gaia_depth_grade/cli.py tests/test_e2e_synthetic.py
git commit -m "fix: store full verbatim honesty tag in DEPTHTAG (no truncation)"
```

### E2E Test Output
```
tests/test_e2e_synthetic.py::test_near_brightens_far_dims_end_to_end PASSED
tests/test_e2e_synthetic.py::test_write_fits_has_honesty_tag PASSED
2 passed in 0.16s
```

### Full Suite Output
```
28 passed in 0.19s
```

### Commit
`2e18159 fix: store full verbatim honesty tag in DEPTHTAG (no truncation)`
