# Star-shine halos for enlarged stars

**Date:** 2026-06-29
**Status:** approved (design), implementing

## Problem

When the depth grade enlarges a near star (`zsize > 1`), `render_stars` adds a
peak-normalized **Gaussian** halo (`render.py:73-80`). Three things make the
result read as fake:

1. A Gaussian (`exp(-r^2)`) has no wings — it collapses fast, producing a tight
   *blob* instead of the extended *shine* real bright stars have.
2. The halo is **achromatic** (`stamp[..., None]` adds the same value to R, G, B),
   so a grey glow sits on a colored star and looks pasted on.
3. Amplitude is scaled by DAO `flux`, not the star's actual peak pixel, so the
   bump can sit at the wrong level relative to the real core underneath it.

Goal: enlarged stars should wear a halo whose **shape comes from the real bright
stars in the same layer** and whose **color matches the star's own**, so they look
like genuine large stars.

## Decisions (from brainstorming)

- **Halo shape:** empirical — stacked from the brightest unsaturated stars in *this*
  layer, so it literally is the image's own optics/seeing glow.
- **Color:** tint each halo to that star's own measured core color ratio.
- **Fallback:** when too few clean reference stars exist, fall back to a Moffat
  parametric profile and emit a **loud WARNING** (never silent).
- **Replace, don't stack:** the empirical halo both widens and glows, so it
  replaces the old Gaussian stamp in the enlarge branch.
- **Amplitude anchored to the real peak pixel**, not DAO flux.
- **Intensity knob:** new `halo_gain` (default 1.0) — dial shine independently of
  footprint size.
- Far stars (`zsize < 1`) are untouched — they still recede via dimming only,
  preserving the dark-ring fix.

## New module: `psf.py`

Single responsibility: turn a star layer + detected table into a halo profile.

```
build_halo_profile(stars_layer, detected, base_sigma_px, cfg) -> HaloProfile
```

- **Reference selection:** bright but unsaturated (peak pixel `< 0.9`), isolated
  (no comparably bright neighbor inside the stamp), finite measured `fwhm`. Require
  `>= cfg.halo_min_refs` (default 8).
- **Stack:** extract each reference's luminance cutout, recenter on its centroid,
  normalize to peak 1.0, median-combine into a robust 1-D radial profile `f(r)`
  with `f(0)=1`, sampled out to `cfg.halo_radius_sigma * base_sigma_px`.
- **Fallback:** too few refs -> `log.warning(...)`, return Moffat
  `f(r) = (1 + (r/alpha)^2)^(-beta)` with `beta = cfg.halo_moffat_beta` (~2.5),
  `alpha` derived from `base_sigma_px`.

`HaloProfile`:
- `sample(r) -> amplitude` — interpolated radial lookup (0 beyond max radius).
- `is_empirical: bool` — for QA logging.

## Change in `render.py` (the `zsize > 1` branch)

Build the `HaloProfile` once at the top of `render_stars` (gain-independent for the
layer). For each enlarged star, replace lines 73-80 with:

1. Sample the shared profile at the window radii, scaled so it stretches with
   `zsize`.
2. **Color tint:** mean R:G:B over the star's central few px, normalized to a unit
   ratio; multiply the halo per-channel. (Grey input stays grey.)
3. **Amplitude:** `halo_gain * (zsize - 1) * peak_core`, where `peak_core` is the
   star's actual central pixel value.
4. Keep the existing radial cosine taper so it blends to 0 at the window edge.

## Config (`config.py`)

Add to `GradeConfig`: `halo_min_refs: int = 8`, `halo_radius_sigma: float = 8.0`,
`halo_moffat_beta: float = 2.5`. Add `halo_gain: float = 1.0` to `Gains` (so it
lives with the other render gains and is TOML-tunable under `[gains]`).

## Tests (`tests/`)

- Synthetic frame of known-Moffat bright stars -> `build_halo_profile` returns a
  profile with wings heavier than a Gaussian; `is_empirical == True`.
- Sparse frame (too few refs) -> Moffat returned, warning logged.
- Enlarged colored star -> halo carries the star's hue (per-channel ratio
  preserved); achromatic input stays grey.
- Edge stars / empty windows don't crash (existing guards hold).
- Mono (2-D) layer path still works.

## Release

Ships in the sidecar (Python) gated behind the signed scripts. Per project rules:
bump sidecar version + scripts version, run `./release.sh`, integrity-check every
declared sha1, commit version bump + `repository/` artifacts together. Do not push
without confirmation.
