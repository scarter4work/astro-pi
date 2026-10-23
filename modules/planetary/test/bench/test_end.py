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
    Measured: a +60 % here (a 3 px disc is all limb, whose samples move between frames; +16 % at 6 px):
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


@pytest.mark.skipif(not all(os.access(p, os.X_OK) for p in PROBES), reason="stack-probe / stack-region-probe not built")
def test_region_probe_box_k_matches_analytic_layouts():
    """stack-region-probe --keff-selftest: BoxKeff on hand-built plans -- one region keeping 37 of 100
    (k 37), a region keeping none (the fallback, k 100), two blended nodes with nested kept sets
    (against per-pixel weights written out from the layout's geometry, not the engine's AxisWeightAt;
    it checks BoxKeff's sums over the engine's blend rule, not the rule; reviews M2 / N-m6 / N2-m8)."""
    import subprocess
    r = subprocess.run([PROBES[1], "--keff-selftest"], capture_output=True, text=True)
    print("\n" + r.stdout)
    assert r.returncode == 0 and r.stdout.count(": ok") == 3


@pytest.mark.skipif(not all(os.access(p, os.X_OK) for p in PROBES), reason="stack-probe / stack-region-probe not built")
@pytest.mark.parametrize("kind, jitter, seed, n", [("disc", 0.5, 7, 400), ("lunar", 0.6, 10, 600)])
def test_end_to_end_noise_model_through_the_engine(tmp_path, kind, jitter, seed, n):
    """Synthetic RGGB frames of known noise (a m + c) -> noise.photon_transfer -> the engine stacks the
    even and odd halves -> stack-region-probe's noise-power k over A's box (checked against
    stack-probe's plan) -> noise.box_noise -> the halves' difference against sigma_frame^2 (1/k_A +
    1/k_B) / 4, with the MEASURED photon-transfer line. It must lie within the kind's frozen tolerance
    and within 30 % of the kind's calibrated ratio. Both cases are held out of the calibration (scenes 7
    and 10, outside halves.CAL_SEEDS, at jitters it does not use), so the 30 % check is not circular
    (review N2-m4). The lunar case is one where the engine's regions keep few frames over the box (k 18
    and 16 of 300): there the half's frame count as k misses the model by about 17 x, so the test
    discriminates the k (review M2). (The box k and the mean-kept reading agree on these synthetics,
    whose regions are alike, so they are not told apart here.)"""
    import halves
    import json as _json
    assert seed not in halves.CAL_SEEDS
    assert f"{kind}-j{jitter}-s{seed}" not in {r["condition"] for r in halves.load_calibration()["kinds"][kind]["runs"]}
    frames, mean, truth = halves.synthetic_frames(kind, n, jitter, 0.15, 0.6, seed)
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
          f"(calibrated {cal['high_mean']:.3f}); with the half's {n // 2} frames as k x{naive['high']:.3f}")
    assert lo <= r["high"] <= hi
    assert r["high"] == pytest.approx(cal["high_mean"], rel=0.30)
    if kind == "lunar":
        assert not naive["high"] == pytest.approx(cal["high_mean"], rel=0.30)


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
