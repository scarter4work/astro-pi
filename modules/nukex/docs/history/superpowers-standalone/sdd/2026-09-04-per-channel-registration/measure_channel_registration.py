#!/usr/bin/env python3
"""Median red-to-green and blue-to-green star separation in a stacked image.

The acceptance measurement for per-channel registration. Blue-vs-green is the
floor: through an L-Quad Enhance green and blue both image near 500 nm, so what
it measures is centroid noise, not colour error. Red-vs-green is the quantity
the feature exists to reduce.

Usage: measure_cr.py <stacked.xisf>
"""
import sys
import numpy as np


def read_planes(path):
    """Return (R, G, B) float64 planes, each shaped (height, width)."""
    from xisf import XISF
    im = XISF(path).read_image(0, data_format='channels_last')
    if im.ndim != 3 or im.shape[2] < 3:
        raise SystemExit(f"expected a 3-channel image, got shape {im.shape}")
    a = im.astype(np.float64)
    return a[:, :, 0], a[:, :, 1], a[:, :, 2]


def centroid(plane, x, y, r=6, iters=2):
    """Median-background, iterated intensity centroid -- the same estimator the
    C++ uses, so the measurement is comparable to what the code optimises."""
    h, w = plane.shape
    px, py = float(x), float(y)
    for _ in range(iters):
        ix, iy = int(round(px)), int(round(py))
        if ix - r < 0 or ix + r >= w or iy - r < 0 or iy + r >= h:
            return None
        box = plane[iy - r:iy + r + 1, ix - r:ix + r + 1]
        ring = np.concatenate([box[0], box[-1], box[1:-1, 0], box[1:-1, -1]])
        wgt = np.clip(box - np.median(ring), 0, None)
        if wgt.sum() <= 0:
            return None
        gy, gx = np.mgrid[iy - r:iy + r + 1, ix - r:ix + r + 1]
        px, py = (wgt * gx).sum() / wgt.sum(), (wgt * gy).sum() / wgt.sum()
    return px, py


def main(path):
    R, G, B = read_planes(path)
    h, w = G.shape
    thresh = np.percentile(G, 99.9)
    ys, xs = np.where(G >= thresh)

    # One star per 16x16 cell, brightest wins -- the same 99.9th-percentile
    # sample the design document's figures come from.
    best = {}
    for y, x in zip(ys, xs):
        key = (y // 16, x // 16)
        if key not in best or G[y, x] > G[best[key][1], best[key][0]]:
            best[key] = (int(x), int(y))
    stars = list(best.values())

    print(f"{path}\n  {w}x{h}, {len(stars)} stars at the 99.9th percentile in green")
    for name, plane in (("R-G", R), ("B-G", B)):
        d = []
        for x, y in stars:
            a, b = centroid(G, x, y), centroid(plane, x, y)
            if a and b:
                d.append(np.hypot(b[0] - a[0], b[1] - a[1]))
        d = np.array(d)
        print(f"  {name}: n={len(d):4d}  median={np.median(d):.3f} px  "
              f"mean={d.mean():.3f} px  p90={np.percentile(d,90):.3f} px")


if __name__ == "__main__":
    main(sys.argv[1])
