"""16-bit PNG (AutoStakkert's output format) and the other formats Pillow would
silently truncate (§2.2, §12).

Measured 2026-10-22 with Pillow 12.3 and 13.0: a 48-bit RGB PNG holding
(65535, 40000, 257) opens as mode "RGB" / uint8 and loads as
(1.0, 0.61176, 0.00392) — 156/255 where 40000/65535 = 0.61036 is correct — with
no warning. PNG was missed when the same failure was closed for TIFF
(tests/test_raster.py).
"""

import struct
import zlib

import numpy as np
import pytest
from PIL import Image

from autocontrast.io.loaders import downsample_factor, image_dimensions, load_image, load_raster


def _write_png16(path, arr):
    """Write a 16-bit PNG with no imaging library, so the fixture is independent of
    both the decoder under test and the Pillow path it must not take.

    ``arr`` is (H, W) grayscale or (H, W, 3) RGB uint16.
    """
    height, width = arr.shape[:2]
    color_type = 0 if arr.ndim == 2 else 2
    raw = b"".join(b"\x00" + arr[y].astype(">u2").tobytes() for y in range(height))

    def chunk(tag, body):
        crc = zlib.crc32(tag + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 16, color_type, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                     + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def test_sixteen_bit_rgb_png_round_trips_without_losing_the_low_byte(tmp_path):
    path = tmp_path / "stack.png"
    arr = np.zeros((4, 6, 3), dtype=np.uint16)
    arr[1, 1] = (65535, 40000, 257)
    _write_png16(path, arr)

    img = load_raster(path)

    assert img.shape == (4, 6, 3)
    np.testing.assert_allclose(img[1, 1], [1.0, 40000 / 65535, 257 / 65535], rtol=0, atol=1e-12)


def test_sixteen_bit_grayscale_png_is_promoted_to_three_channels_at_full_depth(tmp_path):
    path = tmp_path / "mono.png"
    arr = np.zeros((4, 6), dtype=np.uint16)
    arr[2, 3] = 40000
    arr[0, 0] = 257
    _write_png16(path, arr)

    img = load_image(path)

    assert img.shape == (4, 6, 3)
    np.testing.assert_allclose(img[2, 3], 40000 / 65535, rtol=0, atol=1e-12)
    np.testing.assert_allclose(img[0, 0], 257 / 65535, rtol=0, atol=1e-12)


def test_a_dark_sixteen_bit_rgb_png_is_not_rescaled_as_if_it_were_eight_bit(tmp_path):
    """Full scale comes from the PNG's declared depth, not from max(pixels)."""
    path = tmp_path / "dark.png"
    _write_png16(path, np.full((4, 4, 3), 200, dtype=np.uint16))

    np.testing.assert_allclose(load_raster(path), 200 / 65535, rtol=0, atol=1e-12)


def test_a_dark_sixteen_bit_grayscale_png_is_not_rescaled_as_if_it_were_eight_bit(tmp_path):
    path = tmp_path / "dark-mono.png"
    _write_png16(path, np.full((4, 4), 200, dtype=np.uint16))

    np.testing.assert_allclose(load_raster(path), 200 / 65535, rtol=0, atol=1e-12)


def test_sixteen_bit_png_survives_max_dim_downsizing(tmp_path):
    path = tmp_path / "big.png"
    _write_png16(path, np.full((100, 200, 3), 40000, dtype=np.uint16))

    img = load_raster(path, max_dim=50)

    assert img.shape == (25, 50, 3)
    np.testing.assert_allclose(img, 40000 / 65535, rtol=0, atol=1e-6)


def test_a_dark_sixteen_bit_png_survives_the_downsizing_path_too(tmp_path):
    path = tmp_path / "dark-big.png"
    _write_png16(path, np.full((100, 200, 3), 200, dtype=np.uint16))

    np.testing.assert_allclose(load_raster(path, max_dim=50), 200 / 65535, rtol=0, atol=1e-6)


def test_sixteen_bit_png_image_dimensions_are_native(tmp_path):
    path = tmp_path / "dims.png"
    _write_png16(path, np.zeros((100, 200, 3), dtype=np.uint16))

    assert image_dimensions(path) == (200, 100)
    assert downsample_factor(path, load_raster(path, max_dim=50)) == pytest.approx(4.0)


def test_pillow_would_have_truncated_that_png(tmp_path):
    """Keeps the PNG tests above from becoming vacuous: Pillow accepts the file and
    returns 8 bits. If this ever fails Pillow grew 48-bit RGB support."""
    path = tmp_path / "stack.png"
    arr = np.zeros((4, 6, 3), dtype=np.uint16)
    arr[1, 1] = (65535, 40000, 257)
    _write_png16(path, arr)

    with Image.open(path) as im:
        pillow_arr = np.asarray(im)

    assert pillow_arr.dtype == np.uint8
    np.testing.assert_array_equal(pillow_arr[1, 1], [255, 156, 1])


def test_a_corrupt_sixteen_bit_png_raises_instead_of_falling_back_to_pillow(tmp_path):
    path = tmp_path / "bad.png"
    _write_png16(path, np.zeros((4, 4, 3), dtype=np.uint16))
    path.write_bytes(path.read_bytes()[:-40])  # cut into the IDAT stream

    with pytest.raises(ValueError, match="bad.png"):
        load_raster(path)


# The supported set is an explicit allow-list. Everything else Pillow registers is
# refused by name, because Pillow accepts it and quietly reduces it to 8 bits.


def test_supported_suffixes_is_exactly_the_verified_allow_list():
    from autocontrast.io.loaders import supported_suffixes

    assert supported_suffixes() == frozenset({
        ".fits", ".fit", ".fts", ".tif", ".tiff", ".png",
        ".jpg", ".jpeg", ".jpe", ".jfif", ".webp",
    })


@pytest.mark.parametrize("name, payload", [
    ("wide.ppm", b"P6\n1 1\n65535\n" + np.array([65535, 40000, 257], ">u2").tobytes()),
    ("wide.pgm", b"P5\n1 1\n65535\n" + np.array([200], ">u2").tobytes()),
    ("wide.sgi", b"\x01\xda\x00\x02\x00\x03" + bytes(506)),
    ("hdr.avif", b"\x00\x00\x00\x14pixi\x00\x00\x00\x00\x03\x0a\x0a\x0a"),
    ("scene.dds", b"DDS " + bytes(120)),
    ("layered.psd", b"8BPS" + bytes(30)),
    ("wide.jp2", b"\x00\x00\x00\x0cjP  \r\n\x87\n"),
    ("anim.gif", b"GIF89a" + bytes(10)),
    ("pic.bmp", b"BM" + bytes(50)),
    ("pic.xisf", b"XISF0100" + bytes(10)),
    ("noext", b"anything"),
])
def test_a_suffix_outside_the_allow_list_is_refused_naming_the_supported_set(tmp_path, name, payload):
    path = tmp_path / name
    path.write_bytes(payload)

    for call in (load_raster, load_image, image_dimensions):
        with pytest.raises(ValueError, match=r"unsupported image format.*supported: \.fit,"):
            call(path)


def test_a_real_sixteen_bit_jpeg2000_is_refused_not_truncated(tmp_path):
    cv2 = pytest.importorskip("cv2")  # only used to author the fixture
    path = tmp_path / "wide.jp2"
    assert cv2.imwrite(str(path), np.tile(np.array([[[257, 40000, 65535]]], np.uint16), (64, 64, 1)))

    with pytest.raises(ValueError, match="unsupported image format"):
        load_raster(path)


def test_the_allow_list_is_case_insensitive(tmp_path):
    path = tmp_path / "STACK.PNG"
    _write_png16(path, np.full((4, 4, 3), 40000, dtype=np.uint16))

    np.testing.assert_allclose(load_raster(path), 40000 / 65535, rtol=0, atol=1e-12)


@pytest.mark.parametrize("suffix, kwargs", [
    (".jpg", {"quality": 100}), (".jpeg", {"quality": 100}), (".webp", {"lossless": True}),
])
def test_eight_bit_formats_on_the_allow_list_still_load(tmp_path, suffix, kwargs):
    path = tmp_path / f"ok{suffix}"
    Image.fromarray(np.full((8, 8, 3), 128, dtype=np.uint8)).save(path, **kwargs)

    img = load_raster(path)

    assert img.shape == (8, 8, 3)
    np.testing.assert_allclose(img, 128 / 255, atol=2 / 255)
