#!/usr/bin/env python3
"""Median red-to-green and blue-to-green star separation in a stacked image.

The acceptance measurement for per-channel registration.

Blue-vs-green is the FLOOR, not a second result: through an Optolong L-Quad
Enhance, green and blue both image near 500 nm, so blue-green separation is
almost pure centroid noise with very little colour error in it. Red images
H-alpha at 656 nm, so red-green carries the lateral chromatic aberration the
feature exists to remove.

The bar is therefore a RATIO of the two, not an absolute pixel figure. An
absolute threshold silently encodes whichever centroid estimator measured it:
this project's first draft quoted 0.435 px against an 0.058 px floor, and
re-measuring the very same stack with the final estimator and a larger sample
gave 0.354 px against 0.081 px. The data did not move; the estimator did.
Measuring the floor in the same run on the same image keeps the criterion
meaningful when the absolute numbers drift.

    ratio before the feature: 4.38x    passes at: 1.5x or below
    ratio as shipped in v5.0.1.1: 0.93x (R-G 0.354 px -> 0.042 px)

Red ends up marginally below the blue floor, which is the expected place
for it to land: once the colour error is removed both numbers are just
centroid noise, and neither is meaningfully larger than the other.

The estimator below is the settled one and its three parameters were each
fixed by simulation -- do not "simplify" them:

    box half-width  6 px             (4 px: worst error 0.052 -> 0.005)
    background      median of border (minimum: worst error 0.217 -> 0.057)
    passes          2                (1 pass:  worst error 0.057 -> 0.019)

The background row is the trap. The minimum of a noisy ring is biased low, and
the leftover pedestal pulls an intensity-weighted centroid toward the middle of
its own box by an amount that depends on sub-pixel phase. That does NOT cancel
between channels, which is the only thing being measured here.

Usage: measure_channel_registration.py <stacked.xisf|stacked.fit> [...]
"""
import sys

import numpy as np

# Ratio of red-green to blue-green separation at or below which the feature is
# considered to have done its job. See the module docstring for why this is a
# ratio and not a pixel count.
RATIO_BAR = 1.5


def read_planes(path):
    """Return (R, G, B) float64 planes, each shaped (height, width)."""
    if path.lower().endswith(".xisf"):
        from xisf import XISF
        im = XISF(path).read_image(0, data_format='channels_last')
    else:
        from astropy.io import fits
        with fits.open(path) as hdul:
            im = next(h.data for h in hdul if h.data is not None)
        im = np.asarray(im)
        if im.ndim == 3 and im.shape[0] <= 4:      # FITS is channels-first
            im = np.moveaxis(im, 0, -1)
    if im.ndim != 3 or im.shape[2] < 3:
        raise SystemExit(f"expected a 3-channel image, got shape {im.shape}")
    a = im.astype(np.float64)
    return a[:, :, 0], a[:, :, 1], a[:, :, 2]


def _box(plane, ix, iy, r):
    """Background-subtracted weights in a (2r+1)^2 box, or None if clipped."""
    h, w = plane.shape
    if ix - r < 0 or ix + r >= w or iy - r < 0 or iy + r >= h:
        return None
    box = plane[iy - r:iy + r + 1, ix - r:ix + r + 1]
    ring = np.concatenate([box[0], box[-1], box[1:-1, 0], box[1:-1, -1]])
    wgt = np.clip(box - np.median(ring), 0, None)
    return wgt if wgt.sum() > 0 else None


def centroid(plane, x, y, r=6, iters=2):
    """Median-background, iterated intensity centroid -- the same estimator the
    C++ uses, so the measurement is comparable to what the code optimises."""
    px, py = float(x), float(y)
    for _ in range(iters):
        ix, iy = int(round(px)), int(round(py))
        wgt = _box(plane, ix, iy, r)
        if wgt is None:
            return None
        gy, gx = np.mgrid[iy - r:iy + r + 1, ix - r:ix + r + 1]
        px, py = (wgt * gx).sum() / wgt.sum(), (wgt * gy).sum() / wgt.sum()
    return px, py


def fwhm(plane, x, y, r=6):
    """FWHM in px from the flux-weighted second moment, same box and background
    as the centroid. Used only to compare one stack against another, so its
    absolute calibration matters far less than its consistency."""
    ix, iy = int(round(x)), int(round(y))
    wgt = _box(plane, ix, iy, r)
    if wgt is None:
        return None
    gy, gx = np.mgrid[iy - r:iy + r + 1, ix - r:ix + r + 1]
    tot = wgt.sum()
    cx, cy = (wgt * gx).sum() / tot, (wgt * gy).sum() / tot
    var = ((wgt * ((gx - cx) ** 2 + (gy - cy) ** 2)).sum() / tot) / 2.0
    return 2.3548 * np.sqrt(var) if var > 0 else None


def find_stars(G):
    """Peaks in green at the 99.9th percentile, one per 16x16 cell, brightest
    wins -- the same sample the design document's figures come from."""
    ys, xs = np.where(G >= np.percentile(G, 99.9))
    best = {}
    for y, x in zip(ys, xs):
        key = (y // 16, x // 16)
        if key not in best or G[y, x] > G[best[key][1], best[key][0]]:
            best[key] = (int(x), int(y))
    return list(best.values())


def measure(path):
    R, G, B = read_planes(path)
    h, w = G.shape
    stars = find_stars(G)

    print(f"{path}")
    print(f"  {w}x{h}, {len(stars)} stars at the 99.9th percentile in green")

    seps = {}
    for name, plane in (("R-G", R), ("B-G", B)):
        d = []
        for x, y in stars:
            a, b = centroid(G, x, y), centroid(plane, x, y)
            if a and b:
                d.append(np.hypot(b[0] - a[0], b[1] - a[1]))
        d = np.array(d)
        seps[name] = np.median(d)
        print(f"  {name}: n={len(d):4d}  median={np.median(d):.3f} px  "
              f"mean={d.mean():.3f} px  p90={np.percentile(d, 90):.3f} px")

    # Green is the registration reference and is never resampled by this
    # feature, so channel registration cannot blur it. Read the direction, not
    # the mere fact of a change: green getting BLURRIER between stacks means
    # the reference channel is being warped after all, which would be a bug in
    # the near-identity skip. Green getting SHARPER is expected and is not
    # this feature -- star detection also moved from channel 0 to green, and
    # since the frame-to-frame homography is fitted from those centroids,
    # detecting on the plane that is not smeared by lateral colour tightens
    # the alignment of every channel. On the M3 set that took green from
    # 4.24 px to 3.77 px. Resampling is low-pass and cannot sharpen, which is
    # what makes the direction diagnostic in the first place.
    f = np.array([v for v in (fwhm(G, x, y) for x, y in stars) if v is not None])
    print(f"  green FWHM: n={len(f):4d}  median={np.median(f):.3f} px "
          f"(sharper is fine; BLURRIER than a prior stack would be a bug)")

    ratio = seps["R-G"] / seps["B-G"] if seps["B-G"] > 0 else float("inf")
    verdict = "PASS" if ratio <= RATIO_BAR else "FAIL"
    print(f"  ratio R-G / B-G = {ratio:.2f}x   bar {RATIO_BAR}x   {verdict}")
    return ratio <= RATIO_BAR


def main(argv):
    if not argv:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    return 0 if all([measure(p) for p in argv]) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
