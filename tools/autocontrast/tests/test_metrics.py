"""Tests for the tonal / chroma / saturation / background metrics (§4.2)."""

import numpy as np
import pytest

from autocontrast.fingerprint.metrics import (
    QUANTILE_LEVELS,
    background_channel_balance,
    chroma_histogram,
    floor_to_peak_ratio,
    saturation_quantiles,
    structure_to_gradient_ratio,
    tonal_quantiles,
)

# ---- tonal ----------------------------------------------------------------


def test_tonal_quantiles_length_and_monotonic():
    rng = np.random.default_rng(0)
    L = rng.uniform(0, 100, size=(64, 64))
    q = tonal_quantiles(L)
    assert q.shape == (len(QUANTILE_LEVELS),)
    assert np.all(np.diff(q) >= 0)


def test_tonal_median_is_middle_quantile():
    L = np.linspace(0, 100, 10000).reshape(100, 100)
    q = tonal_quantiles(L)
    median_index = QUANTILE_LEVELS.index(50)
    assert q[median_index] == pytest.approx(np.median(L), abs=1e-6)


# ---- saturation -----------------------------------------------------------


def test_saturation_quantiles_nonnegative_and_monotonic():
    rng = np.random.default_rng(1)
    chroma = rng.uniform(0, 50, size=(64, 64))
    q = saturation_quantiles(chroma)
    assert np.all(q >= 0)
    assert np.all(np.diff(q) >= 0)


def test_neutral_image_has_zero_saturation():
    chroma = np.zeros((32, 32))
    assert np.all(saturation_quantiles(chroma) == 0)


# ---- chroma histogram -----------------------------------------------------


def test_chroma_histogram_shape_and_normalization():
    rng = np.random.default_rng(2)
    a = rng.uniform(-60, 60, size=(64, 64))
    b = rng.uniform(-60, 60, size=(64, 64))
    hist = chroma_histogram(a, b, bins=16, extent=100.0)
    assert hist.shape == (16, 16)
    assert hist.sum() == pytest.approx(1.0)


def test_neutral_image_has_empty_chroma_histogram():
    a = np.zeros((32, 32))
    b = np.zeros((32, 32))
    hist = chroma_histogram(a, b, bins=16, extent=100.0)
    assert hist.sum() == pytest.approx(0.0)


def test_single_color_concentrates_chroma_mass():
    a = np.full((32, 32), 40.0)
    b = np.full((32, 32), -20.0)
    hist = chroma_histogram(a, b, bins=16, extent=100.0)
    # Essentially all the mass lands in one bin.
    assert hist.max() == pytest.approx(1.0)
    assert np.count_nonzero(hist) == 1


# ---- background: floor/peak ----------------------------------------------


def test_floor_to_peak_ratio_of_flat_image_is_one():
    L = np.full((32, 32), 40.0)
    assert floor_to_peak_ratio(L) == pytest.approx(1.0)


def test_floor_to_peak_ratio_between_zero_and_one():
    rng = np.random.default_rng(3)
    L = rng.uniform(1, 100, size=(128, 128))
    r = floor_to_peak_ratio(L)
    assert 0.0 < r < 1.0


# ---- background: structure vs gradient -----------------------------------


def test_gradient_dominated_image_scores_lower_than_textured():
    y = np.linspace(0, 1, 64)
    ramp = np.repeat(y[:, None], 64, axis=1)  # pure gradient, no structure
    rng = np.random.default_rng(4)
    textured = rng.random((64, 64))           # fine structure everywhere

    assert structure_to_gradient_ratio(ramp, n_scales=4) < structure_to_gradient_ratio(
        textured, n_scales=4
    )


# ---- background: channel balance -----------------------------------------


def test_neutral_background_is_balanced():
    rng = np.random.default_rng(5)
    gray = rng.uniform(0.0, 0.1, size=(64, 64))
    rgb = np.stack([gray, gray, gray], axis=-1)
    balance = background_channel_balance(rgb)
    np.testing.assert_allclose(balance, [1.0, 1.0, 1.0], atol=1e-6)


def test_red_cast_background_shows_elevated_red():
    rng = np.random.default_rng(6)
    base = rng.uniform(0.0, 0.05, size=(64, 64))
    rgb = np.stack([base + 0.05, base, base], axis=-1)  # red-biased faint background
    balance = background_channel_balance(rgb)
    assert balance[0] > balance[1]
    assert balance[0] > balance[2]
