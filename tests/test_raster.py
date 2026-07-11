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
