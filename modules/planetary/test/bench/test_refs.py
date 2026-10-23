"""Tests for refs.py -- loading, drizzle detection/resampling and registration of references."""
import cv2
import numpy as np
import pytest
import tifffile
from astropy.io import fits
from PIL import Image
from scipy import ndimage

import metric
import refs
from conftest import add_noise, banded_planet, lunar_scene, nas_path, planet_scene


def fourier_shift(img, dy, dx):
    """Exact sub-pixel translation (periodic): out[y, x] = img[y - dy, x - dx]."""
    return np.real(np.fft.ifft2(ndimage.fourier_shift(np.fft.fft2(img), (dy, dx))))


# --- names -----------------------------------------------------------------------------------

@pytest.mark.parametrize("name, factor", [
    ("2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png", 1.0),
    ("2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1_Drizzle30.png", 3.0),
    ("2024-02-01-013847-Moon-Bin1 Lqef 14.9F_lapl5_ap16_Drizzle15.fit", 1.5),
    ("2023-09-29-204604-Saturn-Bin1 LPro 14.9F_Planet_74.fit", 1.0),
])
def test_drizzle_factor_from_name(name, factor):
    assert refs.drizzle_factor(name) == factor


def test_unknown_drizzle_tag_is_refused():
    with pytest.raises(ValueError):
        refs.drizzle_factor("x_lapl5_ap1_Drizzle20.png")


def test_reference_names_parse():
    a = refs.parse_reference_name("/n/2_12_2023/AS_P20/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png")
    assert (a.tool, a.percent, a.drizzle, a.processed) == ("AS", 20, 1.0, False)
    assert a.label == "AS_P20"
    d = refs.parse_reference_name("/n/2_12_2023/AS_P25/2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1_Drizzle30.png")
    assert (d.tool, d.percent, d.drizzle) == ("AS", 25, 3.0)
    assert d.label == "AS_P25 (drizzled x3, resampled)"
    c = refs.parse_reference_name("/n/2_12_2023/AS_P50/m_lapl5_ap2_conv.png")
    assert c.processed
    i = refs.parse_reference_name("/n/9_30_2023/ASIVideoStack_Output/2023-09-29-204604-Saturn-Bin1 LPro 14.9F_Planet_74.fit")
    assert (i.tool, i.percent, i.drizzle, i.processed) == ("ASI", 74, 1.0, False)
    assert i.label == "ASI_74"
    j = refs.parse_reference_name("/n/9_30_2023/ASIVideoStack_Output/x_Planet_74_Sharp37_B45.0_C2.1_S37_N26.jpg")
    assert j.tool == "ASI" and j.processed
    o = refs.parse_reference_name("/tmp/out/stack.fits")
    assert o.tool == "ours" and not o.processed


# --- area resampling -------------------------------------------------------------------------

def test_area_resample_integer_factor_is_the_block_mean():
    rng = np.random.default_rng(3)
    big = rng.random((30, 45))
    small = refs.area_resample(big, 3.0)
    assert small.shape == (10, 15)
    np.testing.assert_allclose(small, big.reshape(10, 3, 15, 3).mean(axis=(1, 3)), rtol=1e-12)


def test_area_resample_fractional_factor_is_exact_area_weighting():
    rng = np.random.default_rng(5)
    big = rng.random((300, 450))
    small = refs.area_resample(big, 1.5)
    assert small.shape == (200, 300)
    # Two x1.5 footprints tile exactly one x3 block, so x1.5 then x2 must equal x3.
    np.testing.assert_allclose(refs.area_resample(small, 2.0), refs.area_resample(big, 3.0), rtol=1e-12)
    # First output pixel by hand: rows/cols 0 (weight 1) and 1 (weight 0.5), normalised by 1.5^2.
    w = np.array([1.0, 0.5])
    assert small[0, 0] == pytest.approx((np.outer(w, w) * big[:2, :2]).sum() / 2.25, rel=1e-12)
    assert small.mean() == pytest.approx(big.mean(), rel=1e-12)      # flux kept (exact tiling)
    np.testing.assert_allclose(refs.area_resample(np.full((30, 45), 7.0), 1.5), 7.0, rtol=1e-12)
    rgb = np.stack([big, 2 * big, 3 * big], axis=-1)
    small_rgb = refs.area_resample(rgb, 1.5)
    assert small_rgb.shape == (200, 300, 3)
    np.testing.assert_allclose(small_rgb[..., 2], 3 * small, rtol=1e-12)


def test_drizzle_then_area_resample_recovers_native():
    """An ideal x3 drizzle (pixfrac 1, each native pixel spread over its 3x3 block) resampled by
    area averaging gives back the native image exactly (Ruling 1)."""
    native = add_noise(planet_scene(), 0.003, 8)
    drizzled = np.kron(native, np.ones((3, 3)))
    np.testing.assert_allclose(refs.area_resample(drizzled, 3.0), native, rtol=1e-12, atol=1e-12)


# --- loaders ---------------------------------------------------------------------------------

def test_loaders_return_float_hwc(tmp_path):
    rng = np.random.default_rng(4)
    rgb16 = rng.integers(0, 65535, (3, 20, 30), dtype=np.uint16)
    fits.PrimaryHDU(rgb16).writeto(tmp_path / "a.fit")
    a = refs.load_image(str(tmp_path / "a.fit"))
    assert a.shape == (20, 30, 3) and a.dtype == np.float64
    np.testing.assert_array_equal(a, np.moveaxis(rgb16, 0, -1).astype(float))

    mono = rng.random((20, 30)).astype(np.float32)
    tifffile.imwrite(tmp_path / "b.tif", mono)
    np.testing.assert_array_equal(refs.load_image(str(tmp_path / "b.tif")), mono.astype(float))

    rgb8 = rng.integers(0, 255, (20, 30, 3), dtype=np.uint8)
    Image.fromarray(rgb8).save(tmp_path / "c.png")
    np.testing.assert_array_equal(refs.load_image(str(tmp_path / "c.png")), rgb8.astype(float))

    m16 = rng.integers(0, 65535, (20, 30), dtype=np.uint16)
    Image.fromarray(m16).save(tmp_path / "d.png")
    np.testing.assert_array_equal(refs.load_image(str(tmp_path / "d.png")), m16.astype(float))

    # AutoStakkert writes 16-bit RGB PNGs; Pillow reads those as 8-bit (it keeps the high byte),
    # so the loader must not use Pillow for PNG.
    rgb16hwc = rng.integers(0, 65535, (20, 30, 3), dtype=np.uint16)
    cv2.imwrite(str(tmp_path / "g.png"), rgb16hwc[..., ::-1])   # OpenCV writes BGR
    np.testing.assert_array_equal(refs.load_image(str(tmp_path / "g.png")), rgb16hwc.astype(float))

    smooth = np.clip(planet_scene(size=(40, 60), radius=15) * 250, 0, 255).astype(np.uint8)
    smooth3 = np.stack([smooth] * 3, axis=-1)
    Image.fromarray(smooth3).save(tmp_path / "e.jpg", quality=95)
    e = refs.load_image(str(tmp_path / "e.jpg"))
    assert e.shape == (40, 60, 3) and np.abs(e - smooth3).mean() < 2

    with pytest.raises(ValueError):
        refs.load_image(str(tmp_path / "f.bmp"))


def test_load_reference_resamples_a_drizzled_file(tmp_path):
    native = planet_scene()
    p = tmp_path / "AS_P25" / "cap_lapl5_ap1_Drizzle30.tif"
    p.parent.mkdir()
    tifffile.imwrite(p, np.kron(native, np.ones((3, 3))).astype(np.float32))
    ref = refs.load_reference(str(p))
    assert ref.factor == 3.0 and ref.image.shape == native.shape
    np.testing.assert_allclose(ref.image, native, rtol=1e-5, atol=1e-6)
    over = refs.load_reference(str(p), factor=1.0)          # explicit factor wins over the name
    assert over.factor == 1.0 and over.image.shape == (3 * native.shape[0], 3 * native.shape[1])


# --- registration ----------------------------------------------------------------------------

@pytest.mark.parametrize("scene", ["planet", "lunar"])
@pytest.mark.parametrize("orientation", ["identity", "flipud", "fliplr", "rot180"])
def test_register_recovers_subpixel_shift_and_orientation(scene, orientation):
    # Shift a larger scene and crop its interior: a Fourier shift of a frame-filling scene
    # would otherwise ring from the wrapped edge.
    big = planet_scene(size=(304, 384), seeing=1.2) if scene == "planet" else lunar_scene(size=(320, 448), seeing=1.2)
    dy, dx = 4.37, -6.81
    inner = (slice(32, -32), slice(32, -32))
    anchor = add_noise(big[inner], 0.003, 21)
    moved = add_noise(fourier_shift(big, -dy, -dx)[inner], 0.003, 22)  # anchor[y,x] = moved[y-dy, x-dx]
    moved = refs.unapply_orientation(moved, orientation)          # as a tool might have written it
    reg = refs.register(moved, anchor)
    assert reg.orientation == orientation
    assert reg.shift == pytest.approx((dy, dx), abs=0.1)
    assert reg.scale == pytest.approx(1.0, abs=0.004)
    assert reg.ncc > 0.9


def test_register_handles_a_cropped_reference_of_another_size():
    anchor = add_noise(planet_scene(size=(300, 400)), 0.003, 31)
    crop = anchor[40:260, 70:350] + 0.0
    reg = refs.register(crop, anchor)
    assert reg.orientation == "identity"
    assert reg.shift == pytest.approx((40.0, 70.0), abs=0.1)
    box = refs.overlap_box(reg, crop.shape, anchor.shape)
    assert tuple(box) == (40, 260, 70, 350)
    np.testing.assert_array_equal(refs.crop_registered(crop, reg, metric.Box(50, 100, 80, 120)),
                                  anchor[50:100, 80:120])


def _tiny_planet(size=(960, 1280), radius=15, seeing=1.0):
    return planet_scene(size=size, radius=radius, seed=9, seeing=seeing)


def test_register_a_tiny_disc_despite_frame_edge_artefacts():
    """The 9_30_2023 Saturn case: a ~30 px disc in a 1280x960 frame, and a reference with a
    bright padding border and an edge strip (ASIStudio / AutoStakkert drizzle edges). Whole
    frame correlation locks onto the edges; the target patch does not."""
    big = _tiny_planet(size=(1024, 1344))
    inner = (slice(32, -32), slice(32, -32))
    anchor = add_noise(big[inner], 0.003, 61)
    moved = add_noise(fourier_shift(big, 12.4, -7.7)[inner], 0.003, 62)
    moved[:40, :] = 0.9
    moved[-40:, :] = 0.9
    moved[:, -12:] = np.random.default_rng(63).random((moved.shape[0], 12))
    reg = refs.register(moved, anchor)
    assert reg.orientation == "identity"
    assert reg.shift == pytest.approx((-12.4, 7.7), abs=0.15)
    assert reg.scale == pytest.approx(1.0, abs=reg.scale_tolerance)


def test_register_scale_is_not_fooled_by_a_sharpness_difference():
    """Our stack, AS and ASI differ in sharpness; blur must not read as a scale change."""
    anchor = add_noise(_tiny_planet(size=(480, 640), radius=18, seeing=0.8), 0.003, 71)
    soft = add_noise(_tiny_planet(size=(480, 640), radius=18, seeing=2.5), 0.003, 72)
    reg = refs.register(soft, anchor)
    assert reg.scale == pytest.approx(1.0, abs=0.02)
    assert reg.shift == pytest.approx((0.0, 0.0), abs=0.15)


def test_scale_tolerance_follows_the_target_size():
    assert refs.scale_tolerance_for(1000) == refs.SCALE_TOLERANCE
    assert refs.scale_tolerance_for(25) == pytest.approx(0.04)


@pytest.mark.parametrize("zoom", [3.0, 1.5, 1.03])
def test_scale_mismatch_is_refused_with_the_measured_factor(zoom):
    anchor = add_noise(planet_scene(size=(240, 320), radius=50), 0.003, 41)
    big = ndimage.zoom(anchor, zoom, order=3)
    with pytest.raises(refs.ScaleMismatch) as e:
        refs.register(big, anchor)
    assert e.value.measured == pytest.approx(zoom, rel=0.01)


@pytest.mark.parametrize("zoom", [3.0, 1.5])
def test_scale_family_of_a_small_featureless_disc(zoom):
    """Venus-like: a 17 px clipped blob, no surface detail, in a 1080x1920 frame, against a
    reference drizzled x`zoom` that was not resampled. The family must still be told apart
    (a wrong drizzle factor must not pass or be misread)."""
    h, w = 1080, 1920
    yy, xx = np.mgrid[0:h, 0:w]
    blob = ((np.hypot(yy - 500.3, xx - 1001.7) < 8.5) * 1.0).astype(float)
    anchor = add_noise(ndimage.gaussian_filter(blob, 1.0), 0.002, 81)
    big = add_noise(ndimage.zoom(ndimage.gaussian_filter(blob, 1.0), zoom, order=1), 0.002, 82)
    with pytest.raises(refs.ScaleMismatch) as e:
        refs.register(big, anchor)
    assert e.value.measured == pytest.approx(zoom, rel=0.06)


def test_compare_measures_everything_on_one_box(tmp_path):
    base = planet_scene(seeing=1.0)
    anchor = add_noise(base, 0.002, 51)
    soft = add_noise(ndimage.gaussian_filter(np.roll(base, (3, -2), axis=(0, 1)), 1.0), 0.002, 52)
    p = tmp_path / "AS_P50"
    p.mkdir()
    tifffile.imwrite(p / "cap_lapl5_ap1.tif", np.flipud(soft).astype(np.float32))
    rows = refs.compare(anchor, [str(p / "cap_lapl5_ap1.tif")])
    assert [r.label for r in rows] == ["ours", "AS_P50"]
    assert rows[0].result.box == rows[1].result.box
    assert rows[1].registration.orientation == "flipud"
    assert rows[1].result.value < 0.9 * rows[0].result.value


def test_compare_refuses_a_processed_reference(tmp_path):
    p = tmp_path / "ASIVideoStack_Output"
    p.mkdir()
    Image.fromarray((planet_scene() * 200).astype(np.uint8)).save(p / "x_Planet_50_Sharp20_B0.0_C1.0_S1_N1.jpg")
    with pytest.raises(ValueError, match="processed"):
        refs.compare(planet_scene(), [str(p / "x_Planet_50_Sharp20_B0.0_C1.0_S1_N1.jpg")])


# --- real data (NAS, read-only) --------------------------------------------------------------

@pytest.mark.nas
def test_real_drizzled_as_registers_to_native_as():
    """2_12_2023 Jupiter: AS_P25 is drizzled x3; after area resampling it must sit on AS_P20's
    native grid at scale 1 (the scale check measures it, it is not assumed)."""
    native = refs.load_reference(nas_path("2_12_2023", "AS_P20", "2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1.png"))
    driz = refs.load_reference(nas_path("2_12_2023", "AS_P25", "2023-02-11-191831-Jupiter-Bin1 13.3F_lapl5_ap1_Drizzle30.png"))
    assert driz.factor == 3.0 and driz.image.shape == (480, 640, 3)
    reg = refs.register(metric.luminance(driz.image), metric.luminance(native.image))
    print(f"\nJupiter AS_P25->AS_P20: {reg}")
    assert reg.scale == pytest.approx(1.0, abs=0.01)
    assert reg.ncc > 0.9


@pytest.mark.nas
def test_real_size_derived_factor_overrules_a_wrong_drizzle_name():
    """2_1_2024 Moon: the AutoStakkert .fit is named Drizzle15 but is 5688x3168 for a 1920x1080
    capture (2.96 / 2.93 -> x3). The factor comes from the size, the label records the
    disagreement, and the result registers to ASIStudio's native .fit at scale 1."""
    asi = refs.load_reference(nas_path("2_1_2024", "ASIVideoStack_Output", "2024-02-01-013847-Moon-Bin1 Lqef 14.9F_Surface_40.fit"),
                              frame_size=(1920, 1080))
    as_ = refs.load_reference(nas_path("2_1_2024", "AS_P60", "2024-02-01-013847-Moon-Bin1 Lqef 14.9F_lapl5_ap16_Drizzle15.fit"),
                              frame_size=(1920, 1080))
    assert asi.factor == 1.0 and as_.factor == 3.0
    assert "named x1.5, size x3" in as_.info.label
    reg = refs.register(metric.luminance(as_.image), metric.luminance(asi.image))
    print(f"\n2_1_2024 AS (size x3) -> ASI: {reg}")
    assert reg.scale == pytest.approx(1.0, abs=0.01) and reg.ncc > 0.8


@pytest.mark.parametrize("ref_size, frame, factor", [
    ((5688, 3168), (1920, 1080), 3.0), ((2916, 1668), (1920, 1080), 1.5), ((640, 480), (640, 480), 1.0),
    ((1896, 1440), (640, 480), 3.0)])
def test_factor_from_size(ref_size, frame, factor):
    assert refs.factor_from_size(ref_size, frame) == factor


def test_factor_from_size_refuses_an_unknown_scale():
    with pytest.raises(ValueError):
        refs.factor_from_size((2560, 1440), (1920, 1080))     # x1.33: no drizzle factor


def test_size_and_name_factors(tmp_path):
    native = planet_scene()
    p = tmp_path / "AS_P60"
    p.mkdir()
    tifffile.imwrite(p / "cap_lapl5_ap1_Drizzle15.tif", np.kron(native, np.ones((3, 3))).astype(np.float32))
    ref = refs.load_reference(str(p / "cap_lapl5_ap1_Drizzle15.tif"), frame_size=(320, 240))
    assert ref.factor == 3.0 and ref.image.shape == native.shape
    assert ref.info.label == "AS_P60 (named x1.5, size x3, resampled)"
    tifffile.imwrite(p / "ok_lapl5_ap1_Drizzle30.tif", np.kron(native, np.ones((3, 3))).astype(np.float32))
    ok = refs.load_reference(str(p / "ok_lapl5_ap1_Drizzle30.tif"), frame_size=(320, 240))
    assert ok.info.label == "AS_P60 (drizzled x3, resampled)"


def test_compare_reports_the_interior_score_on_a_large_disc(tmp_path):
    sharp = add_noise(banded_planet(15), 0.003, 1)
    soft = add_noise(banded_planet(15, interior_blur=1.0), 0.003, 2)
    p = tmp_path / "AS_P20"
    p.mkdir()
    tifffile.imwrite(p / "cap_lapl5_ap1.tif", soft.astype(np.float32))
    rows = refs.compare(sharp, [str(p / "cap_lapl5_ap1.tif")])
    d = refs.rows_to_dicts(rows)
    assert d[0]["interior_judged"] and d[0]["dan_interior"] > 0
    assert d[1]["dan_interior"] < 0.97 * d[0]["dan_interior"]
    assert d[0]["review"] == []


def test_a_softer_ours_cannot_escape_the_interior_bar(tmp_path):
    """The interior window is a property of the capture, not of whoever is anchor: its erosion
    comes from the sharpest limb among ours and the references. A softened ours (seeing 2.5 vs
    1.0, R 13) eroded by its own limb measures 19.7 px, under 20, and would escape the bar."""
    sharp = add_noise(banded_planet(13, seeing=1.0), 0.003, 1)
    soft = add_noise(banded_planet(13, seeing=2.5), 0.003, 2)
    own = metric.interior_window(soft, metric.target_box(soft))
    assert not own.judged                                   # the escape this test guards
    p = tmp_path / "AS_P20"
    p.mkdir()
    tifffile.imwrite(p / "cap_lapl5_ap1.tif", sharp.astype(np.float32))
    d = refs.rows_to_dicts(refs.compare(soft, [str(p / "cap_lapl5_ap1.tif")]))
    print(f"\nown window {own.diameter:.1f} px (limb {own.limb_sigma:.2f}); capture window "
          f"{d[0]['interior_diameter']:.1f} px; interior ratio {d[0]['interior_ratio_to_best_reference']}")
    assert d[0]["interior_judged"] and d[0]["dan_bar"]["interior"] is False   # the window rule, on DAN (no frame noise here)


def test_compare_marks_a_small_disc_limb_only(tmp_path):
    p = tmp_path / "AS_P20"
    p.mkdir()
    tifffile.imwrite(p / "cap_lapl5_ap1.tif", add_noise(banded_planet(7), 0.003, 2).astype(np.float32))
    d = refs.rows_to_dicts(refs.compare(add_noise(banded_planet(7), 0.003, 1), [str(p / "cap_lapl5_ap1.tif")]))
    assert d[0]["interior_judged"] is False and d[0]["dan_interior"] is None
    assert any(n.startswith("limb-only: human review decides") for n in d[0]["review"])


def test_mosaic_fallback_carries_a_bias_warning(tmp_path):
    p = tmp_path / "AS_P20"
    p.mkdir()
    img = add_noise(lunar_scene(), 0.002, 3)
    tifffile.imwrite(p / "cap_lapl5_ap1.tif", img.astype(np.float32))
    d = refs.rows_to_dicts(refs.compare(img, [str(p / "cap_lapl5_ap1.tif")], mosaic=True))
    assert any(w.startswith("mosaic mode") and "not a pass/fail" in w for w in d[0]["review"])


def test_out_of_family_noise_regime_is_flagged(tmp_path):
    """Not a sharpening detector (measured: unsharp masking leaves floor/band where it was) --
    a stack whose noise regime is unlike every reference's goes to human review."""
    base = banded_planet(15)
    p = tmp_path / "AS_P20"
    p.mkdir()
    tifffile.imwrite(p / "cap_lapl5_ap1.tif", add_noise(base, 0.001, 2).astype(np.float32))
    d = refs.rows_to_dicts(refs.compare(add_noise(base, 0.05, 1), [str(p / "cap_lapl5_ap1.tif")]))
    assert any("noise regime" in w for w in d[0]["review"])


def test_compare_runs_on_a_sharp_tiny_disc(tmp_path):
    """Task 9's Mars: our sharper stack of a 13 px disc gave a box under the 32 px floor and
    compare raised 'common area too small' (the references cover the whole frame)."""
    p = tmp_path / "AS_P25"
    p.mkdir()
    ref = add_noise(planet_scene(size=(240, 320), radius=6, seeing=1.6), 0.002, 82)
    tifffile.imwrite(p / "mars_lapl5_ap1.tif", ref.astype(np.float32))
    ours = add_noise(planet_scene(size=(240, 320), radius=6, seeing=0.8), 0.002, 81)
    rows = refs.compare(ours, [str(p / "mars_lapl5_ap1.tif")])
    box = rows[0].result.box
    assert box.y1 - box.y0 >= metric.MIN_BOX_SIDE and box.x1 - box.x0 >= metric.MIN_BOX_SIDE
    assert rows[0].result.value > rows[1].result.value      # the sharper disc still ranks first
