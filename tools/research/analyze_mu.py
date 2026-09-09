#!/usr/bin/env python3
"""Compare the model race's mu against a Huber M-estimator's mu, pixel by pixel.

Reads a NUKEX_DUMP_MU file written by gpu_executor.cpp:

    int32 width, height, n_channels
    float32 race[ch][y][x]
    float32 huber[ch][y][x]
    float32 shape[ch][y][x]   (optional third plane; DistributionShape of the
                               winning model, 255 where nothing was fitted)

Both planes come from the IDENTICAL voxel samples, so any difference in
pixel-scale noise is attributable to the estimator alone.

Why not MAD about the median?  A stacked astro frame is mostly structure --
nebulosity, galaxy, gradient -- and MAD about a global median scores that
structure as noise.  The estimator here differences horizontally adjacent
pixels, which cancels anything smooth and leaves only what changes from one
pixel to the next.  Background pairs are selected automatically (both members
below the channel median) so no tile is hand-picked.

    usage: analyze_mu.py <mu.bin> [label] [--estimator=mad|std|both]
"""
import sys
import numpy as np


# DistributionShape (core/distribution.hpp) -> which optimiser produced it.
SHAPE_TO_MODEL = {0: "Student-t", 2: "Student-t",
                  1: "GMM", 4: "GMM",
                  3: "Contamination",
                  5: "KDE/other", 6: "KDE/other", 255: "(unfitted)"}


def load(path):
    """Return (w, h, c, planes).  Handles both the 2-plane dumps archived on
    2026-09-07 and the 3-plane dumps that carry the winning model."""
    import os
    with open(path, 'rb') as f:
        w, h, c = np.frombuffer(f.read(12), dtype='<i4')
    w, h, c = int(w), int(h), int(c)
    per_plane = w * h * c * 4
    n_planes = (os.path.getsize(path) - 12) // per_plane
    if n_planes not in (2, 3):
        raise SystemExit(f"{path}: {n_planes} planes, expected 2 or 3")
    arr = np.memmap(path, dtype='<f4', mode='r', offset=12,
                    shape=(n_planes, c, h, w))
    return w, h, c, arr


def neighbour_noise(plane, mask, estimator):
    """Pixel-scale noise from horizontally adjacent differences.

    `mask` marks usable pixels; a pair counts only when both members are
    usable and both are background.  The /sqrt(2) converts the noise of a
    difference of two independent samples back to per-pixel noise.
    """
    a = plane[:, :-1]
    b = plane[:, 1:]
    ok = mask[:, :-1] & mask[:, 1:]
    if not ok.any():
        return float('nan'), 0
    d = (b[ok] - a[ok]).astype(np.float64)
    if d.size < 100:
        return float('nan'), int(d.size)
    if estimator == 'std':
        s = d.std()
    else:  # robust: MAD of the differences, scaled to a Gaussian sigma
        s = 1.4826 * np.median(np.abs(d - np.median(d)))
    return float(s / np.sqrt(2.0)), int(d.size)


def analyse(path, label, estimators):
    w, h, c, arr = load(path)
    has_shape = arr.shape[0] >= 3
    print(f"=== {label} ===")
    print(f"  {w} x {h} px, {c} channel(s)"
          + ("" if has_shape else "   [2-plane dump: no winner plane]"))
    for est in estimators:
        rows = []
        hybrid_gmm_share = []
        for ch in range(c):
            race = np.asarray(arr[0, ch], dtype=np.float32)
            hub = np.asarray(arr[1, ch], dtype=np.float32)
            # Uncovered voxels are written as 0 in both planes; a channel that
            # never ran (LRGB-mono slots with no frames) is all zero.
            covered = (race != 0.0) | (hub != 0.0)
            if covered.sum() < 1000:
                rows.append((ch, None, None, None, 0, None))
                continue
            # Background = below the covered median, on the RACE plane, so the
            # same pixel set is scored for both estimators.
            med = np.median(race[covered])
            bg = covered & (race < med)
            nr, npairs = neighbour_noise(race, bg, est)
            nh, _ = neighbour_noise(hub, bg, est)
            hyb = None
            if has_shape:
                # "The race, except fall back to Huber wherever the GMM won."
                shp = np.asarray(arr[2, ch], dtype=np.float32)
                gmm = (shp == 1.0) | (shp == 4.0)
                blend = np.where(gmm, hub, race)
                hyb, _ = neighbour_noise(blend, bg, est)
                hybrid_gmm_share.append(float(gmm[covered].mean()))
            rows.append((ch, nr, nh, (nr / nh if nh and nh > 0 else float('nan')),
                         npairs, hyb))
        print(f"  estimator = {est}")
        hdr = f"    {'ch':>3}  {'race noise':>12}  {'huber noise':>12}  {'ratio':>7}  {'pairs':>12}"
        if has_shape:
            hdr += f"  {'hybrid':>12}  {'hyb/hub':>8}"
        print(hdr)
        num = den = hnum = 0.0
        for ch, nr, nh, ratio, npairs, hyb in rows:
            if nr is None:
                print(f"    {ch:>3}  {'(empty)':>12}")
                continue
            line = f"    {ch:>3}  {nr:12.8f}  {nh:12.8f}  {ratio:7.2f}x  {npairs:12,}"
            if hyb is not None:
                line += f"  {hyb:12.8f}  {hyb/nh:7.2f}x"
                hnum += hyb * npairs
            print(line)
            num += nr * npairs
            den += nh * npairs
        if den > 0:
            extra = (f"   hybrid(no-GMM)/huber {hnum/den:.2f}x" if hnum > 0 else "")
            print(f"    {'all':>3}  pair-weighted race/huber: {num/den:.2f}x{extra}")
        if hybrid_gmm_share:
            print(f"         GMM won {100*np.mean(hybrid_gmm_share):.1f}% of covered voxels")

    # Background noise says nothing about whether the cheaper estimator throws
    # away real signal.  A clipping estimator shows up here as a systematically
    # LOWER bright end -- stars and nebula cores shaved off.
    print("  bright end (does Huber clip signal?):")
    print(f"    {'ch':>3}  {'pct':>7}  {'race':>12}  {'huber':>12}  {'huber-race':>11}")
    for ch in range(c):
        race = np.asarray(arr[0, ch], dtype=np.float32)
        hub = np.asarray(arr[1, ch], dtype=np.float32)
        covered = (race != 0.0) | (hub != 0.0)
        if covered.sum() < 1000:
            continue
        r, hh = race[covered], hub[covered]
        for pct in (99.0, 99.9, 99.99):
            a, b = np.percentile(r, pct), np.percentile(hh, pct)
            rel = 100 * (b - a) / a if a != 0 else float('nan')
            print(f"    {ch:>3}  {pct:6.2f}%  {a:12.8f}  {b:12.8f}  {rel:+10.2f}%")
    print()


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    flags = [a for a in sys.argv[1:] if a.startswith('--')]
    est = 'both'
    for f in flags:
        if f.startswith('--estimator='):
            est = f.split('=', 1)[1]
    estimators = ['mad', 'std'] if est == 'both' else [est]
    analyse(args[0], args[1] if len(args) > 1 else args[0], estimators)
