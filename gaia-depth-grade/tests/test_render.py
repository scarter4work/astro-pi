import numpy as np
import pytest
from astropy.table import Table
from gaia_depth_grade.modulate import Modulation
from gaia_depth_grade.render import render_stars


def _one_star_layer(flux=0.5, x=32, y=32, sigma=2.0):
    ny = nx = 64
    yy, xx = np.mgrid[0:ny, 0:nx]
    img = flux * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma**2))
    det = Table()
    det["x"] = [float(x)]; det["y"] = [float(y)]
    det["flux"] = [flux]; det["fwhm"] = [2.355 * sigma]
    return img, det


def test_brightness_up_increases_peak():
    img, det = _one_star_layer()
    img_before = img.copy()
    base_peak = img.max()
    m = Modulation(brightness=np.array([1.5]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert out.max() > base_peak
    assert np.shares_memory(out, img) is False  # input not mutated
    assert np.array_equal(img, img_before)  # input values untouched, not just non-shared


def test_brightness_down_decreases_peak():
    img, det = _one_star_layer()
    base_peak = img.max()
    m = Modulation(brightness=np.array([0.5]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert out.max() < base_peak


def test_size_up_widens_footprint():
    img, det = _one_star_layer()
    # Measure spatial extent with a FIXED absolute threshold. A relative
    # max*0.5 threshold rises as the glow raises the peak, masking the
    # widening; a fixed threshold counts the genuinely-expanded wings.
    def above_thresh(a, t=0.1):
        return int((a > t).sum())
    base_area = above_thresh(img)
    m = Modulation(brightness=np.array([1.0]), size=np.array([1.6]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    assert above_thresh(out) > base_area


def test_no_square_seam_on_uniform_background():
    # A dimmed (far) star on a flat background must NOT stamp a square patch:
    # the per-star modulation is feathered, so the window corners stay at the
    # background value while the centre is modulated. (Regression: the old flat
    # `win *= b` over the square window left hard-edged squares around stars.)
    bg = 0.4
    ny = nx = 64
    img = np.full((ny, nx), bg, dtype=float)
    det = Table()
    det["x"] = [32.0]; det["y"] = [32.0]; det["flux"] = [0.0]; det["fwhm"] = [4.7]
    m = Modulation(brightness=np.array([0.4]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.0]))
    out = render_stars(img, det, m, base_sigma_px=2.0)
    # Centre is darkened; the window corner (taper -> 0) is untouched background.
    assert out[32, 32] < bg - 0.05
    assert out[0, 0] == pytest.approx(bg)
    # No abrupt edge: every pixel stays within [modulated_centre, background].
    assert out.min() >= 0.4 * bg - 1e-9 and out.max() <= bg + 1e-9


def test_output_clipped_and_color_shape_preserved():
    img, det = _one_star_layer()
    color = np.stack([img, img * 0.5, img * 0.2], axis=-1)
    m = Modulation(brightness=np.array([2.0]), size=np.array([1.0]),
                   contrast=np.array([0.0]), saturation=np.array([1.3]))
    out = render_stars(color, det, m, base_sigma_px=2.0)
    assert out.shape == color.shape
    assert out.max() <= 1.0 and out.min() >= 0.0
