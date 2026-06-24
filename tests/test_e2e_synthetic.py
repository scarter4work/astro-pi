import json
import numpy as np
import pytest
from astropy.io import fits
from astropy.table import Table
from gaia_depth_grade.config import GradeConfig, Gains
from gaia_depth_grade.distances import DistanceSource
from gaia_depth_grade.cli import grade_array, write_fits

HONESTY = "GAIA depth-derived; physically motivated, not per-pixel correct for gas"


class FakeSource(DistanceSource):
    """Two stars: a near one (100pc) and a far one (2000pc)."""
    def __init__(self, wcs, near_xy, far_xy):
        self._w = wcs; self._near = near_xy; self._far = far_xy

    def distances_for(self, footprint):
        sky = self._w.pixel_to_world(
            [self._near[0], self._far[0]], [self._near[1], self._far[1]])
        t = Table()
        t["ra"] = sky.ra.deg; t["dec"] = sky.dec.deg
        t["r_med_geo"] = [100.0, 2000.0]
        t["r_lo_geo"] = [98.0, 1900.0]; t["r_hi_geo"] = [102.0, 2100.0]
        t["source_id"] = [1, 2]
        return t


def _two_star_frame(simple_wcs_header):
    from gaia_depth_grade.wcs import load_wcs
    w = load_wcs(simple_wcs_header)
    ny, nx = 200, 300
    yy, xx = np.mgrid[0:ny, 0:nx]
    near = (90.0, 100.0); far = (210.0, 100.0)
    img = np.zeros((ny, nx))
    for (x, y) in (near, far):
        img += 0.4 * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * 2.0**2))
    return w, img, near, far


def test_near_brightens_far_dims_end_to_end(simple_wcs_header):
    w, img, near, far = _two_star_frame(simple_wcs_header)
    cfg = GradeConfig(gains=Gains(brightness=0.6, size=0.0, contrast=0.0, saturation=0.0),
                      p_low=0, p_high=100, min_match_rate=0.0)
    src = FakeSource(w, near, far)
    graded, qa = grade_array(img, simple_wcs_header, cfg, src)
    near_peak = graded[96:104, 86:94].max()
    far_peak = graded[96:104, 206:214].max()
    assert near_peak > 0.4   # near star brightened above original 0.4
    assert far_peak < 0.4    # far star dimmed
    assert qa["n_matched"] == 2
    assert qa["low_match_warning"] is False


def test_write_fits_has_honesty_tag(tmp_path, simple_wcs_header):
    w, img, near, far = _two_star_frame(simple_wcs_header)
    out = tmp_path / "g.fits"
    write_fits(str(out), img, simple_wcs_header, {"match_rate": 1.0})
    hdr = fits.getheader(str(out))
    assert hdr.get("DEPTHTAG", "") == HONESTY            # full verbatim tag, no truncation
    assert any(HONESTY in str(c) for c in hdr["HISTORY"])
    qa = json.loads((tmp_path / "g.fits.qa.json").read_text())
    assert qa["match_rate"] == 1.0
