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
