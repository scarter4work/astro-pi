#!/usr/bin/env python3
"""M2 measures over stack-probe FITS stacks (3 planes, float32). Not shipped.

cmp.py diff A.fits B.fits      max |A - B| per channel
cmp.py m2 TRUTH.fits X.fits    blue counts at thresholds, blue mean error per truth band, the deep core, mean |error|
                               over truth >= 0.97, share more than 0.02 high in 0.97-0.995
cmp.py dim CLEAN.fits DIRTY.fits   worst dimming of DIRTY against CLEAN, samples dimmed more than 0.02, and the box of
                               samples dimmed more than 0.005
"""
import sys

import numpy as np
from astropy.io import fits
from scipy import ndimage


def load(p):
    return fits.getdata(p).astype(np.float64)  # (3, H, W)


def main():
    mode, a, b = sys.argv[1], load(sys.argv[2]), load(sys.argv[3])
    if mode == "diff":
        d = np.abs(a - b)
        print("max |diff|: %s; overall %.3g; samples > 1e-6: %d" % (
            " ".join("%.3g" % d[c].max() for c in range(d.shape[0])), d.max(), int((d > 1e-6).sum())))
    elif mode == "m2":
        t, x = a[2], b[2]  # blue
        for lvl in (0.995, 0.998, 0.999, 0.9995, 1.0):
            print("blue >= %-6g truth %7d  stack %7d" % (lvl, (t >= lvl).sum(), (x >= lvl).sum()))
        d = x - t
        for lo, hi in ((0.85, 0.97), (0.97, 0.995), (0.995, 0.999), (0.999, 9)):
            m = (t >= lo) & (t < hi)
            print("band %.3f-%.3f: n %7d, mean error %+.4f" % (lo, min(hi, 1), m.sum(), d[m].mean()))
        m = (t >= 0.97) & (t < 0.995)
        print("0.97-0.995: share > 0.02 high %.2f%%" % (100 * (d[m] > 0.02).mean()))
        m = t >= 0.97
        print("truth >= 0.97: mean |error| %.5f, worst over-lift %.4f" % (np.abs(d[m]).mean(), d[m].max()))
        core = ndimage.binary_erosion(t >= 0.98, structure=np.ones((7, 7)), border_value=0)
        print("deep core (3 px inside truth >= 0.98): %d samples, mean %+.4f" % (core.sum(), d[core].mean()))
    elif mode == "dim":
        d = b - a
        ys, xs = np.nonzero((d < -0.005).any(axis=0))
        print("worst dimming %.4f; samples dimmed > 0.02: %d; box > 0.005: x %d-%d, y %d-%d" % (
            -d.min(), int((d < -0.02).sum()), xs.min(), xs.max(), ys.min(), ys.max()))


main()
