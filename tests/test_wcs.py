import numpy as np
import pytest
from astropy.io import fits
from gaia_depth_grade.wcs import load_wcs, field_footprint, FieldFootprint


def test_load_wcs_ok(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    assert w.has_celestial


def test_load_wcs_missing_raises():
    hdr = fits.Header()
    hdr["NAXIS"] = 2
    with pytest.raises(ValueError):
        load_wcs(hdr)


def test_field_footprint_center_and_radius(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    fp = field_footprint(w, (200, 300))
    assert isinstance(fp, FieldFootprint)
    assert fp.center_ra == pytest.approx(10.0, abs=1e-3)
    assert fp.center_dec == pytest.approx(20.0, abs=1e-3)
    # half-diagonal of 300x200 px at 1"/px ≈ 180.3" ≈ 0.0501 deg
    assert fp.radius_deg == pytest.approx(0.0501, abs=2e-3)
