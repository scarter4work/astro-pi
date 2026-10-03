"""The band-limiting rule (design §4.4).

> This is the single most important function in the codebase. Get it right.

Two instruments resolve different ranges of angular scale. Comparing a spectrum
bin the target physically cannot resolve against one the reference can drives the
optimizer to sharpen noise into artifacts (§2.2). So before comparing spectra we
restrict to the overlap of the two resolvable bands, resample onto the target's
scales inside that band, and compare *normalized shape only* — never absolute
energy, because the reference always carries more total structure and that is not
a deficiency to correct.

When the overlap is too thin (< ``min_bins`` comparable scales) structural
guidance is unusable. This module says so explicitly; it never proceeds silently
(§12).
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .energy import EnergySpectrum


def _max_meaningful_scale(spec: EnergySpectrum) -> float:
    """The largest scale that still represents structure, not the residual/gradient
    plane — practically, the second-largest wavelet layer (§4.4)."""
    return float(spec.centers_arcsec[-2])


def comparable_band(
    ref: EnergySpectrum,
    target: EnergySpectrum,
    ref_psf_fwhm: float,
    target_psf_fwhm: float,
) -> tuple[float, float]:
    """The [lo, hi] arcsec window over which the two spectra may be compared."""
    lo = max(ref_psf_fwhm, target_psf_fwhm)
    hi = min(_max_meaningful_scale(ref), _max_meaningful_scale(target))
    return lo, hi


@dataclass
class BandLimitResult:
    """Outcome of band-limiting two spectra.

    On success (``usable``), ``ref_shape`` and ``target_shape`` are unit-L2
    normalized energy vectors sampled on the shared ``grid_arcsec`` and are ready
    for shape comparison. On failure they are ``None`` and ``reason`` explains why
    — the caller must fall back to tonal/chroma guidance and surface the warning.
    """

    lo_arcsec: float
    hi_arcsec: float
    usable: bool
    reason: str
    grid_arcsec: np.ndarray | None = None
    ref_shape: np.ndarray | None = None
    target_shape: np.ndarray | None = None


def _l2_normalize(v: np.ndarray) -> np.ndarray:
    norm = np.linalg.norm(v)
    return v / norm if norm > 0 else v


def band_limit(
    ref: EnergySpectrum,
    target: EnergySpectrum,
    ref_psf_fwhm: float,
    target_psf_fwhm: float,
    min_bins: int = 3,
) -> BandLimitResult:
    """Restrict two spectra to their comparable band and return normalized shapes.

    The target's native scales inside [lo, hi] are the comparison grid — they are
    the scales the image being optimized can actually resolve. The reference is
    interpolated (in log-arcsec) onto that grid. Both are then L2-normalized so
    only *shape* is compared.
    """
    lo, hi = comparable_band(ref, target, ref_psf_fwhm, target_psf_fwhm)

    if hi <= lo:
        return BandLimitResult(
            lo_arcsec=lo, hi_arcsec=hi, usable=False,
            reason=f"No band overlap: lo={lo:.3g}\" >= hi={hi:.3g}\". "
                   "Instruments share no resolvable scale; structural guidance unusable.",
        )

    grid_mask = (target.centers_arcsec >= lo) & (target.centers_arcsec <= hi)
    grid = target.centers_arcsec[grid_mask]

    if grid.size < min_bins:
        return BandLimitResult(
            lo_arcsec=lo, hi_arcsec=hi, usable=False,
            reason=f"Only {grid.size} comparable bin(s) in [{lo:.3g}, {hi:.3g}]\" "
                   f"(need {min_bins}); structural guidance unusable, fall back to tonal/chroma.",
        )

    target_energy = target.energy[grid_mask]
    # Interpolate the reference onto the target grid in log-scale space.
    ref_energy = np.interp(np.log(grid), np.log(ref.centers_arcsec), ref.energy)

    return BandLimitResult(
        lo_arcsec=lo, hi_arcsec=hi, usable=True,
        reason=f"{grid.size} comparable bins in [{lo:.3g}, {hi:.3g}]\".",
        grid_arcsec=grid,
        ref_shape=_l2_normalize(ref_energy),
        target_shape=_l2_normalize(target_energy),
    )
