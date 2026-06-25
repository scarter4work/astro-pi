import numpy as np
from PIL import Image

from gaia_depth_grade.display import autostretch, screen_blend, write_png


def test_screen_blend_formula():
    a = np.array([[0.0, 0.5, 1.0]])
    b = np.array([[0.2, 0.5, 0.0]])
    out = screen_blend(a, b)
    assert np.allclose(out, 1.0 - (1.0 - a) * (1.0 - b))


def test_autostretch_brightens_and_is_uint8():
    rng = np.random.default_rng(0)
    img = np.clip(rng.normal(0.02, 0.005, size=(64, 64, 3)), 0, 1)
    img[30:34, 30:34] = 0.8  # a bright star
    out = autostretch(img)
    assert out.dtype == np.uint8
    assert out.shape == (64, 64, 3)
    # dark background median lifted well above its tiny linear value
    assert np.median(out) > 20


def test_write_png_downscale(tmp_path):
    img = np.zeros((200, 400, 3), dtype=np.uint8)
    p = tmp_path / "full.png"
    write_png(img, str(p), max_width=100)
    w, h = Image.open(p).size
    assert w == 100 and h == 50


def test_write_png_region_crop(tmp_path):
    img = (np.random.default_rng(1).random((200, 400, 3)) * 255).astype(np.uint8)
    p = tmp_path / "inset.png"
    write_png(img, str(p), region=(50, 30, 80, 60))  # x, y, w, h
    w, h = Image.open(p).size
    assert w == 80 and h == 60
