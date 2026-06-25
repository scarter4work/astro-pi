import numpy as np
import pytest
from gaia_depth_grade.config import Gains
from gaia_depth_grade.modulate import compute_modulation, Modulation


def test_near_brightens_far_dims():
    s = np.array([1.0, -1.0])
    m = compute_modulation(s, Gains(brightness=0.5, size=0.4, contrast=0.3, saturation=0.3))
    assert isinstance(m, Modulation)
    assert m.brightness[0] == pytest.approx(1.5)
    assert m.brightness[1] == pytest.approx(0.5)
    assert m.size[0] > 1.0 and m.size[1] < 1.0


def test_zero_gain_is_identity():
    s = np.array([1.0, -0.7, 0.3])
    m = compute_modulation(s, Gains(brightness=0.0, size=0.0, contrast=0.0, saturation=0.0))
    assert np.allclose(m.brightness, 1.0)
    assert np.allclose(m.size, 1.0)
    assert np.allclose(m.saturation, 1.0)
    assert np.allclose(m.contrast, 0.0)
