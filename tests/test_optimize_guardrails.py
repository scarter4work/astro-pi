import numpy as np
import pytest

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


# --- hue-angle acceptance fixtures (design SS5.1) ---
#
# rgb_to_lab has no inverse in the production code (nothing needed one before
# this). Isolating "saturation boost" from "genuine hue shift" requires one:
# these boost the fixture directly in a*/b* -- an EXACT, angle-preserving
# radial scale -- then round-trip back through sRGB, where a strong enough
# boost pushes a channel out of gamut and forces a clip that genuinely moves
# the hue. _lab_to_rgb is the algebraic inverse of rgb_to_lab (same D65
# matrices, same gamma), verified below by round-trip.
#
# _lab_to_linear is exposed separately from the final sRGB conversion because
# the gamma step floors negative LINEAR light to 0 before encoding (raising a
# negative number to a fractional power is undefined) -- so the final sRGB
# array can never report a negative minimum even when the pre-floor value
# genuinely went out of gamut. Checking "did this clip" against the final sRGB
# array is a silent false negative; it has to be checked in linear space.

from autocontrast.fingerprint.color import _DELTA, _RGB_TO_XYZ, _WHITE, rgb_to_lab

_XYZ_TO_RGB = np.linalg.inv(_RGB_TO_XYZ)


def _lab_finv(f):
    return np.where(f > _DELTA, f**3, 3 * _DELTA**2 * (f - 4.0 / 29.0))


def _lab_to_linear(lab):
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    fy = (L + 16.0) / 116.0
    fx, fz = fy + a / 500.0, fy - b / 200.0
    xyz = np.stack([_lab_finv(fx), _lab_finv(fy), _lab_finv(fz)], axis=-1) * _WHITE
    return xyz @ _XYZ_TO_RGB.T


def _linear_to_srgb(linear):
    linear = np.clip(linear, 0, None)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)


def _lab_to_rgb(lab):
    return _linear_to_srgb(_lab_to_linear(lab))


def test_lab_to_rgb_round_trips_rgb_to_lab():
    # Not exercising the guardrail -- confirming the inverse used to build the
    # fixtures below is actually correct, so a wrong inverse can't silently
    # fabricate the "boost preserves hue exactly" premise those tests rely on.
    rng = np.random.default_rng(20260727)
    rgb = rng.uniform(0.05, 0.95, (8, 8, 3))
    assert np.abs(_lab_to_rgb(rgb_to_lab(rgb)) - rgb).max() < 1e-9


def _boost_lab_chroma(rgb, boost):
    """Scale a*/b* by ``boost`` about the same L* -- pure radial motion, EXACT
    hue-angle preservation by construction. Returns (true_linear, candidate_rgb):
    true_linear is the UNFLOORED linear-light result, needed to tell whether this
    boost genuinely left the sRGB gamut; candidate_rgb is the clipped [0, 1]
    image a real pipeline would actually produce."""
    lab = rgb_to_lab(rgb).copy()
    lab[..., 1] *= boost
    lab[..., 2] *= boost
    linear = _lab_to_linear(lab)
    return linear, np.clip(_linear_to_srgb(linear), 0.0, 1.0)


def _hue_deg(rgb):
    lab = rgb_to_lab(rgb)
    a, b = lab[..., 1], lab[..., 2]
    return np.degrees(np.arctan2(b, a)) % 360.0, np.hypot(a, b)


def _blue_deficient_source(h=64, w=64):
    # A real, existing color bias (warm/orange cast from a blue deficiency),
    # uniform so the hue-vs-clip measurement below is exact and unambiguous,
    # not averaged over a spatially varying fixture. 0.64 is not arbitrary: it
    # is the largest deficiency (in 0.02 steps) for which boost=2.2 is still
    # measurably inside the sRGB gamut -- see task-4-report.md for the sweep.
    rgb = np.full((h, w, 3), 0.5)
    rgb[..., 2] *= 0.64
    return rgb


@pytest.mark.parametrize("boost", [1.3, 1.8, 2.2])
def test_hue_invention_passes_on_legitimate_saturation_boosts(boost):
    source = _blue_deficient_source()
    linear, candidate = _boost_lab_chroma(source, boost)
    # Confirm this boost genuinely stays in gamut -- the premise this fixture
    # relies on -- rather than asserting on a candidate quietly clipped anyway.
    assert linear.min() >= 0.0 and linear.max() <= 1.0

    hue_s, _ = _hue_deg(source)
    hue_c, _ = _hue_deg(candidate)
    assert np.abs(hue_c - hue_s).max() == pytest.approx(0.0, abs=1e-9)

    verdict = check_hue_invention(candidate, source, GuardrailLimits())
    assert verdict.ok, verdict.reason


def test_hue_invention_trips_on_a_boost_that_clips_and_shifts_hue():
    source = _blue_deficient_source()
    linear, candidate = _boost_lab_chroma(source, 3.0)
    # Confirm this boost genuinely clips (the blue channel is driven negative
    # in true linear light, before the gamma floor hides it) -- the premise
    # that this is a real hue shift, not an arbitrary threshold pick.
    assert linear.min() < 0.0

    hue_s, _ = _hue_deg(source)
    hue_c, _ = _hue_deg(candidate)
    delta = float(np.abs(hue_c - hue_s).max())
    assert delta > 3.0  # measured 3.58 degrees -- see task-4-report.md

    verdict = check_hue_invention(candidate, source, GuardrailLimits())
    assert not verdict.ok
    assert "hue" in verdict.reason.lower()


def test_hue_invention_trips_on_color_with_no_source_support():
    source = np.stack([np.full((64, 64), 0.5)] * 3, axis=-1)  # perfectly neutral
    invented = source.copy()
    invented[..., 1] = 0.9                                     # a green cast from nowhere
    verdict = check_hue_invention(invented, source, GuardrailLimits())
    assert not verdict.ok
    assert "hue" in verdict.reason.lower()


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
