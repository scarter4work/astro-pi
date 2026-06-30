"""Halo profiles for the star-shine effect.

When the depth grade enlarges a near star, render adds a synthetic halo so the
star looks like a genuine large star rather than a fast-collapsing Gaussian blob.
The *shape* of that halo is taken — wings and all — from the real bright stars in
the same layer (`build_halo_profile`). When a frame lacks enough clean reference
stars, we fall back to a Moffat profile (`moffat_profile`) and say so loudly.

A `HaloProfile` is just a normalized 1-D radial lookup: `sample(r)` returns the
halo amplitude at radius `r` px, 1.0 at the centre, falling to 0 at the edge.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass

import numpy as np

log = logging.getLogger(__name__)

# Reference stars must peak below this to be unsaturated; a clipped core would
# flatten the stacked profile and dump the saturation plateau into every halo.
_SATURATION_CEILING = 0.9


@dataclass(frozen=True)
class HaloProfile:
    """Normalized radial halo amplitude. `r_grid` ascending, `values[0] == 1.0`."""

    r_grid: np.ndarray
    values: np.ndarray
    is_empirical: bool

    def sample(self, r: np.ndarray) -> np.ndarray:
        r = np.asarray(r, dtype=float)
        # 0 beyond the last grid point so the halo never adds energy past where the
        # profile was actually measured (np.interp would otherwise clamp flat).
        out = np.interp(r, self.r_grid, self.values, left=self.values[0], right=0.0)
        out[r > self.r_grid[-1]] = 0.0
        return out


def moffat_profile(base_sigma_px: float, beta: float, radius_px: float) -> HaloProfile:
    """Parametric Moffat halo `(1 + (r/alpha)^2)^(-beta)`, normalized to 1 at r=0.

    `alpha` is chosen so the Moffat FWHM matches a Gaussian of `base_sigma_px`, so
    the core lines up with the rest of the render while the power-law wings give the
    natural extended glow a Gaussian lacks.
    """
    fwhm = 2.3548 * base_sigma_px
    alpha = fwhm / (2.0 * np.sqrt(2.0 ** (1.0 / beta) - 1.0))
    r_grid = np.linspace(0.0, radius_px, max(int(radius_px) + 1, 2))
    values = (1.0 + (r_grid / alpha) ** 2) ** (-beta)
    values = values / values[0]
    return HaloProfile(r_grid=r_grid, values=values, is_empirical=False)


def _radial_median(cut: np.ndarray, cx: float, cy: float, r_grid: np.ndarray) -> np.ndarray:
    """Median pixel value in each radial bin of `r_grid` (centre of bin i is r_grid[i])."""
    h, w = cut.shape
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2).ravel()
    flat = cut.ravel()
    step = r_grid[1] - r_grid[0]
    out = np.full(r_grid.shape, np.nan)
    for i, rc in enumerate(r_grid):
        m = np.abs(r - rc) <= step / 2.0
        if np.any(m):
            out[i] = np.median(flat[m])
    return out


def build_halo_profile(stars_layer, detected, base_sigma_px, cfg) -> HaloProfile:
    """Stack the bright, unsaturated, isolated stars in `stars_layer` into a halo
    profile. Falls back to a Moffat (logged loudly) when too few qualify.
    """
    lum = stars_layer.mean(axis=2) if stars_layer.ndim == 3 else np.asarray(stars_layer, float)
    radius_px = cfg.halo_radius_sigma * base_sigma_px
    r_grid = np.linspace(0.0, radius_px, int(radius_px) + 1)
    rad = int(np.ceil(radius_px))

    n = len(detected)
    flux = np.asarray(detected["flux"], float)
    xs = np.asarray(detected["x"], float)
    ys = np.asarray(detected["y"], float)
    # Brightest first — those have the cleanest, most extended wings.
    order = np.argsort(flux)[::-1]

    stack = []
    ny, nx = lum.shape
    for i in order:
        x, y = xs[i], ys[i]
        xi, yi = int(round(x)), int(round(y))
        if xi - rad < 0 or yi - rad < 0 or xi + rad >= nx or yi + rad >= ny:
            continue  # too close to the frame edge to cut a full stamp
        cut = lum[yi - rad : yi + rad + 1, xi - rad : xi + rad + 1]
        peak = cut.max()
        if not np.isfinite(peak) or peak <= 0 or peak >= _SATURATION_CEILING:
            continue  # dark or saturated -> unusable
        # isolation: no comparably bright neighbour away from the core
        cyc = cxc = rad
        yy, xx = np.mgrid[0 : cut.shape[0], 0 : cut.shape[1]]
        far = np.sqrt((xx - cxc) ** 2 + (yy - cyc) ** 2) > base_sigma_px * 4.0
        if far.any() and cut[far].max() > 0.5 * peak:
            continue  # a bright neighbour would contaminate the wings
        prof = _radial_median(cut, cxc, cyc, r_grid) / peak
        stack.append(prof)
        if len(stack) >= max(cfg.halo_min_refs * 3, cfg.halo_min_refs):
            break

    if len(stack) < cfg.halo_min_refs:
        log.warning(
            "star-shine: only %d clean reference star(s) (need %d) — using a "
            "Moffat halo this run, not the image's own PSF",
            len(stack), cfg.halo_min_refs)
        return moffat_profile(base_sigma_px, cfg.halo_moffat_beta, radius_px)

    values = np.nanmedian(np.vstack(stack), axis=0)
    # Fill any empty bins, normalize the centre to 1, enforce monotone non-increasing
    # (stacking noise can leave tiny upward wiggles that would read as faint rings).
    values = np.nan_to_num(values, nan=0.0)
    if values[0] <= 0:
        values[0] = 1.0
    values = values / values[0]
    values = np.minimum.accumulate(values)
    return HaloProfile(r_grid=r_grid, values=values, is_empirical=True)
