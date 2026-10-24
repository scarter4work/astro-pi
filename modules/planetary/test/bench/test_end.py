"""Validation of END, the benchmark's judged sharpness-at-equal-noise measure (Ruling P28), and of
noise.py, the per-frame noise it is built on.

Every image is judged against ONE noise model: white noise of power sigma_frame^2 / k, the
capture's per-frame noise over the image's effective frame count (metric.py, END). The tests build
stacks whose truth is known -- the scene, each stack's blur and its noise -- and check that END
ranks them by what a matched restoration of each actually recovers, that the photon budget
charges a tool's smoothing as blur, and (pinned as a known limit, strict xfail) that END cannot
see noise or sharpening the budget does not hold (Ruling P28 fix round 1, C1).

Run with -s to see the measured numbers (recorded in README.md).
"""
import math
import os

import numpy as np
import pytest
from scipy import ndimage

import benchset
import metric
import noise
import refs
from conftest import add_noise, banded_planet, planet_scene, unsharp

SIGMA = 0.004            # per-stack white noise of the base case, scene units (scene max ~ 0.8)
C1 = ("KNOWN LIMIT (Ruling P28 fix round 1, C1): END counts power the photon budget does not hold as "
      "detail; no single-image estimator can charge it without charging genuine sharpness (metric.py)")


def _np(img, box, var):
    """var (scene units) in the metric's units: a fraction of the box's mean target signal squared."""
    return var / metric.measure(img, box).signal ** 2


def _end(img, box, var, **kw):
    return metric.measure(img, box, noise_power=_np(img, box, var), **kw)


# --- the matched restoration: what each stack can actually give back ---------------------------

def _otf(shape, sigmas):
    """OTF of the mean of Gaussian blurs `sigmas` (a stack of frames of different seeing)."""
    f2 = metric._radial_frequency(shape) ** 2
    return np.mean([np.exp(-2 * math.pi ** 2 * s * s * f2) for s in np.atleast_1d(sigmas)], axis=0)


def restoration_error(img, truth, sigmas, var, box=None):
    """Relative error, over END's band, of the oracle Wiener restoration of `img` (truth blurred
    circularly by `sigmas`, plus white noise of variance `var`) back to `truth`: the restoration
    knows the true blur, the true object spectrum and the true noise, so this is the best any
    linear sharpening can do at this image's noise -- the question "sharpness at equal noise" asks.
    Exact: whole frame, periodic, no window (a window would not commute with the blur, and at low
    noise that mismatch, not the noise, would decide). `box` is unused (END's own business)."""
    O = np.fft.fft2(truth - truth.mean())
    Y = np.fft.fft2(img - truth.mean())
    H = _otf(truth.shape, sigmas)
    So, N = np.abs(O) ** 2, var * truth.size
    R = (H * So / (H * H * So + N)) * Y
    f = metric._radial_frequency(truth.shape)
    band = (f >= metric.BAND[0]) & (f < metric.BAND[1])
    return float((np.abs(R - O)[band] ** 2).sum() / (So[band]).sum())


def _stack(truth, sigmas, var, seed):
    """Mean of frames blurred (circularly, so the oracle is exact) by each of `sigmas`, plus the
    stack's white noise (variance var): an UNSMOOTHED stack, whose noise is exactly the budget."""
    blurred = np.real(np.fft.ifft2(np.fft.fft2(truth) * _otf(truth.shape, sigmas)))
    return add_noise(blurred, math.sqrt(var), seed)


def _ours_like(truth, seeing, frame_var, k, seed):
    """k frames, each moved by a random sub-pixel shift (exact), given its noise, and moved back with
    a cubic B-spline (order 3: the engine's interpolation), averaged -- noise smoothed as ours is."""
    rng = np.random.default_rng(seed)
    blurred = np.fft.fft2(np.real(np.fft.ifft2(np.fft.fft2(truth) * _otf(truth.shape, seeing))))
    acc = np.zeros(truth.shape)
    for _ in range(k):
        d = rng.uniform(-0.5, 0.5, 2)
        fr = np.real(np.fft.ifft2(ndimage.fourier_shift(blurred, -d))) + rng.normal(0, math.sqrt(frame_var), truth.shape)
        acc += ndimage.shift(fr, d, order=3, mode="nearest")
    return acc / k


def _drizzled(img):
    """x3 bilinear, shifted a native pixel, area-resampled back: a drizzled reference's smoothing."""
    up = ndimage.zoom(img, 3, order=1)
    out = refs.area_resample(ndimage.shift(up, (1.0, 1.0), order=1, mode="nearest"), 3)[:img.shape[0], :img.shape[1]]
    return np.pad(out, ((0, img.shape[0] - out.shape[0]), (0, img.shape[1] - out.shape[1])), mode="edge")


def _base_stack(kind, truth, var, seed=3):
    if kind == "unsmoothed":
        return _stack(truth, 1.2, var, seed)
    if kind == "ours-like":
        return _ours_like(truth, 1.2, var * 16, 16, seed)
    return _drizzled(_stack(truth, 1.2, var, seed))          # reference-like


def _perturb(img, how, amount, sigma, seed=9):
    rng = np.random.default_rng(seed)
    if how == "uniform":
        return img + rng.normal(0, amount * sigma, img.shape)
    if how == "shot":           # extra variance proportional to the level (amount x the stack's own at the mean)
        lvl = np.clip(img, 0, None)
        return img + rng.standard_normal(img.shape) * np.sqrt(amount * sigma ** 2 * lvl / lvl[lvl > 0.1].mean())
    if how == "target-only":
        return img + (img > 0.1) * rng.normal(0, amount * sigma, img.shape)
    return unsharp(img, 1.5, amount)                          # "usm"


@pytest.fixture(scope="module")
def truth():
    return planet_scene(seeing=0)


@pytest.fixture(scope="module")
def box(truth):
    return metric.target_box(ndimage.gaussian_filter(truth, 1.0))


# --- the plan's validation properties ----------------------------------------------------------

def test_blur_at_equal_noise_ranks_lower(truth, box):
    """A blurred copy at the same photon budget ranks below the original. (A copy blurred AFTER the
    noise is a smoothing, which END also charges as blur: test_smoothing_counts_as_blur.)"""
    var = SIGMA ** 2
    values = [_end(_stack(truth, s, var, 3), box, var).end for s in (1.0, 1.5, 2.0, 3.0)]
    print("\nEND at seeing 1.0 / 1.5 / 2.0 / 3.0: " + " ".join(f"{v:.3f}" for v in values))
    assert all(b < a for a, b in zip(values, values[1:])), values
    assert values[1] < 0.9 * values[0]


@pytest.mark.xfail(strict=True, raises=AssertionError, reason=C1)
@pytest.mark.parametrize("kind", ["unsmoothed", "ours-like", "reference-like"])
@pytest.mark.parametrize("how, amount", [("uniform", 1.0), ("uniform", 2.0), ("shot", 1.0), ("shot", 2.0),
                                         ("target-only", 1.0), ("target-only", 2.0), ("usm", 0.5), ("usm", 1.0)])
def test_added_noise_or_sharpening_never_raises_it(truth, box, kind, how, amount):
    """The plan's "added noise does not raise the score", for every kind of noise and for an unsharp
    mask, on an unsmoothed stack, one smoothed as ours is (B-spline) and one smoothed as a drizzled
    reference is. It FAILS for every case (strict xfail: a fix must turn the xfail off and update
    README / metric.py): END counts the power the budget does not hold as detail. Measured rises:
    README, "Known limit: noise and sharpening the budget does not hold"."""
    var = SIGMA ** 2
    img = _base_stack(kind, truth, var)
    budget = _np(img, box, var)
    base = metric.measure(img, box, noise_power=budget).end
    out = metric.measure(_perturb(img, how, amount, SIGMA), box, noise_power=budget).end
    print(f"\n{kind}, {how} {amount}: END {base:.3f} -> {out:.3f} (x{out / base:.3f})")
    assert out <= base * 1.005


@pytest.mark.xfail(strict=True, raises=AssertionError, reason=C1)
def test_noise_cannot_buy_back_blur(truth, box):
    """Seeing 2.0 plus 3 x the noise the budget holds must stay under 0.9 x seeing 1.0. It does not
    (C1): the unmodelled noise counts as detail and buys the blur back."""
    var = SIGMA ** 2
    sharp = _end(_stack(truth, 1.0, var, 3), box, var)
    soft_noisy = metric.measure(add_noise(_stack(truth, 2.0, var, 4), 3 * SIGMA, 5), box,
                                noise_power=sharp.end_noise_budget)
    print(f"\nsharp {sharp.end:.3f}, soft + 3 x noise {soft_noisy.end:.3f}")
    assert soft_noisy.end < 0.9 * sharp.end


def test_gain_and_offset_do_not_change_it(truth, box):
    var = SIGMA ** 2
    img = _stack(truth, 1.0, var, 3)
    a = _end(img, box, var).end
    b = _end(3.7 * img + 0.25, box, var * 3.7 ** 2).end
    assert b == pytest.approx(a, rel=1e-9)


# --- Ruling P28's new properties ---------------------------------------------------------------

@pytest.mark.parametrize("ratio", [2, 4])
def test_more_frames_at_the_same_blur_rank_higher(truth, box, ratio):
    """B is A averaged over `ratio` x the frames: same blur, 1/ratio the noise power. END ranks B
    above A (measured x1.20 for 2x, x1.49 for 4x: under sqrt(ratio), since the band's finest bins
    sit near the noise, where a restoration gains less than the SNR does). DAN, which removes each
    image's noise, scores them alike (within 1 %) -- it fails this test: that is why the ruling
    replaced it."""
    var = SIGMA ** 2
    a_img, b_img = _stack(truth, 1.2, var, 3), _stack(truth, 1.2, var / ratio, 4)
    a, b = _end(a_img, box, var), _end(b_img, box, var / ratio)
    print(f"\nframes x{ratio}: END {a.end:.3f} -> {b.end:.3f} (x{b.end / a.end:.3f}); DAN {a.value:.4f} -> "
          f"{b.value:.4f} (x{b.value / a.value:.3f})")
    assert b.end > a.end * (1 + 0.15 * math.log2(ratio))
    assert not b.value > a.value * (1 + 0.15 * math.log2(ratio))      # DAN would fail it
    assert b.value == pytest.approx(a.value, rel=0.01)


# (seeing A, noise A, seeing B, noise B): B always the sharper and the noisier.
SHARP_NOISY = [
    (1.6, 1.0, 1.0, 1.5),     # much sharper, a little noisier: B wins
    (1.1, 1.0, 1.0, 4.0),     # a little sharper, much noisier: A wins
    (2.0, 1.0, 1.2, 3.0),
    (1.3, 1.0, 1.0, 2.5),
]


def _sharp_noisy_case(truth, box, sa, na, sb, nb):
    va, vb = (na * SIGMA) ** 2, (nb * SIGMA) ** 2
    a_img, b_img = _stack(truth, sa, va, 3), _stack(truth, sb, vb, 4)
    ea, eb = restoration_error(a_img, truth, sa, va), restoration_error(b_img, truth, sb, vb)
    return ea, eb, _end(a_img, box, va), _end(b_img, box, vb)


@pytest.mark.parametrize("sa, na, sb, nb", SHARP_NOISY)
def test_sharper_but_noisier_ranks_as_the_matched_restoration_shows(truth, box, sa, na, sb, nb):
    """B is sharper but noisier than A. The oracle Wiener restoration of each (true blur, true
    object spectrum, true noise) says which gives back more of the band; END must agree."""
    ea, eb, a, b = _sharp_noisy_case(truth, box, sa, na, sb, nb)
    print(f"\nA s{sa} n{na} / B s{sb} n{nb}: restoration error {ea:.4f} / {eb:.4f}; END {a.end:.3f} / {b.end:.3f}; "
          f"DAN {a.value:.4f} / {b.value:.4f}")
    assert abs(ea - eb) > 0.03 * min(ea, eb), "the case must have a clear answer"
    assert (eb < ea) == (b.end > a.end)


def test_the_sharper_noisier_set_goes_both_ways_and_dan_gets_one_wrong(truth, box):
    """The set holds a win each way (review M10), and DAN -- blind to noise -- ranks at least one
    pair against the restoration, so the set discriminates (review M1)."""
    cases = [_sharp_noisy_case(truth, box, *c) for c in SHARP_NOISY]
    assert {eb < ea for ea, eb, _, _ in cases} == {True, False}
    assert any((eb < ea) != (b.value > a.value) for ea, eb, a, b in cases)


def _seeing(kind, seed):
    """200 frames' seeing sigmas (px), sorted sharpest first. Parametric, not a real capture's
    per-frame quality (README records the substitution: stack-probe does not export per-frame quality)."""
    rng = np.random.default_rng(seed)
    if kind == "log-normal":             # median 1.4 px, log-sd 0.45: seeing that drifts smoothly
        s = 1.4 * np.exp(0.45 * rng.standard_normal(200))
    else:                                # bimodal: a quarter of the frames in steady moments
        s = np.where(rng.random(200) < 0.25, 1.0, 3.0) * np.exp(0.05 * rng.standard_normal(200))
    return np.sort(s)


@pytest.mark.parametrize("kind, seed, frame_noise", [("log-normal", 21, 6), ("log-normal", 21, 2),
                                                     ("bimodal", 31, 6), ("bimodal", 31, 2)])
def test_a_20_and_a_50_percent_stack_rank_by_photon_budget_and_blur(truth, box, kind, seed, frame_noise):
    """The unconfounded form of the plan's AS P20/P50 check (Ruling P28, rule 4): ONE capture of
    200 frames whose seeing varies; its sharpest 20 % and sharpest 50 %, each the mean of its
    frames (PSF: the mean of their Gaussians) with the frame noise / k. Where the seeing drifts
    smoothly the extra frames cost little blur and P50 wins; where a quarter of the frames are
    steady moments and the rest poor, P50 adds blur faster than it removes noise and P20 wins.
    END must rank each pair as the exact matched restoration does, and the cases must go both ways.
    DAN ranks P20 first in every case, so it fails the log-normal ones (asserted: the test
    discriminates)."""
    s_all = _seeing(kind, seed)
    frame_var = (frame_noise * SIGMA) ** 2
    out = {}
    for pct in (20, 50):
        k = 200 * pct // 100
        s = s_all[:k]
        img = _stack(truth, s, frame_var / k, seed + pct)
        out[pct] = (restoration_error(img, truth, s, frame_var / k), _end(img, box, frame_var / k))
    (e20, r20), (e50, r50) = out[20], out[50]
    print(f"\n{kind}, frame noise {frame_noise}: P20 restoration error {e20:.4f}, END {r20.end:.3f}, DAN "
          f"{r20.value:.4f} | P50 {e50:.4f}, END {r50.end:.3f}, DAN {r50.value:.4f}")
    assert abs(e20 - e50) > 0.03 * min(e20, e50), "the case must have a clear answer"
    assert (e20 < e50) == (kind == "bimodal"), "the set must hold a case each way"
    assert (e20 < e50) == (r20.end > r50.end)
    if kind == "log-normal":
        assert r20.value > r50.value          # DAN's (wrong) answer here


@pytest.mark.parametrize("kernel", ["drizzle x3, area-resampled", "gaussian 0.7 (a denoise)", "cubic B-spline half-pixel shift"])
def test_smoothing_counts_as_blur(truth, box, kernel):
    """The reference-side bias, quantified: a known stack, then a tool's smoothing (drizzle and its
    resampling, a denoise, an interpolation). The photon budget does not change, so END must not
    rise; the white-floor reading of the same image -- its own box's 0.40-0.50 floor as its noise --
    does: that is the bias the photon budget removes. Numbers in README.md."""
    var = SIGMA ** 2
    img = _stack(truth, 1.2, var, 3)
    if kernel.startswith("drizzle"):
        smooth = _drizzled(img)
    elif kernel.startswith("gaussian"):
        smooth = ndimage.gaussian_filter(img, 0.7)
    else:
        smooth = ndimage.shift(img, (0.5, 0.5), order=3, mode="nearest")
    budget = _np(img, box, var)
    before, after = metric.measure(img, box, noise_power=budget), metric.measure(smooth, box, noise_power=budget)
    white_b = metric.measure(img, box, noise_power=metric.measure(img, box).noise_floor)
    white_a = metric.measure(smooth, box, noise_power=metric.measure(smooth, box).noise_floor)
    print(f"\n{kernel}: END {before.end:.3f} -> {after.end:.3f} (x{after.end / before.end:.3f}); white-floor END "
          f"{white_b.end:.3f} -> {white_a.end:.3f} (x{white_a.end / white_b.end:.3f})")
    assert after.end <= before.end * 1.005
    assert white_a.end > white_b.end


def _interior(img, npw):
    sharp = banded_planet(15)
    bx = metric.target_box(sharp)
    win = metric.interior_window(sharp, bx)
    return metric.measure(img, bx, interior=win, noise_power=npw, noise_power_interior=npw)


def test_interior_end_falls_with_an_interior_blur():
    sharp = banded_planet(15)
    bx = metric.target_box(sharp)
    var = 0.003 ** 2
    base_img = add_noise(sharp, 0.003, 1)
    npw = _np(base_img, bx, var)
    base = _interior(base_img, npw)
    soft = _interior(add_noise(banded_planet(15, interior_blur=1.0), 0.003, 2), npw)
    print(f"\ninterior END: base {base.end_interior:.3f}, interior blur 1 {soft.end_interior:.3f}")
    assert soft.end_interior < 0.9 * base.end_interior


@pytest.mark.xfail(strict=True, raises=AssertionError, reason=C1)
def test_interior_end_ignores_added_noise():
    sharp = banded_planet(15)
    bx = metric.target_box(sharp)
    base_img = add_noise(sharp, 0.003, 1)
    npw = _np(base_img, bx, 0.003 ** 2)
    base, noisy = _interior(base_img, npw), _interior(add_noise(sharp, 0.006, 3), npw)
    print(f"\ninterior END: base {base.end_interior:.3f}, noise x2 {noisy.end_interior:.3f}")
    assert noisy.end_interior <= base.end_interior * 1.005


def test_without_a_noise_budget_end_is_none_and_says_why(truth, box):
    r = metric.measure(_stack(truth, 1.0, SIGMA ** 2, 3), box)
    assert r.end is None and "noise" in r.end_note


def test_mosaic_mode_has_no_end(truth, box):
    img = _stack(truth, 1.0, SIGMA ** 2, 3)
    r = metric.measure(img, box, mosaic=True, noise_power=1e-5)
    assert r.end is None and "mosaic" in r.end_note


# --- noise.py: the per-frame noise -------------------------------------------------------------

def _cfa_frames(a=0.15, c=0.4, n=24, seed=7):
    """8-bit CFA frames of a drifting, jittering limb-darkened disc on a 3 DN sky, with Gaussian
    noise of variance a m + c, rounded (the rounding adds 1/12 DN^2)."""
    rng = np.random.default_rng(seed)
    base = 3 + 180 * planet_scene(size=(240, 320), radius=60, seed=4, seeing=1.0)
    gains = np.ones_like(base)
    gains[0::2, 0::2], gains[1::2, 1::2] = 0.7, 0.5
    out = []
    for t in range(n):
        shifted = ndimage.shift(base, (0.3 * t + rng.normal(0, 0.4), -0.2 * t + rng.normal(0, 0.4)), order=1, mode="nearest")
        m = shifted * gains
        out.append(np.clip(np.round(m + rng.standard_normal(m.shape) * np.sqrt(a * m + c)), 0, 255))
    return np.array(out, dtype=np.uint8), (a, c + 1 / 12)


def test_photon_transfer_recovers_a_known_line():
    """A textured disc drifting ~0.5 px per frame (scene change between frames that the high-pass
    leaves). Measured (fix round 2, the symmetric fit): slope +7 %, c 0.44 against 0.48. A bias on
    sigma_frame is a common factor on every image's budget."""
    frames, (a, c) = _cfa_frames()
    ptc = noise.photon_transfer(frames)
    print(f"\n{ptc} (true a {a}, c {c:.3f}); v(100) {ptc.variance(100):.3f} vs {a * 100 + c:.3f}")
    assert ptc.a == pytest.approx(a, rel=0.10)
    assert 0.0 <= ptc.c <= 1.5 * c
    for level in (30, 100, 150):
        assert ptc.variance(level) == pytest.approx(a * level + c, rel=0.10), level
    assert ptc.used_bins == len(ptc.bins) or ptc.used_bins >= 0.9 * len(ptc.bins)


def test_photon_transfer_fits_a_small_disc_on_its_own_levels():
    """Review I-C / I-E (Mars, Saturn): a 3 px disc in a 640 x 480 frame is under 0.05 % of the samples,
    so a binning cut at the 99.9th percentile (rounds 0-1) leaves the disc out (asserted below on the
    samples themselves) and fitted Mars on sky bins alone (a = 0). The fit must reach the disc's levels.
    Measured: a +60 % here (a 3 px disc is all limb, whose samples move between frames):
    an over-read, which raises every image's budget alike -- recorded as a limit for tiny discs."""
    rng = np.random.default_rng(3)
    a, c = 0.15, 0.4
    base = 3 + 150 * planet_scene(size=(480, 640), radius=3, seed=4, seeing=0.8)
    out = []
    for _ in range(128):
        m = ndimage.shift(base, rng.normal(0, 0.2, 2), order=1, mode="nearest")
        out.append(np.clip(np.round(m + rng.standard_normal(m.shape) * np.sqrt(a * m + c)), 0, 255))
    frames = np.array(out, dtype=np.uint8)
    disc = base > 40
    assert disc.mean() < 0.0005                                   # under 0.05 % of the samples
    assert np.percentile(frames, 99.9) < 0.5 * base.max()         # the old top would cut the disc away
    ptc = noise.photon_transfer(frames)
    print(f"\nsmall disc ({int(disc.sum())} px): {ptc}, kept levels {ptc.kept_levels}")
    assert ptc.kept_levels[1] > 60
    assert a <= ptc.a <= 1.7 * a


def test_photon_transfer_fits_through_a_black_pedestal():
    """Review N2-m6: an 8-bit capture whose sky sits on a black pedestal (3 DN, no noise: it rounds to
    one value, as the 2023-02-11 skies do) and whose signal above it has v = a (m - 3) + r. The line's
    intercept is then negative; with it clamped to 0, the constrained least squares through the origin
    bends the slope (asserted below on the same bins)."""
    from conftest import lunar_scene
    rng = np.random.default_rng(11)
    a, r, b = 0.14, 0.05, 3.0
    sig = 150 * lunar_scene(size=(240, 320), seed=4, seeing=1.0)
    sig[:, :120] = 0                                            # a third of the frame is bare sky
    out = []
    for _ in range(24):
        noisy = sig + rng.standard_normal(sig.shape) * np.sqrt(a * sig + r) * (sig > 0)
        out.append(np.clip(np.round(b + noisy), 0, 255))
    ptc = noise.photon_transfer(np.array(out, dtype=np.uint8))
    print(f"\npedestal: {ptc}, zero at {-ptc.c / ptc.a:.2f} DN")
    assert ptc.a == pytest.approx(a, rel=0.05)
    assert -ptc.c / ptc.a == pytest.approx(b, abs=1.0)
    L = np.array([x[0] for x in ptc.bins]); V = np.array([x[1] for x in ptc.bins])
    w = 1.0 / np.maximum(V, noise.QUANTISATION) ** 2       # relative-error weights, floored at the rounding variance
    through_origin = float((w * L * V).sum() / (w * L * L).sum())
    print(f"through the origin (c clamped to 0): a {through_origin:.4f}")
    assert np.isfinite(through_origin) and through_origin < 0.9 * a


@pytest.mark.parametrize("jitter", [0.0, 0.5, 1.0, 1.5])
def test_photon_transfer_on_lunar_texture_under_seeing_jitter(jitter):
    """Review M7 / I-H: unregistered consecutive frames of a frame-filling lunar texture jittered by up
    to 1.5 px (sd) per frame -- both shipped passes are lunar. Measured (fix round 3): v(50), v(150)
    within 0.5 %."""
    from conftest import lunar_scene
    rng = np.random.default_rng(5)
    a, c = 0.15, 0.4
    base = 10 + 200 * lunar_scene(size=(256, 384), seed=3, seeing=1.0)
    gains = np.ones_like(base)
    gains[0::2, 0::2], gains[1::2, 1::2] = 0.7, 0.5
    out = []
    for _ in range(24):
        m = (ndimage.shift(base, rng.normal(0, jitter, 2), order=1, mode="nearest") if jitter else base) * gains
        out.append(np.clip(np.round(m + rng.standard_normal(m.shape) * np.sqrt(a * m + c)), 0, 255))
    ptc = noise.photon_transfer(np.array(out, dtype=np.uint8))
    print(f"\njitter {jitter}: {ptc}")
    for level in (50, 150):
        assert ptc.variance(level) == pytest.approx(a * level + c + noise.QUANTISATION, rel=0.02)


def test_photon_transfer_refuses_a_line_through_too_few_bins():
    """Fewer than half the bins on the line (or a collapsed slope) is NoiseUnmeasurable, never a line
    extrapolated silently (review I-C)."""
    rng = np.random.default_rng(1)
    # Three level ranges with three unrelated noise laws (v = 0.15 m, 4 m, 0.01 m): no line holds half.
    base = np.tile(np.geomspace(5, 200, 320), (240, 1))
    law = np.where(base < 20, 0.15, np.where(base < 70, 4.0, 0.01)) * base
    out = []
    for _ in range(24):
        out.append(np.clip(np.round(base + rng.standard_normal(base.shape) * np.sqrt(law)), 0, 255))
    with pytest.raises(noise.NoiseUnmeasurable):
        noise.photon_transfer(np.array(out, dtype=np.uint8))
    flat = np.full((24, 240, 320), 40, np.uint8)            # no noise at all: the slope collapses to 0
    flat[:, ::7, ::5] = 41
    with pytest.raises(noise.NoiseUnmeasurable):
        noise.photon_transfer(flat)


def test_box_noise_refuses_to_extrapolate_past_the_fitted_levels():
    ptc = noise.PhotonTransfer(0.2, 0.5, ((3.0, 1.1, 5000), (10.0, 2.5, 5000), (20.0, 4.5, 5000)), 3, 1, (3.0, 20.0))
    with pytest.raises(noise.NoiseUnmeasurable):
        noise.box_noise(ptc, np.full((32, 32), 100.0) + np.arange(32)[None, :])


def test_box_noise_is_the_window_weighted_variance_over_the_signal_squared():
    """A bright block on a 10 DN sky: sigma^2 = sum over the CFA sites of sum w^2 v(level) /
    s_site^2, over sum w^2, with s_site the site's mean over the box minus the sky -- analytic."""
    ptc = noise.PhotonTransfer(0.2, 0.5, ((0.0, 0.5, 1), (10.0, 2.5, 1), (100.0, 20.5, 1)), 3, 1)
    h, w = 64, 96
    yy, xx = np.mgrid[0:h, 0:w]
    mean = 10.0 + 100.0 * (xx > 30) * (xx < 70) * (yy > 10) * (yy < 54)
    crop = mean[4:60, 6:90]
    got = noise.box_noise(ptc, crop)
    w2 = np.outer(np.hanning(56), np.hanning(84)) ** 2
    want = 0.0
    for dy, dx in noise.SITES:
        site, ws = crop[dy::2, dx::2], w2[dy::2, dx::2]
        want += float((ws * (0.2 * site + 0.5)).sum()) / (site.mean() - 10.0) ** 2
    want /= float(w2.sum())
    assert got == pytest.approx(want, rel=1e-6)


def test_raw_under_maps_the_stack_box_onto_flipped_drifted_frames():
    """The frames' mean is registered to the stack (orientation and shift): a stack flipped and
    shifted against its raw frames still gets the raw pixels under its box."""
    frames, _ = _cfa_frames(n=4)
    mean = frames.astype(np.float64).mean(axis=0)
    stack = np.roll(np.flipud(metric.flatten_cfa(mean)), (6, -10), axis=(0, 1))
    box = metric.target_box(stack)
    raw = noise.raw_under(mean, stack, box)
    assert raw.orientation == "flipud"
    want = np.roll(np.flipud(mean), (6, -10), axis=(0, 1))[box.y0:box.y1, box.x0:box.x1]
    np.testing.assert_allclose(raw.crop, want[raw.sub])


def test_photon_transfer_refuses_too_little_data():
    with pytest.raises(noise.NoiseUnmeasurable):
        noise.photon_transfer(np.zeros((1, 16, 16), np.uint8))


# --- real data (NAS, read-only): END on AutoStakkert's 2_12_2023 P20 stacks --------------------

REAL = {
    "jupiter": ("2023-02-11-jupiter", ("2_12_2023", "AS_P20", "2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png")),
    "moon": ("2023-02-12-moon", ("2_12_2023", "AS_P20", "2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap1.png")),
}


@pytest.fixture(scope="module", params=sorted(REAL))
def real(request):
    from conftest import nas_path
    cid, parts = REAL[request.param]
    c = benchset.by_id(cid)
    lum = metric.luminance(refs.load_image(nas_path(*parts)))
    box = metric.target_box(lum)
    ptc, raw_mean = noise.measure_video(nas_path(c.video), c.size)
    raw = noise.raw_under(raw_mean, lum, box)
    k = 0.20 * c.frames
    budget = {"noise_power": noise.box_noise(ptc, raw) / k}
    return request.param, lum, box, budget


@pytest.mark.nas
@pytest.mark.ffmpeg
def test_real_blur_and_smoothing_rank_lower(real):
    """A copy of AutoStakkert's P20 blurred after the fact keeps its photon budget: END charges the
    smoothing as blur and falls with it (strictly)."""
    name, lum, box, budget = real
    values = [metric.measure(lum, box, **budget).end] + [
        metric.measure(ndimage.gaussian_filter(lum, s), box, **budget).end for s in (0.5, 1.0, 2.0)]
    print(f"\n{name} AS_P20 END, blur 0 / 0.5 / 1 / 2: " + " ".join(f"{v:.3f}" for v in values))
    assert all(b < a for a, b in zip(values, values[1:])), values


@pytest.mark.nas
@pytest.mark.ffmpeg
@pytest.mark.xfail(strict=True, raises=AssertionError, reason=C1)
@pytest.mark.parametrize("how, amount", [("uniform", 0.005), ("uniform", 0.02), ("usm", 1.0)])
def test_real_added_noise_or_sharpening_never_raises_it(real, how, amount):
    """The plan's property on real AutoStakkert P20 stacks (uniform noise as a fraction of the mean
    target signal; an unsharp mask sigma 1.5): fails, C1 (strict xfail)."""
    name, lum, box, budget = real
    base = metric.measure(lum, box, **budget)
    img = add_noise(lum, amount * base.signal, 7) if how == "uniform" else unsharp(lum, 1.5, amount)
    out = metric.measure(img, box, **budget)
    print(f"\n{name} AS_P20 {how} {amount}: END {base.end:.3f} -> {out.end:.3f} (x{out.end / base.end:.3f})")
    assert out.end <= base.end * 1.005


# --- the noise model end to end, through the engine (needs the built stack-probe and
# stack-region-probe; skipped otherwise) ----------------------------------------------------------

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build", "test")
PROBES = (os.path.join(BUILD, "stack-probe"), os.path.join(BUILD, "stack-region-probe"))


def _fresh_probes():
    """The engine tests run only on probes built from the tree: not built is a skip (the decorators);
    built but older than a source they are built from FAILS (final review, bench M2) -- a stale probe
    tests an engine that is no longer the one in the tree."""
    import identity
    stale = [s for p in PROBES for s in identity.stale(p)]
    if stale:
        pytest.fail("stale probes, rebuild them (cmake --build modules/planetary/build): " + "; ".join(stale))


@pytest.mark.skipif(not all(os.access(p, os.X_OK) for p in PROBES), reason="stack-probe / stack-region-probe not built")
def test_region_probe_box_k_matches_analytic_layouts():
    """stack-region-probe --keff-selftest: BoxKeff on hand-built plans -- one region keeping 37 of 100
    (k 37), a region keeping none (the fallback, k 100), two blended nodes with nested kept sets
    (against per-pixel weights written out from the layout's geometry, not the engine's AxisWeightAt;
    it checks BoxKeff's sums over the engine's blend rule, not the rule; reviews M2 / N-m6 / N2-m8)."""
    import subprocess
    _fresh_probes()
    r = subprocess.run([PROBES[1], "--keff-selftest"], capture_output=True, text=True)
    print("\n" + r.stdout)
    assert r.returncode == 0 and r.stdout.count(": ok") == 3


@pytest.mark.skipif(not all(os.access(p, os.X_OK) for p in PROBES), reason="stack-probe / stack-region-probe not built")
@pytest.mark.parametrize("kind, jitter, seed, n, steady", [("disc", 0.5, 7, 400, None), ("lunar", 0.6, 10, 600, 0.3)])
def test_end_to_end_noise_model_through_the_engine(tmp_path, kind, jitter, seed, n, steady):
    """Synthetic RGGB frames of known noise (a m + c) -> noise.photon_transfer -> the engine stacks the
    even and odd halves -> stack-region-probe's noise-power k over A's box (checked against
    stack-probe's plan) -> noise.box_noise -> the halves' difference against sigma_frame^2 (1/k_A +
    1/k_B) / 4, with the MEASURED photon-transfer line. It must lie within the kind's frozen tolerance
    and within 30 % of the kind's calibrated ratio. Both cases are held out of the calibration (scenes 7
    and 10, outside halves.CAL_SEEDS, at jitters it does not use), so the 30 % check is not circular
    (review N2-m4).
    The lunar case carries per-frame seeing (ruling E3): a random `steady` share of the frames at the
    scene's own seeing, the rest blurred by a further sigma 1.5 px. The engine then has a real reason
    to keep a subset, so its box k is well under the half's frame count, and the half's frame count as
    k misses the model: the test discriminates the k (review M2). The two seeing levels are far apart,
    so each half keeps steady frames only and the kept sets hold the same blur (no unshared signal in
    D, as in the calibration). Until fix round 4 this case relied on the generator's wrapped edge
    (e3-report.md), which made the engine keep 18 and 16 of 300 for a reason the scene did not have.
    (The box k and the mean-kept reading agree on these synthetics, whose regions are alike, so they
    are not told apart here.)"""
    _fresh_probes()
    import halves
    import json as _json
    assert seed not in halves.CAL_SEEDS
    assert f"{kind}-j{jitter}-s{seed}" not in {r["condition"] for r in halves.load_calibration()["kinds"][kind]["runs"]}
    blur = None if steady is None else np.where(np.random.default_rng(seed + 1000).random(n) < steady, 0.0, 1.5)
    frames, mean, truth = halves.synthetic_frames(kind, n, jitter, 0.15, 0.6, seed, blur=blur)
    measured = noise.photon_transfer(frames[:64])
    print(f"\nphoton transfer {measured} (true a {truth.a}, c {truth.c:.3f})")
    assert measured.a == pytest.approx(truth.a, rel=0.10)
    k, used, box = {}, {}, None
    for half, sl in (("A", slice(0, None, 2)), ("B", slice(1, None, 2))):
        ser = str(tmp_path / f"h{half}.ser")
        halves.write_ser(ser, frames[sl])
        fits = str(tmp_path / f"h{half}.fits")
        probe = _json.loads(halves._run([PROBES[0], ser, "--bayer", "RGGB", "--out", fits, "--json"]))
        used[half] = probe["used"]
        if box is None:
            A_lum = metric.luminance(refs.load_image(fits))
            box = hbox = metric.target_box(A_lum)
        else:                                     # A's box in B's own grid (review N2-m5)
            hbox = halves.box_in_b(A_lum, metric.luminance(refs.load_image(fits)), box)
        kj = str(tmp_path / f"h{half}.keff.json")
        halves._run([PROBES[1], ser, "--bayer", "RGGB", "--plan-only", "--keff-box",
                     f"{hbox.y0},{hbox.y1},{hbox.x0},{hbox.x1}", "--json-out", kj])
        with open(kj) as fh:
            k[half], why = benchset.ours_frames_box(probe, _json.load(fh))
        assert k[half] is not None, why
    cal = halves.load_calibration()["kinds"][kind]
    lo, hi = cal["tolerance"]
    r = halves.ratio(str(tmp_path / "hA.fits"), str(tmp_path / "hB.fits"), k["A"], k["B"], measured, mean)
    naive = halves.ratio(str(tmp_path / "hA.fits"), str(tmp_path / "hB.fits"), n // 2, n // 2, measured, mean)
    print(f"{kind}: k {k['A']:.0f}/{k['B']:.0f} (used {used['A']}/{used['B']}): 0.21-0.30 x{r['high']:.3f} "
          f"(calibrated {cal['high_mean']:.3f}); with the half's {n // 2} frames as k x{naive['high']:.3f}"
          + ("" if blur is None else f"; steady frames per half {int((blur[0::2] == 0).sum())}/{int((blur[1::2] == 0).sum())}"))
    assert lo <= r["high"] <= hi
    assert r["high"] == pytest.approx(cal["high_mean"], rel=0.30)
    if steady is not None:
        assert max(k.values()) < 0.6 * (n // 2)           # the engine dropped the blurred frames
        assert not naive["high"] == pytest.approx(cal["high_mean"], rel=0.30)


def test_the_tolerance_has_a_real_lower_bound():
    """Re-review 3 N3-m1: on fix round 3's 12 lunar runs (calibration.json at ee79ddfd), mean - 5.81 sd
    on the linear scale is below 0, so the band could never block a capture from below. On log R the
    interval is multiplicative and both ends are real: here 0.272-2.665. exp(log mean) is the interval's
    geometric centre."""
    import halves
    r3 = [0.868, 0.584, 1.002, 0.874, 1.056, 0.936, 0.808, 0.575, 0.934, 0.821, 0.978, 0.953]
    lo, hi, m, sd, kf = halves.tolerance(r3, 6)
    print(f"\nround-3 lunar runs: log-scale tolerance {lo:.3f}-{hi:.3f}; linear mean - k sd "
          f"{np.mean(r3) - kf * np.std(r3, ddof=1):.3f}")
    assert np.mean(r3) - kf * np.std(r3, ddof=1) < 0 < 0.25 < lo < min(r3)
    assert lo * hi == pytest.approx(math.exp(2 * m))
    assert halves.tolerance([0.5, 2.0, 1.0], 3)[:2] == pytest.approx(
        (1 / halves.tolerance([0.5, 2.0, 1.0], 3)[1], halves.tolerance([0.5, 2.0, 1.0], 3)[1]))


def test_the_calibration_runs_every_scene_and_jitter_with_and_without_a_pedestal():
    """Re-review 3 N3-m6: every real capture but the 2026-01-30 Jupiter fits a negative intercept (a black
    pedestal), so the calibration measures that regime too, on the same scenes and jitters."""
    import halves
    conds = halves.calibration_conditions()
    for kind in ("disc", "lunar"):
        for j in halves.CAL_JITTERS:
            for s in halves.CAL_SEEDS:
                assert {p for k, jj, ss, p in conds if (k, jj, ss) == (kind, j, s)} == {0.0, 3.0}


def test_the_frozen_calibration_holds_every_condition_of_the_procedure():
    """calibration.json was frozen by the procedure in this tree: one run per calibration condition, the
    tolerance and R_cal as halves.tolerance and the mean over the runs give them (fix round 4)."""
    import halves
    cal = halves.load_calibration()["kinds"]
    for kind in ("disc", "lunar"):
        runs = cal[kind]["runs"]
        assert sorted(r["condition"] for r in runs) == sorted(
            halves.condition_tag(k, j, s, p) for k, j, s, p in halves.calibration_conditions() if k == kind)
        lo, hi = halves.tolerance([r["high"] for r in runs], cal[kind]["n_eff"])[:2]
        assert cal[kind]["tolerance"] == pytest.approx([lo, hi])
        assert cal[kind]["r_cal"] == pytest.approx(list(np.mean([r["ratio"] for r in runs], axis=0)))
        assert all("tiles" in r and r["tiles"]["n"] > 0 for r in runs)        # measured with the P28d tile shifts


@pytest.mark.skipif(not all(os.access(p, os.X_OK) for p in PROBES), reason="stack-probe / stack-region-probe not built")
def test_the_frozen_calibration_describes_the_probes_in_use_and_reproduces(tmp_path):
    """Ruling F2 as a test: calibration.json was frozen with the identity in use (engine, probe sources and
    binaries, procedure) -- else every verdict is NO VERDICT and this fails, naming the part to re-freeze --
    and one of its conditions, re-run now, reproduces its frozen ratio. The final review measured that
    condition reproducing within 1e-5 relative at 1, 7 and 32 threads, so 1e-4 is the bound."""
    import halves
    import identity
    _fresh_probes()
    cal = halves.load_calibration()
    used = identity.current(*PROBES)
    assert identity.mismatches(cal.get("identity"), used) == [], "re-freeze calibration.json (halves.py calibrate --write)"
    tag = "lunar-j0.7-s2-p3"
    frozen = next(r for r in cal["kinds"]["lunar"]["runs"] if r["condition"] == tag)
    (now,) = halves.calibrate(str(tmp_path), *PROBES, conditions=[("lunar", 0.7, 2, 3.0)])
    print(f"\n{tag}: frozen x{frozen['high']:.6f} k {frozen['k']}; now x{now['high']:.6f} k {now['k']}")
    assert now["high"] == pytest.approx(frozen["high"], rel=1e-4)
    assert now["k"] == pytest.approx(frozen["k"], rel=1e-9)


def test_a_pedestal_synthetic_fits_the_negative_intercept_real_captures_fit():
    """Re-review 3 N3-m6: with the pedestal the sky rounds to one 8-bit value and the line is
    v = a m + (c + 1/12 - a b) < 0, as the real captures' fits read (-0.03 to -3.17); the measured line
    must find it, as on the real captures, or the calibration would not represent them."""
    import halves
    for kind in ("disc", "lunar"):
        frames, _, truth = halves.synthetic_frames(kind, noise.FRAMES, 0.3, halves.CAL_SHOT, halves.CAL_PEDESTAL_READ,
                                                   1, pedestal=3.0)
        m = noise.photon_transfer(frames)
        print(f"\n{kind}, 3 DN pedestal: {m}, zero at {-m.c / m.a:.2f} DN; true a {truth.a}, c {truth.c:.3f}")
        assert truth.c < 0 and m.c < 0
        assert m.a == pytest.approx(truth.a, rel=0.08)
        assert m.c == pytest.approx(truth.c, abs=0.15)


# --- Ruling P28b: ours charged its measured noise -----------------------------------------------

def test_charge_is_the_measured_ratio_over_the_calibrated_one_never_below_one():
    import halves
    assert halves.charge([2.0, 0.5, 1.0] + [1.0] * 6, [1.0, 1.0, 0.5] + [1.0] * 6) == [2.0, 1.0, 2.0] + [1.0] * 5


@pytest.mark.parametrize("how, amount", [("uniform", 1.0), ("uniform", 2.0), ("target-only", 2.0)])
def test_charging_ours_its_measured_noise_stops_added_noise_raising_end(truth, box, how, amount):
    """The mechanism of Ruling P28b on an unsmoothed stack: when the noise ours actually carries is
    charged (here the true one: budget + the added variance, per bin, as the split halves would
    measure it), added noise no longer raises END -- unlike the bare budget (C1)."""
    var = SIGMA ** 2
    img = _stack(truth, 1.2, var, 3)
    budget = _np(img, box, var)
    base = metric.measure(img, box, noise_power=budget).end
    noisy = _perturb(img, how, amount, SIGMA)
    added = (amount * SIGMA) ** 2 * (1.0 if how == "uniform" else float((img[box.y0:box.y1, box.x0:box.x1] > 0.1).mean()))
    factor = [1.0 + added / var] * (len(metric.END_EDGES) - 1)
    bare = metric.measure(noisy, box, noise_power=budget).end
    charged = metric.measure(noisy, box, noise_power=budget, noise_factor=factor).end
    print(f"\n{how} {amount}: END {base:.3f}; noisy, bare budget {bare:.3f}; charged x{factor[0]:.2f} {charged:.3f}")
    assert bare > base and charged <= base * 1.005


# --- Ruling P28d: local registration is not charged as noise -----------------------------------

def _node_field(shape, step, sd, rng, up):
    """An engine-like shift field (luma px) on a grid `up` x finer: per-node shifts N(0, sd) on nodes
    `step` px apart, blended with sin^2 weights between neighbours (Regions.h: half-overlapping
    windows of side 2 step, sin^2 blend weights summing to 1)."""
    H, W = shape
    v = rng.normal(0, sd, (H // (step * up) + 3, W // (step * up) + 3))

    def axis(n):
        u = np.arange(n) / up / step + 1
        i = np.floor(u).astype(int)
        return i, np.cos(np.pi * (u - i) / 2) ** 2, np.sin(np.pi * (u - i) / 2) ** 2
    (iy, y0, y1), (ix, x0, x1) = axis(H), axis(W)
    return (y0[:, None] * (x0 * v[iy][:, ix] + x1 * v[iy][:, ix + 1])
            + y1[:, None] * (x0 * v[iy + 1][:, ix] + x1 * v[iy + 1][:, ix + 1]))


def _halves_pair(sd, sigma_a, sigma_b, step=64, size=(256, 320), up=4, seed=4):
    """Two 'half stacks' of one textured surface: B's scene moved by an engine-like local shift field
    of `sd` px per node (0: the same scene), each with its own white noise. The scene is drawn `up` x
    finer, warped there (cubic), and both are area-binned back, so the shift is a real one, not its
    first-order expansion. Returns (A, B)."""
    from conftest import fractal_field
    h, w = size
    H, W = h * up, w * up
    base = 0.45 + 0.96 * ndimage.gaussian_filter(fractal_field((H, W), seed + 100, 1.2), 20 * up)
    hi = ndimage.gaussian_filter(np.clip(base + 0.10 * fractal_field((H, W), seed, 1.5), 0.02, None), 1.2 * up)
    rng = np.random.default_rng(seed + 7)
    dy, dx = (_node_field((H, W), step, sd, rng, up) * up for _ in range(2))
    yy, xx = np.mgrid[0:H, 0:W].astype(float)
    warped = ndimage.map_coordinates(hi, [yy - dy, xx - dx], order=3, mode="reflect") if sd else hi
    down = lambda z: z.reshape(h, up, w, up).mean(axis=(1, 3))      # noqa: E731
    noise_rng = np.random.default_rng(1)
    return (down(hi) + noise_rng.normal(0, sigma_a, (h, w)), down(warped) + noise_rng.normal(0, sigma_b, (h, w)))


def _halves_ratio(A, B, sigma, **kw):
    """D's power per END bin (halves.refined_difference) over the model of two halves of noise sigma."""
    import halves
    D, box = halves.refined_difference(A, B, **kw)[:2]
    s = metric.measure(A, box).signal
    p = metric.periodogram(D[box.y0:box.y1, box.x0:box.x1] / s)
    return np.array(metric.band_means(p, metric.END_EDGES)) / (2 * sigma ** 2 / 4 / s ** 2)


P28D_SIGMA = 0.001     # stack noise against a surface of mean ~0.5: the leak dominates the low bins


def test_a_local_shift_between_the_halves_is_not_charged_as_noise():
    """Ruling P28d: halves differing only by an engine-like local shift (nodes 64 px apart, 0.1 px sd)
    charge about nothing. The charge is taken as the bench takes it, max(1, R / R_cal), with R_cal the
    same scene's noise-only pair."""
    import halves
    honest = _halves_ratio(*_halves_pair(0.0, P28D_SIGMA, P28D_SIGMA), P28D_SIGMA)
    shifted = _halves_ratio(*_halves_pair(0.1, P28D_SIGMA, P28D_SIGMA), P28D_SIGMA)
    unremoved = _halves_ratio(*_halves_pair(0.1, P28D_SIGMA, P28D_SIGMA), P28D_SIGMA, tile=None)
    charge = halves.charge(list(shifted) + [1.0], list(honest) + [1.0])
    print("\nlocal shift: per END bin " + " ".join(f"{v:.2f}" for v in shifted) + "; charge " + " ".join(f"{v:.2f}" for v in charge)
          + "; without the tile shifts " + " ".join(f"{v:.2f}" for v in unremoved))
    assert max(charge) < 1.25
    assert unremoved[0] > 3 * shifted[0]          # a field the global refinement alone leaks


@pytest.mark.parametrize("amp, above", [(0.02, 1.1), (0.05, 1.5)])
def test_a_level_offset_between_the_halves_is_never_charged_less_for_the_tile_shifts(amp, above):
    """Ruling P28d removes registration only. A smooth gain offset between the halves over part of a disc
    and its limb (Investigation 2's level offsets between kept subsets) is not registration; near a limb a
    gain change partly resembles a limb shift, so a shift fit could absorb it and under-charge ours. The
    bench's fit must not: per END bin it charges at least what the offset itself charges -- the gain-only
    difference (triage.gain_only_difference: the registration and one global gain, no shift fit), less 3 %
    for the fit's degrees of freedom -- and the global refinement alone (tile=None) reads it as the
    gain-only difference does (within 3 %).
    Until bench6 the reference here was the global refinement alone, whose fit read the bump partly as a
    shift (Ruling F5): it charged a 2 % bump x2.79 in the 0.06-0.09 bin and the tile fit x5.05, where the
    bump itself charges x1.18 (measured at ad36e4b7; at 5 %: x12.27 and x26.60 against x1.82). Both
    readings were the shift fit's, not the bump's. Now: 2 % reads x1.17 with the tiles, x1.18 globally and
    gain-only; 5 %, x1.92, x1.82 and x1.82. (A smoothly interpolated shift field was tried in fix round 4:
    it under-charged this pair.)"""
    import triage
    from conftest import planet_scene
    S = planet_scene(size=(240, 320), radius=90, seed=3, seeing=1.0)
    yy, xx = np.mgrid[0:240, 0:320]
    bump = np.exp(-(((yy - 190) / 30.0) ** 2 + ((xx - 160) / 60.0) ** 2))
    rng = np.random.default_rng(2)
    A = S + rng.normal(0, P28D_SIGMA, S.shape)
    B = S * (1 + amp * bump) + rng.normal(0, P28D_SIGMA, S.shape)
    fitted, bare = _halves_ratio(A, B, P28D_SIGMA), _halves_ratio(A, B, P28D_SIGMA, tile=None)
    d, _, bx = triage.gain_only_difference(A, B)
    s = metric.measure(A, bx).signal
    truth = np.array(metric.band_means(metric.periodogram(d / s), metric.END_EDGES)) / (2 * P28D_SIGMA ** 2 / 4 / s ** 2)
    print(f"\n{amp:.0%} gain bump: per END bin, tile fit " + " ".join(f"{v:.2f}" for v in fitted) + "; global only "
          + " ".join(f"{v:.2f}" for v in bare) + "; gain-only " + " ".join(f"{v:.2f}" for v in truth))
    assert truth[0] > above                                # the offset is above the noise
    assert all(f >= 0.97 * t for f, t in zip(fitted, truth))
    assert all(b == pytest.approx(t, rel=0.03) for b, t in zip(bare, truth))


def _gain_field(shape, amplitude, period=128):
    """A gain difference varying smoothly across the frame (period `period` px along y and x), as the
    engine's region blends make the halves' differences vary (test_triage.py's field)."""
    yy, xx = np.mgrid[0:shape[0], 0:shape[1]].astype(float)
    return amplitude * np.sin(2 * np.pi * yy / period) * np.cos(2 * np.pi * xx / period)


def _high_band(D, box, s):
    """D's power over 0.21-0.30 cycles/px (halves.HIGH_BINS), D over the box in units of the signal s."""
    import halves
    p = metric.periodogram(D[box.y0:box.y1, box.x0:box.x1] / s)
    return float(np.mean(metric.band_means(p, metric.END_EDGES)[halves.HIGH_BINS]))


@pytest.mark.parametrize("seed", [3, 7])
def test_a_smooth_gain_difference_between_the_halves_is_charged_as_gain_not_as_shift(seed):
    """Ruling F5 (bench6): a smooth gain difference between the halves -- kept subsets at different
    levels, varying across the frame as the engine's region blends do -- is a real difference, so it is
    charged, but as itself: refined_difference must read it as a gain-only difference reads it
    (triage.gain_only_difference: the registration and one global gain, no shift fit), not turn it into a
    shift. Synthetic, truth known: B = A x (1 + a 3 % field), no shift. Measured at 80282e0e (before the
    fix): the global fit read the field as a (+0.099, -0.181) px shift on seed 3, and with noise of 0.4 %
    of the mean the 0.21-0.30 band read x1.51 (seed 3) and x2.02 (seed 7) the noise-only pair's, where the
    gain-only difference reads x1.005 and x0.994 (the field is nearly invisible there)."""
    import halves
    import triage
    from conftest import lunar_scene
    A = lunar_scene(size=(256, 256), seed=seed, seeing=1.0)
    B = A * (1 + _gain_field(A.shape, 0.03))
    coef = halves.refined_difference(A, B, tile=None)[3]
    rng = np.random.default_rng(11)
    sd = 0.004 * A.mean()
    nA, nB = rng.normal(0, sd, A.shape), rng.normal(0, sd, A.shape)

    def gain_only(X, Y):
        d, _, bx = triage.gain_only_difference(X, Y)
        D = np.zeros_like(X)
        D[bx.y0:bx.y1, bx.x0:bx.x1] = d
        return D, bx

    D_noise, box = halves.refined_difference(A + nA, A + nB)[:2]
    s = metric.measure(A, box).signal
    refined = _high_band(halves.refined_difference(A + nA, B + nB)[0], box, s) / _high_band(D_noise, box, s)
    G_gain, gbx = gain_only(A + nA, B + nB)
    G_noise, _ = gain_only(A + nA, A + nB)
    truth = _high_band(G_gain, gbx, s) / _high_band(G_noise, gbx, s)
    print(f"\nseed {seed}: global shift fit ({coef[1]:+.4f}, {coef[2]:+.4f}) px; 0.21-0.30 band over the noise-only "
          f"pair: refined_difference x{refined:.3f}, gain-only x{truth:.3f}")
    assert refined == pytest.approx(truth, rel=0.02)
    assert abs(coef[1]) <= 0.005 and abs(coef[2]) <= 0.005


@pytest.mark.parametrize("seed", [3, 7])
def test_the_tiles_level_field_is_the_lowest_order_that_reads_a_gain_field_as_gain(seed, monkeypatch):
    """Why remove_tile_shifts models a cubic level field (halves.LEVEL_ORDER, Ruling F5): on the gain-only
    pair of the test above, the 0.21-0.30 band over the noise-only pair per order of the tiles' level field
    -- one intercept (the procedure before bench6) x1.512 / x2.016 (seeds 3 / 7), first order x1.147 /
    x1.303, second order x1.025 / x1.031, cubic x1.006 / x0.998, against gain-only x1.005 / x0.994. The
    cubic is the lowest order within 2 % of the gain-only reading."""
    import halves
    import triage
    from conftest import lunar_scene
    A = lunar_scene(size=(256, 256), seed=seed, seeing=1.0)
    B = A * (1 + _gain_field(A.shape, 0.03))
    rng = np.random.default_rng(11)
    sd = 0.004 * A.mean()
    nA, nB = rng.normal(0, sd, A.shape), rng.normal(0, sd, A.shape)
    d_g, _, gbx = triage.gain_only_difference(A + nA, B + nB)
    d_n, _, _ = triage.gain_only_difference(A + nA, A + nB)
    G_g, G_n = np.zeros_like(A), np.zeros_like(A)
    G_g[gbx.y0:gbx.y1, gbx.x0:gbx.x1], G_n[gbx.y0:gbx.y1, gbx.x0:gbx.x1] = d_g, d_n
    reading = {}
    for order in (0, 1, 2, 3):
        monkeypatch.setattr(halves, "LEVEL_ORDER", order)
        D_noise, box = halves.refined_difference(A + nA, A + nB)[:2]
        s = metric.measure(A, box).signal
        reading[order] = _high_band(halves.refined_difference(A + nA, B + nB)[0], box, s) / _high_band(D_noise, box, s)
    truth = _high_band(G_g, gbx, s) / _high_band(G_n, gbx, s)
    print(f"\nseed {seed}: level order 0-3 " + " ".join(f"x{reading[o]:.3f}" for o in range(4)) + f"; gain-only x{truth:.3f}")
    assert [o for o in range(4) if reading[o] == pytest.approx(truth, rel=0.02)] == [3]
    monkeypatch.undo()
    assert halves.LEVEL_ORDER == 3


def test_noise_between_the_halves_is_still_charged():
    """Ruling P28d: removing the local shift must not remove noise. Noise-only halves read the true
    model per END bin (within -15 / +10 %: the fit's degrees of freedom cost a little in the lowest
    bins, which the calibration measures alike), and halves where B carries twice the noise charge it:
    (1 + 4) / 2 = 2.5 x the noise-only pair, per bin."""
    import halves
    honest = _halves_ratio(*_halves_pair(0.0, P28D_SIGMA, P28D_SIGMA), P28D_SIGMA)
    noisy = _halves_ratio(*_halves_pair(0.0, P28D_SIGMA, 2 * P28D_SIGMA), P28D_SIGMA)
    charge = halves.charge(list(noisy) + [1.0], list(honest) + [1.0])
    unremoved = _halves_ratio(*_halves_pair(0.0, P28D_SIGMA, P28D_SIGMA), P28D_SIGMA, tile=None)  # printed only
    print("\nnoise only: per END bin " + " ".join(f"{v:.2f}" for v in honest) + " (without the tile shifts "
          + " ".join(f"{v:.2f}" for v in unremoved) + "); B at 2 x the noise: charge "
          + " ".join(f"{v:.2f}" for v in charge))
    assert all(0.85 <= v <= 1.10 for v in honest)
    assert all(v == pytest.approx(2.5, rel=0.08) for v in charge)


def test_noise_factor_must_be_one_per_bin_and_at_least_one(truth, box):
    img = _stack(truth, 1.2, SIGMA ** 2, 3)
    with pytest.raises(ValueError):
        metric.measure(img, box, noise_power=1e-4, noise_factor=[1.0] * 3)
    with pytest.raises(ValueError):
        metric.measure(img, box, noise_power=1e-4, noise_factor=[0.5] * 8)


def test_provenance_reads_the_autostakkert_session_and_marks_asistudio_unknown(tmp_path):
    d = tmp_path / "AS_P20"
    d.mkdir()
    (d / "cap_lapl5_ap1.png").write_bytes(b"")
    (d / "cap_lapl5_ap1.as3").write_text("Settings\n  _menu_image_calibration_row_noise   False\n"
                                         "  _experimental_pre_processing_vertical_blur_enabled   False\n"
                                         "  _stack_sharpened   True\n")
    p = refs.provenance(str(d / "cap_lapl5_ap1.png"))
    assert p["state"] == "none"
    (d / "cap_lapl5_ap1.as3").write_text("  _menu_image_calibration_row_noise   True\n")
    assert refs.provenance(str(d / "cap_lapl5_ap1.png"))["state"] == "processed"
    (d / "cap2_lapl5_ap1.png").write_bytes(b"")
    assert refs.provenance(str(d / "cap2_lapl5_ap1.png"))["state"] == "unknown"           # no .as3
    assert refs.provenance(str(d / "cap_lapl5_ap1_conv.png"))["state"] == "processed"
    asi = tmp_path / "ASIVideoStack_Output"
    asi.mkdir()
    from astropy.io import fits
    h = fits.Header()
    h["PN"], h["SHARPEN"] = 1, "20"
    fits.writeto(asi / "cap_Planet_40.fit", np.zeros((4, 4), np.float32), h)
    p = refs.provenance(str(asi / "cap_Planet_40.fit"))
    assert p["state"] == "unknown" and "PN 1" in p["text"]


@pytest.mark.parametrize("seed", [3, 6])
def test_synthetic_lunar_frames_carry_no_wrapped_edge(seed):
    """halves.synthetic_frames moves a static scene by sub-pixel jitter only, so every frame shows the
    same detail: what changes at the frame's edge must be no more than what changes just inside it.
    A circular (FFT) shift of the lunar scene wraps its 6 DN sky strip (columns 0-59) onto the far
    edge: a frame moved left by ~0.5 px or more gains a hard dark edge at its last columns, its
    whole-frame quality jumps (to 1.44 x the median on scene 6, 2.2 x on scene 3 at jitter 0.7), and
    the engine's selection -- doing what Select.h step 8 says on frames that really differ -- keeps
    5-32 of 300 (e3-report.md). Scenes 3 and 6: the brightest right edges (165 / 231 DN on the 6 DN sky)."""
    import halves
    frames, _, _ = halves.synthetic_frames("lunar", 40, 0.7, 0.15, 0.6, seed)
    f = frames.astype(float)

    def spread(cols):
        m = f[:, :, cols].mean(axis=(1, 2))
        return m.max() - m.min()

    edge = spread(np.s_[-2:])
    inside = max(spread(np.s_[-2 - k:-k]) for k in (8, 16, 24))
    print(f"\nscene {seed}: last 2 columns' mean spread {edge:.1f} DN over 40 frames, 8-24 px inside {inside:.1f} DN")
    assert edge <= 3 * inside + 2
