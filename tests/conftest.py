import numpy as np
import pytest
from astropy.io import fits
from astropy.table import Table
from astropy.wcs import WCS

from gaia_depth_grade.distances import DistanceSource


class FakeTwoStarSource(DistanceSource):
    """Catalog with one near (100 pc) and one far (2000 pc) star at given pixels."""

    def __init__(self, wcs, near_xy, far_xy):
        self._w = wcs
        self._near = near_xy
        self._far = far_xy

    def distances_for(self, footprint):
        sky = self._w.pixel_to_world(
            [self._near[0], self._far[0]], [self._near[1], self._far[1]])
        t = Table()
        t["ra"] = sky.ra.deg
        t["dec"] = sky.dec.deg
        t["r_med_geo"] = [100.0, 2000.0]
        t["r_lo_geo"] = [98.0, 1900.0]
        t["r_hi_geo"] = [102.0, 2100.0]
        t["source_id"] = [1, 2]
        return t


@pytest.fixture
def simple_wcs_header():
    """A tangent-plane WCS centered at RA=10, Dec=20, 1 arcsec/px, 200x300."""
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crpix = [150.0, 100.0]      # center of 300(x) x 200(y)
    w.wcs.crval = [10.0, 20.0]
    w.wcs.cdelt = [-1.0 / 3600.0, 1.0 / 3600.0]
    hdr = w.to_header()
    hdr["NAXIS"] = 2
    hdr["NAXIS1"] = 300
    hdr["NAXIS2"] = 200
    return hdr


def _gaussian_star(img, x, y, flux, sigma):
    ny, nx = img.shape
    yy, xx = np.mgrid[0:ny, 0:nx]
    img += flux * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * sigma**2))


@pytest.fixture
def synthetic_stars():
    """64x64 frame, 3 stars at known positions/fluxes, sigma=2 px, faint noise."""
    rng = np.random.default_rng(0)
    img = rng.normal(0.0, 0.001, size=(64, 64)).astype(np.float64)
    truth = [(16.0, 16.0, 1.0), (48.0, 20.0, 0.6), (32.0, 50.0, 0.3)]
    for x, y, f in truth:
        _gaussian_star(img, x, y, f, sigma=2.0)
    return img, truth


@pytest.fixture
def two_star_scene(simple_wcs_header):
    """(header, mono image, source, near_xy, far_xy) for a near+far two-star frame."""
    from gaia_depth_grade.wcs import load_wcs

    w = load_wcs(simple_wcs_header)
    ny, nx = 200, 300
    yy, xx = np.mgrid[0:ny, 0:nx]
    near = (90.0, 100.0)
    far = (210.0, 100.0)
    img = np.zeros((ny, nx))
    for (x, y) in (near, far):
        img += 0.4 * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * 2.0**2))
    return simple_wcs_header, img, FakeTwoStarSource(w, near, far), near, far
