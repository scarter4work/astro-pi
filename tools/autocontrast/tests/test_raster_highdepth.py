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


# Formats Pillow also opens and truncates. Each guard reads the file's own header,
# so a crafted header is enough to prove the loader refuses instead of decoding.


def test_sixteen_bit_ppm_is_rejected_not_truncated(tmp_path):
    path = tmp_path / "wide.ppm"
    path.write_bytes(b"P6\n1 1\n65535\n" + np.array([65535, 40000, 257], ">u2").tobytes())

    with pytest.raises(ValueError, match=r"wide\.ppm.*16-bit"):
        load_raster(path)


def test_sixteen_bit_pgm_is_rejected_not_rescaled_by_inference(tmp_path):
    path = tmp_path / "wide.pgm"  # Pillow returns int32 here, so a dark one would infer /255
    path.write_bytes(b"P5\n# comment\n1 1\n65535\n" + np.array([200], ">u2").tobytes())

    with pytest.raises(ValueError, match=r"wide\.pgm.*16-bit"):
        load_raster(path)


def test_eight_bit_ppm_still_loads(tmp_path):
    path = tmp_path / "ok.ppm"
    path.write_bytes(b"P6\n1 1\n255\n" + bytes([255, 128, 0]))

    np.testing.assert_allclose(load_raster(path)[0, 0], [1.0, 128 / 255, 0.0])


def test_sixteen_bit_sgi_is_rejected_not_truncated(tmp_path):
    path = tmp_path / "wide.sgi"
    # magic 474, storage 0 (verbatim), bpc 2, dimension 3
    path.write_bytes(b"\x01\xda\x00\x02\x00\x03" + bytes(506))

    with pytest.raises(ValueError, match=r"wide\.sgi.*16-bit"):
        load_raster(path)


def test_sixteen_bit_jpeg2000_rgb_is_rejected_not_truncated(tmp_path):
    cv2 = pytest.importorskip("cv2")  # only used to author the fixture
    path = tmp_path / "wide.jp2"
    ok = cv2.imwrite(str(path), np.tile(np.array([[[257, 40000, 65535]]], np.uint16), (64, 64, 1)))
    assert ok

    with pytest.raises(ValueError, match=r"wide\.jp2.*16-bit"):
        load_raster(path)


def test_high_bit_depth_avif_is_rejected_not_truncated(tmp_path):
    path = tmp_path / "hdr.avif"
    # An ISOBMFF 'pixi' property box (version/flags, 3 channels, 10 bits each).
    path.write_bytes(b"\x00\x00\x00\x14pixi\x00\x00\x00\x00\x03\x0a\x0a\x0a")

    with pytest.raises(ValueError, match=r"hdr\.avif.*10-bit"):
        load_raster(path)
