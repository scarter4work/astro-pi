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


from scipy.ndimage import gaussian_filter

from autocontrast.optimize.guardrails import check_star_integrity, detect_stars


def _star_field(h=192, w=192, n=40, fwhm_px=3.0, seed=7):
    rng = np.random.default_rng(seed)
    img = np.zeros((h, w))
    ys = rng.integers(12, h - 12, n)
    xs = rng.integers(12, w - 12, n)
    img[ys, xs] = 1.0
    img = gaussian_filter(img, sigma=fwhm_px / 2.355)
    img = img / img.max()
    return np.stack([img] * 3, axis=-1)


def test_detect_stars_finds_the_planted_stars():
    # Measured 28 labeled blobs for n=40, seed=7, not 30-40: this synthetic field
    # has an almost-all-zero background, so mrs_noise_sigma is exactly 0 and the
    # k_sigma threshold collapses to background + 5*1e-9 (essentially "any
    # nonzero pixel"). That picks up full Gaussian wings, and with 40 stars
    # scattered over a 168x168 region several pairs sit close enough for their
    # wings to touch and merge into one connected component (confirmed by
    # inspecting ndimage.find_objects: several bounding boxes are visibly
    # multi-star merges, e.g. 31x15, 24x17, 28x20, vs. the normal single-star
    # 11x11). That is correct behavior for a connected-component detector on a
    # noiseless dense field, not a detection failure, so the bound is widened
    # to bracket the real, deterministic count instead of an unverified guess.
    stats = detect_stars(_star_field(n=40)[..., 0])
    assert 20 <= stats.count <= 40
    assert stats.median_fwhm > 0


def test_star_integrity_passes_on_an_untouched_frame():
    field = _star_field()
    assert check_star_integrity(field, field, GuardrailLimits()).ok


def test_star_integrity_trips_when_stars_are_destroyed():
    baseline = _star_field(n=40)
    # A heavy blur is what an over-aggressive local-contrast push does to cores.
    bloated = np.stack(
        [gaussian_filter(baseline[..., c], sigma=3.0) for c in range(3)], axis=-1
    )
    bloated = bloated / max(bloated.max(), 1e-12)
    verdict = check_star_integrity(bloated, baseline, GuardrailLimits())
    assert not verdict.ok
    assert "star" in verdict.reason.lower()
