"""Tests for sRGB -> CIELAB conversion, pinned against published reference values."""

import numpy as np
import pytest

from autocontrast.fingerprint.color import rgb_to_lab


def _lab_of(r, g, b):
    img = np.array([[[r, g, b]]], dtype=np.float64)  # 1x1x3
    lab = rgb_to_lab(img)
    return lab[0, 0]  # (L, a, b)


def test_white_maps_to_L100_neutral():
    L, a, b = _lab_of(1.0, 1.0, 1.0)
    assert L == pytest.approx(100.0, abs=1e-3)
    assert a == pytest.approx(0.0, abs=1e-3)
    assert b == pytest.approx(0.0, abs=1e-3)


def test_black_maps_to_origin():
    L, a, b = _lab_of(0.0, 0.0, 0.0)
    assert L == pytest.approx(0.0, abs=1e-6)
    assert a == pytest.approx(0.0, abs=1e-6)
    assert b == pytest.approx(0.0, abs=1e-6)


def test_pure_red_matches_reference():
    L, a, b = _lab_of(1.0, 0.0, 0.0)
    assert L == pytest.approx(53.24, abs=0.05)
    assert a == pytest.approx(80.09, abs=0.05)
    assert b == pytest.approx(67.20, abs=0.05)


def test_output_shape_matches_spatial_dims():
    img = np.zeros((7, 5, 3))
    lab = rgb_to_lab(img)
    assert lab.shape == (7, 5, 3)


def test_rejects_non_rgb_input():
    with pytest.raises(ValueError):
        rgb_to_lab(np.zeros((4, 4)))  # missing channel axis
