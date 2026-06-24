# Depth Grade Interactive UI — Implementation Plan

> **For agentic workers:** Implement task-by-task. Steps use `- [ ]` checkboxes.

**Goal:** An interactive PixInsight Script Dialog that runs the depth grade on a master with a two-phase Prepare→Preview→Execute flow, live gain sliders, and a whole-frame + 1:1 zoom preview.

**Architecture:** Approach A — PJSR dialog orchestrates; the Python core is split into gain-independent `prepare` (cached) and gain-dependent `preview`/`render`. Preview renders full-res once, screen-blends over starless, autostretches, downscales the result for display (and crops the 1:1 inset from it).

**Tech Stack:** Python 3.13 (numpy, astropy, photutils, astroquery, scipy, Pillow), PJSR (PixInsight 1.9.4).

## Global Constraints

- No silent fallbacks — surface real errors (CLAUDE.md).
- Preview screen-blend MUST equal Execute's PixelMath: `result = 1 - (1-stars)*(1-starless)`.
- `grade` (headless harness path) output MUST stay equal to `prepare`+`render`.
- All Python changes are TDD with real tests; PJSR dialog is build + live-validation (no CI — needs GPU/SXT).

---

### Task 1: Split grade into prepare/render stages (`pipeline.py`)

**Files:**
- Create: `src/gaia_depth_grade/pipeline.py`
- Modify: `src/gaia_depth_grade/cli.py` (grade_array delegates to pipeline)
- Test: `tests/test_pipeline.py`

**Interfaces:**
- Produces: `prepare_grade(image, header, config, source) -> (matched: astropy.table.Table, qa: dict)`;
  `render_from_prep(stars_layer: np.ndarray, matched: Table, config) -> np.ndarray`.
- `matched` carries columns: `x, y, flux, r_med_geo, r_lo_geo, r_hi_geo` (only matched rows).
- `qa` keys: `n_detected, n_matched, match_rate, median_offset_px, low_match_warning`.

- [ ] Step 1: Write `tests/test_pipeline.py::test_prepare_then_render_equals_grade` using the synthetic fixture (reuse `tests/test_e2e_synthetic.py` helpers / conftest), asserting `render_from_prep(img, *prepare_grade(...))` equals `grade_array(...)[0]` (np.allclose) and qa dicts match.
- [ ] Step 2: Run `pytest tests/test_pipeline.py -v` → FAIL (module missing).
- [ ] Step 3: Implement `pipeline.py`: `prepare_grade` = wcs/detect/footprint/query/cross_match → keep only matched rows into `matched` Table + qa; `render_from_prep` = effective_strength(matched r_*; p_low/p_high/neutral) → compute_modulation(gains) → render_stars(stars_layer, matched, mod, base_sigma). Refactor `grade_array` to call both.
- [ ] Step 4: Run `pytest -q` → PASS (all, incl. existing 29).
- [ ] Step 5: Commit.

### Task 2: Prep cache round-trip (`cache.py`)

**Files:**
- Create: `src/gaia_depth_grade/cache.py`
- Test: `tests/test_cache.py`

**Interfaces:**
- Produces: `save_prep(cache_dir: str, matched: Table, qa: dict) -> None`; `load_prep(cache_dir) -> (Table, dict)`.
- Layout: `<cache_dir>/prep.npz` (columns as arrays), `<cache_dir>/qa.json`.

- [ ] Step 1: `tests/test_cache.py::test_roundtrip` — build a small matched Table + qa, save, load, assert columns and qa equal.
- [ ] Step 2: Run → FAIL.
- [ ] Step 3: Implement save/load (np.savez for columns; json for qa; recreate Table on load).
- [ ] Step 4: Run → PASS.
- [ ] Step 5: Commit.

### Task 3: Display helpers (`display.py`)

**Files:**
- Create: `src/gaia_depth_grade/display.py`
- Test: `tests/test_display.py`

**Interfaces:**
- `screen_blend(stars, starless) -> np.ndarray` == `1-(1-a)*(1-b)`.
- `autostretch(rgb) -> np.ndarray uint8` (STF-style MTF; median/MAD, target bg 0.25).
- `write_png(arr_uint8_or_float, path, max_width=None, region=None)` — region=(x,y,w,h) crops first (1:1), max_width downscales.

- [ ] Step 1: `tests/test_display.py`: `test_screen_blend_formula` (np.allclose to 1-(1-a)(1-b)); `test_autostretch_range` (uint8, brightens a dark median); `test_write_png_region_and_downscale` (PNG opens at expected size for region crop and for max_width downscale).
- [ ] Step 2: Run → FAIL.
- [ ] Step 3: Implement; use Pillow for PNG.
- [ ] Step 4: Run → PASS.
- [ ] Step 5: Commit (add pillow to pyproject deps).

### Task 4: CLI subcommands prepare/preview/render

**Files:**
- Modify: `src/gaia_depth_grade/cli.py`
- Test: `tests/test_cli_subcommands.py`

**Interfaces (CLI):**
- `prepare <stars.fits> <cache_dir> [--config T]`
- `preview <cache_dir> <stars.fits> <starless.fits> <out_full.png> [--inset <png> --region x,y,w,h] --gains b,s,c,sat --p-low F --p-high F --base-sigma F`
- `render <cache_dir> <stars.fits> <out.fits> --gains b,s,c,sat --p-low F --p-high F --base-sigma F`
- `grade` unchanged.

- [ ] Step 1: `tests/test_cli_subcommands.py` (mock Gaia via the existing source-injection or monkeypatch as test_distances does): `test_grade_equals_prepare_then_render` (run prepare→render on a synthetic stars FITS, compare graded FITS to `grade` output); `test_preview_writes_pngs` (full + inset of expected sizes); `test_render_uses_cache`.
- [ ] Step 2: Run → FAIL.
- [ ] Step 3: Implement subcommands wiring prepare_grade/render_from_prep/cache/display; parse `--gains` as 4 floats; build a GradeConfig overriding gains/p_low/p_high/base_sigma.
- [ ] Step 4: Run `pytest -q` → PASS.
- [ ] Step 5: Commit.

### Task 5: Extract shared PJSR lib

**Files:**
- Create: `pjsr/gaia_depth_grade_lib.jsh` (solver preamble macros are include-level; export `run`, `splitStars`, `writeWcsKeywords`)
- Modify: `pjsr/gaia_depth_grade.js` to `#include` the lib.

- [ ] Step 1: Move `run`, `splitStars`, `writeWcsKeywords` (and the solver `#define`/`#include` preamble) into `gaia_depth_grade_lib.jsh`. Harness includes it and keeps `gradeWindow`/`resolveTarget`/`main`.
- [ ] Step 2: Live-validate: run the harness headless on the master via the recipe; confirm `depthgraded.xisf` + qa (match_rate ~0.43) still produced. Screenshot console on error.
- [ ] Step 3: Commit.

### Task 6: Build the dialog (`GaiaDepthGradeDialog.js`)

**Files:**
- Create: `pjsr/GaiaDepthGradeDialog.js`

- [ ] Step 1: Implement dialog: ViewList+Browse target; preview Control (whole-frame Bitmap) + inset Control; live NumericControls (brightness/size/contrast/saturation/base_sigma/p_low/p_high); prepare-time fields (detect_threshold_sigma/match_tolerance_px); Prepare/Preview/Execute/Close buttons; status Label. State machine Idle→Prepared→preview/execute. Prepare: solve+splitStars+writeWcsKeywords (from lib) → save stars_full/starless_full → `py prepare` → show qa. Preview: `py preview` → load PNGs into controls; click whole-frame sets inset region. Execute: `py render` → PixelMath screen-blend(graded, starless) → new window. All errors surfaced to status + MessageBox.
- [ ] Step 2: Live-validate in PI (headless via Xvfb + screenshot, then note for interactive run): open dialog driver that constructs the dialog non-modally OR a test entry that runs Prepare+Preview+Execute programmatically and screenshots the preview. Capture a screenshot for the user.
- [ ] Step 3: Commit.

### Task 7: End-to-end live validation + screenshot for user

- [ ] Step 1: Drive the dialog (or its Prepare/Preview/Execute methods) headless on the real master; produce a preview PNG and the final window; screenshot the dialog with the preview visible.
- [ ] Step 2: Embed the screenshot for the user.
- [ ] Step 3: Final commit; update memory.
