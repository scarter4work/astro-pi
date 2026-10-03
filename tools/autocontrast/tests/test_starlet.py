"""Tests for the à trous (starlet) multiscale transform — the energy-spectrum engine (§4.2)."""

import numpy as np
import pytest

from autocontrast.fingerprint.starlet import starlet_transform


def test_reconstruction_is_exact():
    """Original == sum(wavelet planes) + smooth residual, to float precision.

    This is the defining invariant of the à trous transform; if it fails the
    energy spectrum is meaningless.
    """
    rng = np.random.default_rng(0)
    image = rng.random((64, 64)).astype(np.float64)

    planes, residual = starlet_transform(image, n_scales=4)

    reconstructed = np.sum(planes, axis=0) + residual
    np.testing.assert_allclose(reconstructed, image, atol=1e-10)


def test_returns_one_plane_per_scale_plus_residual_same_shape():
    image = np.zeros((32, 48), dtype=np.float64)

    planes, residual = starlet_transform(image, n_scales=3)

    assert planes.shape == (3, 32, 48)
    assert residual.shape == (32, 48)


def test_flat_image_has_zero_detail():
    """A constant field has no structure at any scale: every wavelet plane is ~0
    and the residual carries the constant."""
    image = np.full((32, 32), 0.42, dtype=np.float64)

    planes, residual = starlet_transform(image, n_scales=4)

    np.testing.assert_allclose(planes, 0.0, atol=1e-12)
    np.testing.assert_allclose(residual, 0.42, atol=1e-12)


def test_fine_detail_lands_in_first_scale():
    """A single-pixel spike is the finest possible structure; scale 1 must carry
    more of its energy than the coarsest scale."""
    image = np.zeros((32, 32), dtype=np.float64)
    image[16, 16] = 1.0

    planes, _ = starlet_transform(image, n_scales=4)

    energy_per_scale = np.sum(planes**2, axis=(1, 2))
    assert energy_per_scale[0] > energy_per_scale[-1]


def test_rejects_non_2d_input():
    with pytest.raises(ValueError):
        starlet_transform(np.zeros((8, 8, 3)), n_scales=2)
