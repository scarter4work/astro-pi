"""Validation of metric.py -- the benchmark's sharpness-at-equal-noise measure.

What the spec asks the metric to prove before it judges anything (Task 9):
  * a blurred copy ranks below the original;
  * added noise does not raise it;
  * on the 2_12_2023 capture, AS_P20 against AutoStakkert's results from a larger share of the
    frames ranks the way AutoStakkert's own selection implies (fewer, sharper frames -> more detail).
DAN is now the bench's SECONDARY figure (Ruling P28); these tests keep pinning its behaviour. The
judged metric, END, is validated in test_end.py; the 2_12_2023 P20/P50 ordering became a reported
figure there and here (the pair is confounded: ap1 against ap2).
Run with -s to see the measured numbers (they are recorded in README.md).
"""
import numpy as np
import pytest
from scipy import ndimage

import metric
import refs
from conftest import add_noise, banded_planet, lunar_scene, nas_path, planet_scene, unsharp

BLUR_SIGMAS = (0.5, 1.0, 2.0)


def _blur_series(img):
    return [metric.measure(img).value] + [
        metric.measure(ndimage.gaussian_filter(img, s)).value for s in BLUR_SIGMAS]


def _assert_strictly_decreasing(values):
    for a, b in zip(values, values[1:]):
        assert b < a, values


# --- synthetic -------------------------------------------------------------------------------

@pytest.mark.parametrize("scene", ["planet", "lunar"])
def test_blurred_copy_ranks_below_original(scene, request):
    img = request.getfixturevalue(scene)
    values = _blur_series(img)
    print(f"\n{scene} blur sigma 0/{'/'.join(map(str, BLUR_SIGMAS))}: "
          + " ".join(f"{v:.5f}" for v in values))
    _assert_strictly_decreasing(values)
    # A clear margin, not a rounding-level ordering: sigma 1 extra blur costs at least 10 %
    # (measured 16-17 %; the seed-to-seed scatter of the measure at heavy noise is 1-3 %).
    assert values[2] < 0.9 * values[0]


@pytest.mark.parametrize("scene", ["planet", "lunar"])
@pytest.mark.parametrize("sigma", [0.005, 0.01, 0.02])
def test_added_noise_does_not_raise_it(scene, sigma, request):
    img = request.getfixturevalue(scene)
    base = metric.measure(img)
    noisy = metric.measure(add_noise(img, sigma, 99))
    print(f"\n{scene} noise {sigma}: {base.value:.5f} -> {noisy.value:.5f} "
          f"(band SNR {base.band_snr:.1f} -> {noisy.band_snr:.2f})")
    assert noisy.value <= 1.02 * base.value
    # ...and it is a noise-corrected measure, not a noise penalty: the detail is still found.
    assert noisy.value >= 0.95 * base.value


def test_noise_cannot_buy_back_blur(planet):
    """A blurred copy plus noise must not climb back above the original (a plain gradient or
    Laplacian energy would: noise is high-frequency power)."""
    box = metric.target_box(planet)
    soft_noisy = add_noise(ndimage.gaussian_filter(planet, 1.0), 0.02, 5)
    assert metric.measure(soft_noisy, box).value < 0.9 * metric.measure(planet, box).value


def test_gain_and_offset_do_not_change_it(planet, lunar):
    for img in (planet, lunar):
        a = metric.measure(img).value
        b = metric.measure(3.7 * img + 0.25).value
        assert b == pytest.approx(a, rel=1e-9)


def test_target_box_planet_holds_the_disc_and_leaves_out_the_moon():
    img = planet_scene()
    y0, y1, x0, x1 = metric.target_box(img)
    h, w = img.shape
    cy, cx = h / 2 + 3.3, w / 2 - 7.6
    assert y0 <= cy - 70 and y1 >= cy + 70 and x0 <= cx - 70 and x1 >= cx + 70
    assert not (y0 <= 25 < y1 and x0 <= 25 < x1), (y0, y1, x0, x1)
    assert 0 <= y0 < y1 <= h and 0 <= x0 < x1 <= w


def test_target_ignores_a_bright_strip_on_the_frame_edge():
    """AutoStakkert's drizzled Mars (2_12_2023 AS_P25) carries a bright 11 px strip down its
    right edge that is larger than the 11 px disc; a thin edge-touching component is an
    artefact, not the target."""
    img = planet_scene(size=(240, 320), radius=6)
    img[30:200, -15:-3] = 0.95              # measured: cols 305-317, inside a 2 px black border
    img[:, -2:] = 0.0
    t = metric.target_extent(img)
    cy, cx = 120 + 3.3, 160 - 7.6
    assert t.y0 <= cy <= t.y1 and t.x0 <= cx <= t.x1, t


def test_a_large_target_on_the_frame_edge_is_kept():
    img = lunar_scene()
    img[:, :60] = 0.02                      # a lunar limb: sky on the left, surface to the edges
    t = metric.target_extent(img)
    assert t.x0 >= 55 and (t.x1 - t.x0) > 0.7 * img.shape[1] and (t.y1 - t.y0) > 0.9 * img.shape[0]


def test_target_box_lunar_fills_the_frame():
    img = lunar_scene()
    y0, y1, x0, x1 = metric.target_box(img)
    h, w = img.shape
    assert (y1 - y0) >= 0.9 * h and (x1 - x0) >= 0.9 * w


def test_explicit_box_is_used(planet):
    box = metric.Box(40, 200, 60, 260)
    r = metric.measure(planet, box)
    assert r.box == box


def test_rgb_input_is_measured_on_its_luminance(planet):
    rgb = np.stack([planet * 0.9, planet, planet * 1.1], axis=-1)
    assert metric.measure(rgb).value == pytest.approx(metric.measure(planet).value, rel=1e-9)


@pytest.mark.parametrize("seeing", [0.5, 1.2])
def test_mosaic_mode_removes_the_cfa_checkerboard(seeing):
    """An undeclared Bayer capture stacked as mono carries the CFA's per-site gains as a
    checkerboard. Its power sits at Nyquist (outside the band), but it inflates the noise
    floor; mosaic mode equalises the four sites first, and then the modulated scene measures
    as the unmodulated one -- floor included -- while a scene without a CFA is left alone."""
    img = add_noise(lunar_scene(seeing=seeing), 0.002, 4)
    gains = np.empty_like(img)
    gains[0::2, 0::2], gains[0::2, 1::2], gains[1::2, 0::2], gains[1::2, 1::2] = 0.6, 1.0, 1.0, 0.45
    cfa = img * gains
    box = metric.target_box(img)
    plain, raw = metric.measure(img, box), metric.measure(cfa, box)
    flat = metric.measure(cfa, box, mosaic=True)
    assert raw.noise_floor > 1.5 * plain.noise_floor
    assert flat.value == pytest.approx(plain.value, rel=0.005)
    assert flat.noise_floor == pytest.approx(plain.noise_floor, rel=0.05)
    assert metric.measure(img, box, mosaic=True).value == pytest.approx(plain.value, rel=0.002)


def test_featureless_image_is_refused_loudly():
    with pytest.raises(ValueError):
        metric.measure(np.full((128, 128), 0.3))


# --- interior score (review round 1, Important 2) -----------------------------------------------

def _interior_pair(R, **kw):
    """(sharp, other) measured on the sharp image's box and interior window, noise 0.003."""
    sharp = banded_planet(R)
    other = banded_planet(R, **kw)
    box = metric.target_box(sharp)
    win = metric.interior_window(sharp, box)
    m = lambda img, s: metric.measure(add_noise(img, 0.003, s), box, interior=win)   # noqa: E731
    return m(sharp, 5), m(other, 6), win


def test_interior_smeared_with_a_crisp_limb_fails_the_interior_score():
    """The review's case: surface detail smeared sigma 1, limb kept crisp. DAN_full is limb-
    weighted and barely moves; DAN_interior must drop below the 0.97 bar."""
    sharp, smeared, win = _interior_pair(15, interior_blur=1.0)
    full, interior = smeared.value / sharp.value, smeared.interior / sharp.interior
    print(f"\nR15 interior sigma 1: full {full:.3f}, interior {interior:.3f}, eroded disc "
          f"{win.diameter:.1f} px (erosion {win.erosion:.1f}, limb sigma {win.limb_sigma:.2f})")
    assert win.diameter >= metric.INTERIOR_MIN_DIAMETER
    assert interior < 0.97
    assert interior < full


def test_interior_score_falls_with_global_blur_and_ignores_added_noise():
    sharp = banded_planet(15)
    box = metric.target_box(sharp)
    win = metric.interior_window(sharp, box)
    base = metric.measure(add_noise(sharp, 0.003, 1), box, interior=win).interior
    soft = metric.measure(add_noise(ndimage.gaussian_filter(sharp, 1.0), 0.003, 2), box, interior=win).interior
    noisy = metric.measure(add_noise(sharp, 0.01, 3), box, interior=win).interior
    print(f"\ninterior: base {base:.4f}, blur 1 {soft:.4f}, noise 0.01 {noisy:.4f}")
    assert soft < 0.9 * base
    assert noisy <= 1.05 * base


def test_limb_sigma_estimate():
    for seeing in (1.0, 2.0):
        img = banded_planet(20, seeing=seeing, size=(200, 240))
        win = metric.interior_window(img, metric.target_box(img))
        assert win.limb_sigma == pytest.approx(seeing, rel=0.4), win


def test_a_small_disc_is_limb_only():
    img = banded_planet(7)
    win = metric.interior_window(img, metric.target_box(img))
    assert win.diameter < metric.INTERIOR_MIN_DIAMETER
    r = metric.measure(img, metric.target_box(img), interior=win)
    assert r.interior is None and r.interior_judged is False


# --- known limit: sharpening (review round 1, Important 3) -------------------------------------

@pytest.mark.parametrize("R", [7, 15])
def test_known_limit_unsharp_masking_buys_dan(R):
    """KNOWN LIMIT, pinned: a disc softened sigma 1.6 and then unsharp-masked (sigma 1.5,
    amount 1) scores at least the 0.97 bar against the genuinely sharp disc. DAN is valid only
    between unsharpened linear stacks (metric.py LIMITS); this test fails if that changes, so
    the README and LIMITS get revisited."""
    sharp = banded_planet(R)
    box = metric.target_box(sharp)
    a = metric.measure(add_noise(sharp, 0.003, 5), box).value
    b = metric.measure(add_noise(unsharp(banded_planet(R, seeing=1.6)), 0.003, 5), box).value
    print(f"\nR{R}: soft+USM / sharp = {b / a:.3f}")
    assert b / a >= 0.97


# --- real data (NAS, read-only) --------------------------------------------------------------

AS_P20_JUPITER = ("2_12_2023", "AS_P20", "2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png")
AS_P20_MOON = ("2_12_2023", "AS_P20", "2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap1.png")
AS_P50_MOON = ("2_12_2023", "AS_P50", "2023-02-12-024708-Moon-Bin1 14.9F_lapl5_ap2.png")


@pytest.mark.nas
@pytest.mark.parametrize("parts", [AS_P20_JUPITER, AS_P20_MOON], ids=["jupiter", "moon"])
def test_real_blurred_copy_ranks_below(parts):
    lum = metric.luminance(refs.load_image(nas_path(*parts)))
    box = metric.target_box(lum)
    values = [metric.measure(lum, box).value] + [
        metric.measure(ndimage.gaussian_filter(lum, s), box).value for s in BLUR_SIGMAS]
    print(f"\n{parts[2]} blur 0/{'/'.join(map(str, BLUR_SIGMAS))}: "
          + " ".join(f"{v:.5f}" for v in values))
    _assert_strictly_decreasing(values)


@pytest.mark.nas
@pytest.mark.parametrize("parts", [AS_P20_JUPITER, AS_P20_MOON], ids=["jupiter", "moon"])
@pytest.mark.parametrize("sigma", [0.005, 0.01, 0.02])  # as a fraction of the mean target signal
def test_real_added_noise_does_not_raise_it(parts, sigma):
    lum = metric.luminance(refs.load_image(nas_path(*parts)))
    box = metric.target_box(lum)
    base = metric.measure(lum, box)
    noisy = metric.measure(add_noise(lum, sigma * base.signal, 7), box)
    print(f"\n{parts[2]} noise {sigma} of signal: {base.value:.5f} -> {noisy.value:.5f} "
          f"(band SNR {base.band_snr:.1f} -> {noisy.band_snr:.2f})")
    assert noisy.value <= 1.02 * base.value


@pytest.mark.nas
@pytest.mark.ffmpeg
def test_real_2_12_2023_moon_p20_p50_pair_is_a_reported_figure():
    """REPORTED, NOT ASSERTED (Ruling P28, rule 4). The plan asked that AS_P20 rank above AS_P50 on
    the 2_12_2023 Moon "as AutoStakkert's selection implies". The pair is confounded: P20 is
    lapl5_ap1, P50 lapl5_ap2 (different alignment-point settings), so it does not isolate the share
    of frames. The ordering check moved to an unconfounded synthetic (test_end.py, a 20 % and a 50 %
    stack of one capture). This test records both metrics on the real pair -- measured exactly as the
    bench measures the capture, AS_P20 standing in for our stack with its own frame count -- and
    asserts only that both are measurable. Numbers in README.md."""
    import benchset
    c = benchset.by_id("2023-02-12-moon")
    probe = {"used": 0.20 * c.frames, "regionDetail": []}           # AS_P20's k: 20 % of the frames
    rows = benchset.compare_capture("2023-02-12-moon", nas_path(*AS_P20_MOON), probe=probe)
    by = {r.label: r.result for r in rows}
    for r in rows:
        print(f"\n{r.label}: END {r.result.end} (k {r.result.end_frames}), DAN {r.result.value:.5f}, "
              f"floor {r.result.noise_floor:.3g}, note {r.result.end_note}")
    for label in ("AS_P20", "AS_P50"):
        assert by[label].end is not None and by[label].value > 0


def test_target_box_of_a_tiny_disc_is_at_least_the_minimum_side():
    """The 2_12_2023 Mars case (Task 9's run): a sharp 13 px disc plus the 8 px margin gives a
    29 px box, under refs.compare's 32 px floor, so the comparison could not run at all. The box
    grows about the disc to MIN_BOX_SIDE (sky only: the same for every image compared)."""
    img = planet_scene(size=(240, 320), radius=6)
    y0, y1, x0, x1 = metric.target_box(img)
    assert y1 - y0 >= metric.MIN_BOX_SIDE and x1 - x0 >= metric.MIN_BOX_SIDE
    cy, cx = 120 + 3.3, 160 - 7.6
    assert abs((y0 + y1) / 2 - cy) <= 1.5 and abs((x0 + x1) / 2 - cx) <= 1.5


def test_a_tiny_disc_near_the_frame_edge_keeps_a_minimum_box_inside_the_frame():
    img = np.roll(planet_scene(size=(240, 320), radius=6), (-100, 140), axis=(0, 1))
    y0, y1, x0, x1 = metric.target_box(img)
    assert y1 - y0 >= metric.MIN_BOX_SIDE and x1 - x0 >= metric.MIN_BOX_SIDE
    assert y0 >= 4 and x1 <= 320 - 4
