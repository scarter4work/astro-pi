"""Build a hermetic (no-network) render fixture for the frozen-sidecar smoke test.

`grade`/`prepare` query Gaia over the network, which a build smoke test must not
depend on. `render` does not — it replays a saved prep cache through the image
pipeline. So we synthesize a two-star scene here (mirroring tests/conftest.py),
run the real prepare stage against an in-process fake catalog, and persist the
prep cache + stars FITS. The frozen binary's `render` then exercises the actual
image-read / render_from_prep / write_fits path with zero network.

Usage: python _smoke_fixture.py <out_dir>
  writes <out_dir>/stars.fits and <out_dir>/cache/{prep.npz,qa.json}
"""
import sys

import numpy as np
from astropy.io import fits
from astropy.table import Table
from astropy.wcs import WCS

from gaia_depth_grade.cache import save_prep
from gaia_depth_grade.config import GradeConfig
from gaia_depth_grade.distances import DistanceSource
from gaia_depth_grade.pipeline import prepare_grade
from gaia_depth_grade.wcs import load_wcs


class _FakeTwoStarSource(DistanceSource):
    """One near (100 pc) + one far (2000 pc) star at the given pixel positions."""

    def __init__(self, wcs, near_xy, far_xy):
        self._w, self._near, self._far = wcs, near_xy, far_xy

    def distances_for(self, footprint):
        sky = self._w.pixel_to_world(
            [self._near[0], self._far[0]], [self._near[1], self._far[1]])
        t = Table()
        t["ra"], t["dec"] = sky.ra.deg, sky.dec.deg
        t["r_med_geo"] = [100.0, 2000.0]
        t["r_lo_geo"] = [98.0, 1900.0]
        t["r_hi_geo"] = [102.0, 2100.0]
        t["source_id"] = [1, 2]
        return t


def _header():
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crpix = [150.0, 100.0]
    w.wcs.crval = [10.0, 20.0]
    w.wcs.cdelt = [-1.0 / 3600.0, 1.0 / 3600.0]
    hdr = w.to_header()
    hdr["NAXIS"], hdr["NAXIS1"], hdr["NAXIS2"] = 2, 300, 200
    return hdr


def main(out_dir):
    hdr = _header()
    ny, nx = 200, 300
    yy, xx = np.mgrid[0:ny, 0:nx]
    near, far = (90.0, 100.0), (210.0, 100.0)
    img = np.zeros((ny, nx))
    for (x, y) in (near, far):
        img += 0.4 * np.exp(-((xx - x) ** 2 + (yy - y) ** 2) / (2 * 2.0 ** 2))

    source = _FakeTwoStarSource(load_wcs(hdr), near, far)
    table, qa = prepare_grade(img, hdr, GradeConfig(), source)
    save_prep(f"{out_dir}/cache", table, qa)
    fits.PrimaryHDU(data=img.astype("float32"), header=hdr).writeto(
        f"{out_dir}/stars.fits", overwrite=True)
    print(f"smoke fixture written to {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1])
