"""Build a hermetic (no-network) fixture for the frozen-sidecar smoke test.

`grade` and online `prepare` query Gaia over the network, which a build smoke
test must not depend on. Everything the PJSR dialog runs in its default offline
mode does not: `footprint`, `prepare --gaia-tsv` (a local-DR3 export), `preview`,
`render` and `nebula`. So we synthesize a two-star scene (mirroring
tests/conftest.py) with a WCS, the matching local-DR3 TSV, a starless layer, and
-- for a render that does not depend on the frozen prepare -- a prep cache made
by the real prepare stage against an in-process fake catalog.
tools/sidecar-smoke.sh then drives every one of those subcommands through the
frozen binary, so a module the slimmed bundle lacks fails the build.

Usage: python _smoke_fixture.py <out_dir>
  writes <out_dir>/{stars.fits,starless.fits,gaia_local.tsv} and
  <out_dir>/cache/{prep.npz,qa.json}
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

    # Starless layer: a smooth glow with a darker lane, so `nebula` has structure.
    neb = 0.15 + 0.10 * np.exp(-((xx - 150) ** 2 + (yy - 100) ** 2) / (2 * 60.0 ** 2))
    neb -= 0.05 * np.exp(-((xx - 150) ** 2) / (2 * 8.0 ** 2))
    fits.PrimaryHDU(data=neb.astype("float32"), header=hdr).writeto(
        f"{out_dir}/starless.fits", overwrite=True)

    # The offline catalogue the dialog exports from PI's local Gaia DR3 database:
    # the same two stars, near (10 mas = 100 pc) and far (0.5 mas = 2000 pc).
    sky = load_wcs(hdr).pixel_to_world([near[0], far[0]], [near[1], far[1]])
    with open(f"{out_dir}/gaia_local.tsv", "w") as fh:
        fh.write("ra\tdec\tparallax\tphot_g_mean_mag\n")
        for ra, dec, plx, g in zip(sky.ra.deg, sky.dec.deg, (10.0, 0.5), (9.0, 10.0)):
            fh.write(f"{float(ra):.10f}\t{float(dec):.10f}\t{plx}\t{g}\n")
    print(f"smoke fixture written to {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1])
