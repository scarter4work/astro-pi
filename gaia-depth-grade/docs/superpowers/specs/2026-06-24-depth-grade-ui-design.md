# Gaia Depth Grade — Interactive PixInsight UI (Script Dialog) — Design

**Date:** 2026-06-24
**Status:** Approved (design); spec under review
**Depends on:** Phase 1 harness (`pjsr/gaia_depth_grade.js`), Python core (`src/gaia_depth_grade/`)

## Goal

An interactive PixInsight Script Dialog to run the depth grade on an open master, with a
**live-ish preview** of the depth effect and per-gain tuning before committing. Replaces the
"edit a TOML / run the headless script and hope" workflow with target selection, sliders, a
preview pane, and explicit **Preview** and **Execute** buttons.

## Key constraint that shapes everything

The pipeline splits into:
- **Expensive, gain-independent** (~50 s, run once per target): plate-solve + StarXTerminator
  split + star detection + Gaia TAP query + cross-match → matched stars with distances.
- **Cheap, gain-dependent** (~1–3 s, re-run per preview): `compute_modulation(strength, gains)`
  + `render_stars` + screen-blend recombine.

So the UI is **two-phase**: **Prepare** once (slow, cached), then **Preview** fast on every gain
change, then **Execute** the full-res result.

## Architecture (Approach A — subprocess per action)

The dialog is PJSR (only way to get a window in PixInsight); the grade math stays in Python.
PJSR orchestrates and shells out to the Python core; the preview image flows Python → PNG → PJSR.

```
PJSR dialog ──Prepare──> PI: solve(view) + SXT split + writeWcsKeywords
                          └─> save stars_full.fits, starless_full.fits
                          └─> py prepare stars_full.fits <cache>   (detect+Gaia+match)
                                └─> cache: per-star x,y,flux,r_med/lo/hi + QA
                                └─> downscaled stars/starless preview layers
            ──Preview──> py preview <cache> out.png --gains.. --p-low/high.. --base-sigma.. [--region/--full]
                          └─> PJSR loads PNG into preview control(s)
            ──Execute──> py render stars_full.fits graded_full.fits --gains..
                          └─> PI PixelMath screen-blend(graded, starless) -> <id>_depthgraded window
```

Rejected alternatives: **B** persistent Python server (snappier previews, but adds IPC + process
lifecycle — overkill for button-driven previews); **C** reimplement render in PixelMath (duplicates
and would drift from the Python core; per-star Gaussian-halo/unsharp is painful in PixelMath).

## Python core changes (`src/gaia_depth_grade/`)

Factor `grade_array` (currently one shot in `cli.py`) into stages so the cache sits between the
gain-independent and gain-dependent work. New CLI subcommands:

- `prepare <stars.fits> <cache_dir>` — detect + Gaia + cross-match. Writes:
  - cache file (e.g. `prep.npz`): `x, y, flux, r_med_geo, r_lo_geo, r_hi_geo` per matched star,
    image shape, and the QA dict.
  - downscaled stars + starless preview layers (≈1/3) for the whole-frame preview.
  - QA JSON (match_rate, n_detected, n_matched, median_offset_px, low_match_warning).
- `preview <cache_dir> <out.png> --gains b,s,c,sat --p-low --p-high --base-sigma [--region x,y,w,h | --full]`
  — load cache, `effective_strength` (uses p_low/p_high) → `compute_modulation` (gains) →
  `render_stars` → screen-blend over starless → autostretch to 8-bit → write PNG.
  `--full` = whole frame on the downscaled layers; `--region` = 1:1 inset cropped from full-res.
  The screen-blend in `preview` MUST use the same formula as Execute's PixelMath,
  `result = 1 - (1-stars)*(1-starless)`, so the preview faithfully matches the final window
  (autostretch is the only display-only step).
- `render <stars.fits> <out.fits> --gains b,s,c,sat --p-low --p-high --base-sigma` — full-res graded
  stars FITS for Execute (the existing render path, parameterized).

`grade` (headless harness path) stays, reimplemented as `prepare`+`render` internally so there is
one code path. Autostretch: STF-style MTF (median/MAD, target bg ≈ 0.25), applied for display only.

## PJSR dialog (`pjsr/GaiaDepthGradeDialog.js`)

Reuses the harness building blocks (solver preamble, `splitStars`, `writeWcsKeywords`, `run`).
Layout, top-to-bottom:

- **Target row:** `ViewList` (open views, default active) + `Browse…` (open master from disk).
- **Preview area:** whole-frame preview (~640 px wide, autostretched). Clicking sets the inset
  center → 1:1 zoom inset (~256 px). A "preview stale" tint when params changed since last render.
- **Controls:**
  - *Live* (re-render only): brightness, size, contrast, saturation, base_sigma_px, p_low, p_high.
  - *Prepare-time* (edit ⇒ Prepare marked stale): detect_threshold_sigma, match_tolerance_px.
- **Buttons:** `Prepare` · `Preview/Update` (disabled until prepared) · `Execute` · `Close`.
- **Status line:** progress, QA summary, errors.

**Cache:** per-target temp dir `/tmp/gaia_depth_grade/ui/<viewid>/`, cleared on each new Prepare.

State machine: `Idle → (Prepare) → Prepared → (Preview)* → (Execute) → window`. Editing a
prepare-time field returns to `Idle` (preview/execute disabled until re-Prepared). Editing a live
control keeps `Prepared` but marks the preview stale.

## Error handling (loud; no silent fallbacks)

- Solve/SXT/Python nonzero exit → show captured stderr in the status line + message box; state
  unchanged. Preview/Execute remain disabled until a Prepare succeeds.
- Low match-rate → prominent warning, NOT a hard error (preview still allowed).
- Missing target / missing venv python / Gaia 500 → explicit message; never a fake/placeholder preview.

## Testing

- **Python:** unit tests for the stage split — cache round-trip; `preview` PNG dimensions and
  `--region` cropping; **`grade` output equals `prepare`+`render`** (no behavior drift). Reuse the
  synthetic-image fixtures; mock Gaia as existing tests do.
- **PJSR dialog:** not CI-testable (needs GPU + SXT, like the harness). Validated by a live run in
  the user's PixInsight session (see [[pixinsight-headless-recipe]] for the headless harness; the
  dialog is run interactively from Script ▸ Execute Script).

## Out of scope (YAGNI)

- Persistent preview server (Approach B).
- Full Process/Instance/Interface (drag-onto-view process icon).
- Auto-update-on-drag (debounced); previews are explicit via the Preview button. Can add later.
- SIP/spline distortion export for tighter matches (separate, deeper change).
