import numpy as np
import pytest

from autocontrast.optimize.guardrails import (
    GuardrailLimits, check_highlight_clipping, check_noise_floor,
    check_shadow_clipping, clipped_fraction, mrs_noise_sigma,
)

RNG = np.random.default_rng(20260727)


def _smooth_image(h=128, w=128):
    y, x = np.mgrid[0:h, 0:w]
    base = 0.5 + 0.2 * np.sin(x / 20.0) * np.cos(y / 20.0)
    return np.clip(np.stack([base] * 3, axis=-1), 0.0, 1.0)


def test_mrs_noise_sigma_rises_with_injected_noise():
    clean = _smooth_image()[..., 0]
    noisy = np.clip(clean + RNG.normal(0, 0.05, clean.shape), 0, 1)
    assert mrs_noise_sigma(noisy) > mrs_noise_sigma(clean) * 3


def test_noise_floor_trips_when_noise_balloons():
    baseline = _smooth_image()
    noisy = np.clip(baseline + RNG.normal(0, 0.05, baseline.shape), 0, 1)
    limits = GuardrailLimits()

    assert check_noise_floor(baseline, baseline, limits).ok
    verdict = check_noise_floor(noisy, baseline, limits)
    assert not verdict.ok
    assert "noise" in verdict.reason.lower()


def test_noise_floor_fails_closed_on_degenerate_baseline():
    # A perfectly flat baseline has an all-zero finest starlet plane, so its
    # MAD-based sigma is exactly 0 -- measured directly below to confirm this
    # fixture genuinely exercises the base <= 0 branch, not just asserts on it.
    flat = np.full((128, 128, 3), 0.5)
    assert mrs_noise_sigma(flat[..., 0]) == 0.0

    noisy = np.clip(flat + RNG.normal(0, 0.05, flat.shape), 0, 1)
    limits = GuardrailLimits()

    verdict = check_noise_floor(noisy, flat, limits)
    assert not verdict.ok
    assert "baseline" in verdict.reason.lower()


def test_clipped_fraction_counts_per_channel():
    img = np.full((10, 10, 3), 0.5)
    img[0, :, 0] = 0.0          # 10 of 100 pixels in channel 0
    assert clipped_fraction(img, at=0.0)[0] == pytest.approx(0.10)
    assert clipped_fraction(img, at=0.0)[1] == pytest.approx(0.0)


def test_shadow_and_highlight_clipping_trip_independently():
    limits = GuardrailLimits()
    clean = _smooth_image()
    assert check_shadow_clipping(clean, limits).ok
    assert check_highlight_clipping(clean, limits).ok

    crushed = clean.copy()
    crushed[:20, :, :] = 0.0
    assert not check_shadow_clipping(crushed, limits).ok
    assert check_highlight_clipping(crushed, limits).ok

    blown = clean.copy()
    blown[:20, :, :] = 1.0
    assert not check_highlight_clipping(blown, limits).ok
    assert check_shadow_clipping(blown, limits).ok


def test_verdict_carries_value_and_limit_for_reporting():
    crushed = _smooth_image()
    crushed[:20, :, :] = 0.0
    v = check_shadow_clipping(crushed, GuardrailLimits())
    assert v.value > v.limit
    assert v.name == "shadow_clipping"
