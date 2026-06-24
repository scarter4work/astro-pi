import numpy as np
import pytest
from gaia_depth_grade.detect import detect_stars, measure_fwhm


def test_detects_all_three(synthetic_stars):
    img, truth = synthetic_stars
    tbl = detect_stars(img, fwhm=3.0, threshold_sigma=5.0)
    assert len(tbl) >= 3
    # brightest detected source should sit near the brightest truth star (16,16)
    tbl.sort("flux", reverse=True)
    assert tbl["x"][0] == pytest.approx(16.0, abs=1.0)
    assert tbl["y"][0] == pytest.approx(16.0, abs=1.0)


def test_measure_fwhm_matches_sigma(synthetic_stars):
    img, _ = synthetic_stars
    f = measure_fwhm(img, 16.0, 16.0, box=11)
    # FWHM = 2.355 * sigma ; sigma=2 -> ~4.71 px
    assert f == pytest.approx(4.71, abs=1.2)


def test_measure_fwhm_offedge_is_nan(synthetic_stars):
    img, _ = synthetic_stars
    assert np.isnan(measure_fwhm(img, 1.0, 1.0, box=11))
