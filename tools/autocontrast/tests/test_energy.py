"""Tests for the angular energy spectrum (§4.2, §2.2)."""

import numpy as np
import pytest

from autocontrast.fingerprint.energy import energy_spectrum


def test_returns_center_and_energy_per_scale():
    image = np.zeros((64, 64))
    spec = energy_spectrum(image, pixel_scale_arcsec=1.0, n_scales=5)

    assert spec.centers_arcsec.shape == (5,)
    assert spec.energy.shape == (5,)


def test_centers_are_monotonically_increasing():
    spec = energy_spectrum(np.zeros((64, 64)), pixel_scale_arcsec=1.0, n_scales=5)
    assert np.all(np.diff(spec.centers_arcsec) > 0)


def test_centers_scale_linearly_with_pixel_scale():
    """The identical layer maps to twice the angular scale on a 2x-coarser
    instrument — the core of §2.2."""
    fine = energy_spectrum(np.zeros((64, 64)), pixel_scale_arcsec=0.5, n_scales=4)
    coarse = energy_spectrum(np.zeros((64, 64)), pixel_scale_arcsec=1.0, n_scales=4)

    np.testing.assert_allclose(coarse.centers_arcsec, 2 * fine.centers_arcsec)


def test_energy_is_nonnegative():
    rng = np.random.default_rng(1)
    spec = energy_spectrum(rng.random((64, 64)), pixel_scale_arcsec=1.0, n_scales=5)
    assert np.all(spec.energy >= 0)


def test_fine_detail_concentrates_energy_at_small_scales():
    rng = np.random.default_rng(2)
    noise = rng.random((64, 64))  # white noise: energy dominated by the finest scale
    spec = energy_spectrum(noise, pixel_scale_arcsec=1.0, n_scales=5)
    assert spec.energy[0] == spec.energy.max()


def test_rejects_non_2d_input():
    with pytest.raises(ValueError):
        energy_spectrum(np.zeros((8, 8, 3)), pixel_scale_arcsec=1.0, n_scales=3)
