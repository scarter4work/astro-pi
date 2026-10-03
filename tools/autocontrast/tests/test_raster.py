"""Tests for the raster loader (PNG/JPEG/TIFF professional renders, §2.1)."""

import numpy as np
import pytest
from PIL import Image

from autocontrast.io.loaders import load_image, load_raster


def _save(path, array):
    Image.fromarray(array).save(path)


def test_loads_rgb_as_float_in_unit_range(tmp_path):
    path = tmp_path / "img.png"
    arr = np.zeros((6, 8, 3), dtype=np.uint8)
    arr[0, 0] = [255, 0, 0]
    _save(path, arr)

    img = load_raster(path)

    assert img.dtype == np.float64
    assert img.shape == (6, 8, 3)
    assert img.min() >= 0.0 and img.max() <= 1.0
    np.testing.assert_allclose(img[0, 0], [1.0, 0.0, 0.0])


def test_grayscale_is_promoted_to_three_channels(tmp_path):
    path = tmp_path / "gray.png"
    _save(path, np.full((4, 4), 128, dtype=np.uint8))

    img = load_raster(path)

    assert img.shape == (4, 4, 3)
    np.testing.assert_allclose(img[..., 0], img[..., 1])
    np.testing.assert_allclose(img[..., 1], img[..., 2])


def test_alpha_channel_is_dropped(tmp_path):
    path = tmp_path / "rgba.png"
    arr = np.zeros((4, 4, 4), dtype=np.uint8)
    arr[..., 3] = 255
    _save(path, arr)

    img = load_raster(path)

    assert img.shape == (4, 4, 3)


def test_max_dim_downsizes_longest_side_preserving_aspect(tmp_path):
    path = tmp_path / "big.png"
    _save(path, np.zeros((100, 200, 3), dtype=np.uint8))  # 200 wide, 100 tall

    img = load_raster(path, max_dim=50)

    assert max(img.shape[:2]) == 50
    assert img.shape == (25, 50, 3)  # aspect preserved


def test_max_dim_does_not_upscale_small_images(tmp_path):
    path = tmp_path / "small.png"
    _save(path, np.zeros((10, 20, 3), dtype=np.uint8))

    img = load_raster(path, max_dim=1000)

    assert img.shape == (10, 20, 3)  # unchanged


def test_sixteen_bit_is_normalized_by_full_range(tmp_path):
    # Pillow can't build a 16-bit *RGB* array via fromarray, but 16-bit grayscale
    # exercises the same /65535 normalization path (and real 16-bit TIFFs load fine).
    path = tmp_path / "hi.tiff"
    arr = np.zeros((4, 4), dtype=np.int32)  # mode 'I' (32-bit), inferred without a mode= arg
    arr[0, 0] = 65535
    Image.fromarray(arr).save(path)

    img = load_raster(path)

    assert img.shape == (4, 4, 3)
    assert img[0, 0, 0] == pytest.approx(1.0)
    assert img.max() <= 1.0


# ---- 16-bit TIFF: the optimizer's candidate format (§2.2) -----------------
#
# PixInsight writes each candidate to disk and the sidecar reads it back to
# score it. If that round trip quantizes, the whole production search runs
# against an approximation. These tests are the proof that it does not — and
# the second one is the proof that the FIRST one is actually testing something,
# because Pillow reads the very same file without raising and hands back 8 bits.


def _write_16bit_rgb_tiff(path, arr):
    import tifffile

    tifffile.imwrite(path, arr)


def test_sixteen_bit_rgb_tiff_round_trips_without_losing_the_low_byte(tmp_path):
    path = tmp_path / "cand.tif"
    arr = np.zeros((4, 6, 3), dtype=np.uint16)
    # Values chosen so 8-bit truncation is unmistakable: 40000 >> 8 == 156, and
    # 257 >> 8 == 1, so a truncating reader lands nowhere near these ratios.
    arr[1, 1] = (65535, 40000, 257)
    _write_16bit_rgb_tiff(path, arr)

    img = load_raster(path)

    assert img.shape == (4, 6, 3)
    np.testing.assert_allclose(img[1, 1], [65535 / 65535, 40000 / 65535, 257 / 65535],
                               rtol=0, atol=1e-12)
    # 16 bits of resolution actually present: the smallest step is 1/65535, and
    # an 8-bit intermediate could not represent 257/65535 at all.
    assert img[1, 1, 2] * 65535 == pytest.approx(257.0)


def test_pillow_would_have_truncated_that_file(tmp_path):
    """Guards the test above against becoming vacuous.

    If Pillow ever grew real 48-bit RGB support this would fail, and the
    dedicated `tifffile` decode could be reconsidered. Until then this records
    WHY `_TIFF_SUFFIXES` bypasses Pillow: not that Pillow refuses the file, but
    that it accepts it and quietly drops the low byte.
    """
    path = tmp_path / "cand.tif"
    arr = np.zeros((4, 6, 3), dtype=np.uint16)
    arr[1, 1] = (65535, 40000, 257)
    _write_16bit_rgb_tiff(path, arr)

    with Image.open(path) as im:
        pillow_arr = np.asarray(im)

    assert pillow_arr.dtype == np.uint8
    np.testing.assert_array_equal(pillow_arr[1, 1], [255, 156, 1])


def test_load_image_reads_a_sixteen_bit_tiff_at_full_depth(tmp_path):
    """`load_image` is what the optimizer session actually calls."""
    path = tmp_path / "cand.tiff"
    arr = np.full((4, 4, 3), 513, dtype=np.uint16)  # 513 >> 8 == 2
    _write_16bit_rgb_tiff(path, arr)

    img = load_image(path)

    assert img[0, 0, 0] * 65535 == pytest.approx(513.0)


def test_sixteen_bit_tiff_survives_max_dim_downsizing(tmp_path):
    """The optimizer reads every candidate at `max_dim`, so the resize is on the
    round trip too — and Pillow has no RGB mode that holds uint16 to resize in."""
    path = tmp_path / "big.tif"
    arr = np.full((100, 200, 3), 40000, dtype=np.uint16)
    _write_16bit_rgb_tiff(path, arr)

    img = load_raster(path, max_dim=50)

    assert img.shape == (25, 50, 3)  # aspect preserved
    # A flat field must resample to itself; 40000/65535 is not representable at
    # 8 bits (it would land on 156/255 = 0.6118).
    np.testing.assert_allclose(img, 40000 / 65535, rtol=0, atol=1e-6)


def test_a_dark_sixteen_bit_tiff_is_not_rescaled_as_if_it_were_eight_bit(tmp_path):
    """The bit depth comes from the DECODER, never from the pixel values.

    A candidate whose brightest pixel is 200 is a legitimate 16-bit image of a
    dark field. Inferring the range from the data sees `max <= 255`, divides by
    255, and returns 0.784 for a pixel whose true value is 0.00305 — 257x too
    bright, with nothing logged and nothing raised (§12). Rare on astro data,
    where the max is normally near full scale, but silent when it happens, and
    16-bit TIFF is now the format every candidate round-trips through.
    """
    path = tmp_path / "dark.tif"
    _write_16bit_rgb_tiff(path, np.full((4, 4, 3), 200, dtype=np.uint16))

    img = load_raster(path)

    np.testing.assert_allclose(img, 200 / 65535, rtol=0, atol=1e-12)
    assert img.max() < 0.01  # not 0.784: the file is dark and must load dark


def test_a_dark_sixteen_bit_tiff_survives_the_downsizing_path_too(tmp_path):
    """`_resize_native` returns float32, so the dtype is gone by the time the
    normalization runs — the full-scale value has to be carried, not re-derived."""
    path = tmp_path / "dark-big.tif"
    _write_16bit_rgb_tiff(path, np.full((100, 200, 3), 200, dtype=np.uint16))

    img = load_raster(path, max_dim=50)

    assert img.shape == (25, 50, 3)
    np.testing.assert_allclose(img, 200 / 65535, rtol=0, atol=1e-6)


def test_supported_suffixes_includes_tiff(tmp_path):
    """`optimize_begin` validates `candidate_suffix` against this set."""
    from autocontrast.io.loaders import supported_suffixes

    assert {".tif", ".tiff"} <= supported_suffixes()


def test_tiff_image_dimensions_are_native(tmp_path):
    """`downsample_factor` divides by these, so they must be the file's own."""
    from autocontrast.io.loaders import downsample_factor, image_dimensions

    path = tmp_path / "dims.tif"
    _write_16bit_rgb_tiff(path, np.zeros((100, 200, 3), dtype=np.uint16))

    assert image_dimensions(path) == (200, 100)
    assert downsample_factor(path, load_raster(path, max_dim=50)) == pytest.approx(4.0)


# ---- load_image dispatcher: FITS must go through astropy, not Pillow ------


def test_load_image_dispatches_raster_to_pillow(tmp_path):
    path = tmp_path / "r.png"
    arr = np.zeros((6, 8, 3), dtype=np.uint8); arr[0, 0] = [255, 0, 0]
    _save(path, arr)

    img = load_image(path)

    assert img.shape == (6, 8, 3)
    np.testing.assert_allclose(img[0, 0], [1.0, 0.0, 0.0])


def test_load_image_reads_fits_via_astropy_without_nans(tmp_path):
    """A float FITS must not be routed through Pillow — that yields NaNs and a
    garbage fingerprint. It loads via astropy, promoted to 3 channels in [0,1]."""
    from astropy.io import fits

    data = np.linspace(-5.0, 500.0, 32 * 32, dtype=np.float32).reshape(32, 32)
    path = tmp_path / "lin.fits"
    fits.PrimaryHDU(data=data).writeto(path)

    img = load_image(path)

    assert img.shape == (32, 32, 3)
    assert np.isfinite(img).all()
    assert img.min() >= 0.0 and img.max() <= 1.0


def test_load_image_fits_is_nan_safe(tmp_path):
    from astropy.io import fits

    data = np.ones((16, 16), dtype=np.float32)
    data[0, 0] = np.nan
    path = tmp_path / "nan.fits"
    fits.PrimaryHDU(data=data).writeto(path)

    img = load_image(path)

    assert np.isfinite(img).all()
