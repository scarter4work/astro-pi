"""Tests for the band-limiting rule (§4.4) — the most important function in the codebase.

The rule:
    lo = max(ref_psf_fwhm, target_psf_fwhm)
    hi = min(ref_max_meaningful_scale, target_max_meaningful_scale)
Compare only scales in [lo, hi]; re-normalize each spectrum over the survivors;
if fewer than 3 comparable bins survive, structural guidance is UNUSABLE and the
function must say so rather than silently proceed (§12).
"""

import numpy as np
import pytest

from autocontrast.fingerprint.bandlimit import band_limit, comparable_band
from autocontrast.fingerprint.energy import EnergySpectrum


def _spec(centers, energy=None):
    centers = np.asarray(centers, dtype=float)
    if energy is None:
        energy = np.ones_like(centers)
    return EnergySpectrum(centers_arcsec=centers, energy=np.asarray(energy, dtype=float))


def test_comparable_band_is_max_psf_to_min_second_largest_scale():
    ref = _spec([0.5, 1, 2, 4, 8, 16])       # second-largest center = 8
    target = _spec([1, 2, 4, 8, 16, 32, 64])  # second-largest center = 32
    lo, hi = comparable_band(ref, target, ref_psf_fwhm=1.0, target_psf_fwhm=3.0)
    assert lo == 3.0            # max(1.0, 3.0)
    assert hi == 8.0            # min(8, 32)


def test_usable_when_at_least_three_bins_overlap():
    centers = [1, 2, 4, 8, 16, 32, 64, 128]
    result = band_limit(_spec(centers), _spec(centers), ref_psf_fwhm=1.0, target_psf_fwhm=1.0)
    assert result.usable
    assert result.target_shape is not None
    assert result.ref_shape.shape == result.target_shape.shape
    assert result.grid_arcsec.size >= 3


def test_surviving_shapes_are_l2_normalized():
    centers = [1, 2, 4, 8, 16, 32, 64, 128]
    energy = [5, 4, 3, 2, 1, 0.5, 0.2, 0.1]
    result = band_limit(_spec(centers, energy), _spec(centers, energy),
                        ref_psf_fwhm=1.0, target_psf_fwhm=1.0)
    assert np.linalg.norm(result.ref_shape) == pytest.approx(1.0)
    assert np.linalg.norm(result.target_shape) == pytest.approx(1.0)


def test_identical_spectra_produce_identical_shapes():
    centers = [1, 2, 4, 8, 16, 32, 64, 128]
    energy = [9, 7, 5, 3, 2, 1, 0.5, 0.25]
    result = band_limit(_spec(centers, energy), _spec(centers, energy),
                        ref_psf_fwhm=1.0, target_psf_fwhm=1.0)
    np.testing.assert_allclose(result.ref_shape, result.target_shape)


def test_all_comparison_is_over_the_target_resolvable_scales_only():
    """Grid points must lie within [lo, hi]."""
    centers = [1, 2, 4, 8, 16, 32, 64, 128]
    result = band_limit(_spec(centers), _spec(centers),
                        ref_psf_fwhm=2.0, target_psf_fwhm=5.0)
    assert result.grid_arcsec.min() >= result.lo_arcsec - 1e-9
    assert result.grid_arcsec.max() <= result.hi_arcsec + 1e-9


def test_unusable_when_bands_do_not_overlap():
    centers = [1, 2, 4, 8, 16, 32, 64]
    result = band_limit(_spec(centers), _spec(centers),
                        ref_psf_fwhm=1.0, target_psf_fwhm=500.0)  # lo=500 > hi=32
    assert not result.usable
    assert result.ref_shape is None
    assert result.reason  # must explain why, not proceed silently


def test_unusable_when_fewer_than_three_bins_survive():
    centers = [1, 2, 4, 8, 16, 32]  # second-largest = 16
    # lo=10 => survivors among target centers in [10,16] = {16} only -> 1 bin
    result = band_limit(_spec(centers), _spec(centers),
                        ref_psf_fwhm=10.0, target_psf_fwhm=1.0)
    assert not result.usable
    assert result.reason
