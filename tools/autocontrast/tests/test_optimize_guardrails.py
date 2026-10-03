import numpy as np
import pytest

from autocontrast.fingerprint.color import rgb_to_lab
from autocontrast.optimize.guardrails import (
    GuardrailLimits, check_channel_ratio_drift, check_highlight_clipping,
    check_hue_invention, check_noise_floor, check_shadow_clipping,
    clipped_fraction, evaluate_guardrails, mrs_noise_sigma,
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


def test_star_integrity_passes_but_says_so_when_baseline_is_starless():
    # A genuinely starless baseline (e.g. a nebula-only crop) -- not a fixture
    # bug: measured directly below that detect_stars really returns count==0
    # for a flat frame, so this exercises the base.count == 0 branch and not
    # some other path.
    blank = np.zeros((192, 192, 3))
    assert detect_stars(blank[..., 0]).count == 0

    verdict = check_star_integrity(blank, blank, GuardrailLimits())
    assert verdict.ok
    # Must not be a silent pass: the reason has to say integrity wasn't
    # actually evaluated, not merely that it checked out clean.
    assert verdict.reason != ""
    assert "no" in verdict.reason.lower() and "star" in verdict.reason.lower()


# --- hue-angle acceptance fixtures (design SS5.1, twice revised) ---
#
# Saturation is applied in RGB, as any real ColorSaturation implementation
# would, NOT by scaling Lab a*/b* directly. CIELAB hue angle is a nonlinear
# function of RGB, so a legitimate boost drifts the angle by a few degrees
# even with zero clipping -- an angle-exact Lab-radial boost hides that drift
# and is not a faithful fixture (an earlier round of this guardrail used one
# and got a false "clean separation"; see task-4-report.md fix round 2 for
# how that went wrong). Gamut clipping is deliberately NOT tested here -- that
# is check_shadow_clipping's and check_highlight_clipping's job, not this
# guardrail's (SS5.1's second correction).

def _hue_deg(rgb):
    lab = rgb_to_lab(rgb)
    a, b = lab[..., 1], lab[..., 2]
    return np.degrees(np.arctan2(b, a)) % 360.0, np.hypot(a, b)


def _circular_distance_deg(h1, h2):
    diff = np.abs(h1 - h2) % 360.0
    return np.minimum(diff, 360.0 - diff)


def _saturation_boost(rgb, boost):
    """gray + (rgb - gray) * boost -- a real, RGB-space saturation stretch, not
    an angle-exact Lab operation. Not clipped: callers confirm gamut themselves,
    since the acceptance set requires some cases to be genuinely unclipped."""
    gray = rgb.mean(axis=-1, keepdims=True)
    return gray + (rgb - gray) * boost


def _red_biased_source(h=128, w=128):
    # A real, existing color bias (red cast), spatially varying via
    # _smooth_image -- NOT a single flat hue, which hid the RGB-space drift
    # in an earlier round of this guardrail (see task-4-report.md).
    source = _smooth_image(h, w)
    source[..., 0] *= 1.18
    return np.clip(source, 0.0, 1.0)


@pytest.mark.parametrize("boost", [1.3, 1.8, 2.2, 3.0])
def test_hue_invention_passes_on_legitimate_saturation_boosts(boost):
    source = _red_biased_source()
    candidate = _saturation_boost(source, boost)
    # The acceptance set requires "3.0x boost, UNCLIPPED -> PASS" specifically
    # -- confirm this boost genuinely stays in gamut, not that a clip silently
    # rescued an out-of-range value into looking legitimate.
    assert candidate.min() >= 0.0 and candidate.max() <= 1.0

    hue_s, chroma_s = _hue_deg(source)
    hue_c, _ = _hue_deg(candidate)
    mask = chroma_s > 1.0
    displacement = float(_circular_distance_deg(hue_c, hue_s)[mask].max())
    assert displacement < 20.0  # measured values recorded in task-4-report.md

    verdict = check_hue_invention(candidate, source, GuardrailLimits())
    assert verdict.ok, (verdict.reason, displacement)


def test_hue_invention_trips_on_green_injected_into_a_red_image():
    source = _red_biased_source()
    invented = source.copy()
    invented[..., 1] = np.clip(invented[..., 1] * 1.3, 0, 1)  # a green cast layered on top

    hue_s, chroma_s = _hue_deg(source)
    hue_i, chroma_i = _hue_deg(invented)
    mask_s, mask_i = chroma_s > 1.0, chroma_i > 1.0
    # Confirm this genuinely lands far from anything the source supports --
    # not a marginal case riding the tolerance boundary (measured ~104 degrees,
    # see task-4-report.md).
    displacement = float(_circular_distance_deg(hue_i[mask_i].mean(), hue_s[mask_s].mean()))
    assert displacement > 90.0

    verdict = check_hue_invention(invented, source, GuardrailLimits())
    assert not verdict.ok
    assert "hue" in verdict.reason.lower()


def test_hue_invention_trips_on_color_with_no_source_support():
    source = np.stack([np.full((64, 64), 0.5)] * 3, axis=-1)  # perfectly neutral
    invented = source.copy()
    invented[..., 1] = 0.9                                     # a green cast from nowhere
    verdict = check_hue_invention(invented, source, GuardrailLimits())
    assert not verdict.ok
    assert "hue" in verdict.reason.lower()


def test_hue_invention_handles_wraparound_at_zero_degrees():
    # Two nearly-identical colors whose hues sit on opposite sides of the
    # 0/360 seam: a naive |h1 - h2| reads ~352 degrees apart (looking like
    # fabrication); the TRUE circular distance is ~7.9 degrees (comfortably
    # legitimate). A wraparound bug in the guardrail would fail this test by
    # tripping on a candidate that is actually well within tolerance.
    source = np.full((64, 64, 3), (0.40, 0.31, 0.33))     # hue ~3.76 degrees
    candidate = np.full((64, 64, 3), (0.39, 0.30, 0.33))  # hue ~355.90 degrees

    hue_s, _ = _hue_deg(source)
    hue_c, _ = _hue_deg(candidate)
    naive = float(np.abs(hue_c[0, 0] - hue_s[0, 0]))
    circular = float(_circular_distance_deg(hue_c[0, 0], hue_s[0, 0]))
    assert naive > 340.0     # confirms this fixture genuinely straddles the seam
    assert circular < 10.0   # the TRUE distance is small -- within tolerance

    verdict = check_hue_invention(candidate, source, GuardrailLimits())
    assert verdict.ok, verdict.reason


def test_channel_ratio_drift_trips_when_ratios_move():
    source = _smooth_image()
    drifted = source.copy()
    drifted[..., 2] *= 1.5                       # push blue hard
    drifted = np.clip(drifted, 0, 1)
    assert check_channel_ratio_drift(source, source, GuardrailLimits()).ok
    assert not check_channel_ratio_drift(drifted, source, GuardrailLimits()).ok


def test_evaluate_guardrails_returns_every_verdict_not_just_the_first():
    source = _smooth_image()
    bad = source.copy()
    bad[:30, :, :] = 0.0
    bad[30:60, :, :] = 1.0
    verdicts = evaluate_guardrails(bad, source, source, GuardrailLimits())
    names = {v.name for v in verdicts}
    assert {"noise_floor", "star_integrity", "shadow_clipping",
            "highlight_clipping", "hue_invention", "channel_ratio_drift"} <= names
    failed = {v.name for v in verdicts if not v.ok}
    assert "shadow_clipping" in failed and "highlight_clipping" in failed
