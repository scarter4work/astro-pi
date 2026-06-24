import numpy as np
import pytest
from gaia_depth_grade.transform import depth_strength, confidence, effective_strength


def test_near_is_plus_one_far_is_minus_one():
    r = np.array([10.0, 100.0, 1000.0])
    s = depth_strength(r, p_low=0, p_high=100)
    assert s[0] == pytest.approx(1.0, abs=1e-6)   # nearest
    assert s[-1] == pytest.approx(-1.0, abs=1e-6)  # farthest
    assert s[0] > s[1] > s[2]                      # monotonic decreasing


def test_nan_maps_to_neutral():
    r = np.array([10.0, np.nan, 1000.0])
    s = depth_strength(r, p_low=0, p_high=100, neutral=0.0)
    assert s[1] == 0.0


def test_confidence_tight_vs_loose():
    r_med = np.array([100.0, 100.0])
    r_lo = np.array([98.0, 50.0])
    r_hi = np.array([102.0, 150.0])
    c = confidence(r_med, r_lo, r_hi)
    assert c[0] > c[1]                 # tight error -> higher confidence
    assert 0.0 <= c[1] <= c[0] <= 1.0


def test_effective_strength_attenuates_noisy():
    # distinct distances -> a real depth range (no degenerate hi<=lo case)
    r_med = np.array([10.0, 1000.0])
    r_lo = np.array([9.8, 500.0])
    r_hi = np.array([10.2, 2000.0])
    e = effective_strength(r_med, r_lo, r_hi, p_low=0, p_high=100, neutral=0.0)
    # nearer + tighter error -> larger magnitude strength
    assert abs(e[0]) > abs(e[1])


def test_degenerate_equal_distances_are_neutral():
    # all matched stars at one distance -> no depth spread -> neutral, not a boost
    r_med = np.array([10.0, 10.0])
    s = depth_strength(r_med, p_low=0, p_high=100, neutral=0.0)
    assert np.allclose(s, 0.0)
