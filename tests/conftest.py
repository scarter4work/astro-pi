import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS


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
