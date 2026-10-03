#!/usr/bin/env python3
"""How far does the race's mu sit from Huber's, split by which model won?

Reads a NUKEX_DUMP_VOXELS file.  Each record carries the voxel's own samples,
so Huber can be recomputed here from the IDENTICAL inputs the race saw -- no
spatial join with the mu dump is needed, and the comparison stays exact.

The deviation is reported in units of each voxel's own robust scale, because an
absolute offset means nothing without knowing the local noise: 0.001 is
catastrophic on a mono L frame and invisible on a bright OSC one.

    usage: analyze_winner_deviation.py <vox.bin> [label]
"""
import struct
import sys
import collections

import numpy as np

NAMES = {0: "Student-t", 1: "GMM", 2: "Contamination", 3: "KDE/other"}


def huber_irls(x, w, iters=6):
    """Mirror of mudump::huber_irls in gpu_executor.cpp."""
    n = x.size
    if n <= 0:
        return 0.0
    med = float(np.median(x))
    madv = float(np.median(np.abs(x - med)))
    sigma = madv * 1.4826
    if not sigma > 1e-12:
        return med
    delta = 1.345 * sigma
    mu = med
    for _ in range(iters):
        r = np.abs(x - mu)
        hw = np.where(r <= delta, 1.0, delta / np.maximum(r, 1e-30))
        ww = hw * w
        den = ww.sum()
        if den <= 0:
            break
        mu = float((ww * x).sum() / den)
    return mu


def records(path):
    with open(path, 'rb') as f:
        magic, ver, stride = struct.unpack('<III', f.read(12))
        assert magic == 0x4456584E, f"bad magic {magic:#x}"
        while True:
            b = f.read(4)
            if len(b) < 4:
                break
            n = struct.unpack('<I', b)[0]
            vals = np.frombuffer(f.read(4 * n), dtype='<f4')
            wts = np.frombuffer(f.read(4 * n), dtype='<f4')
            fb = f.read(288)
            if len(fb) < 288 or vals.size < n:
                break
            yield n, vals.astype(np.float64), wts.astype(np.float64), struct.unpack('<36d', fb)
    return


def main(path, label):
    by_winner = collections.defaultdict(list)
    n_tot = 0
    samples, scales, close = [], [], 0
    for n, vals, wts, f in records(path):
        winner = int(round(f[25]))
        race_mu = f[27]                 # best.distribution.true_signal_estimate
        scale = f[1] if f[1] > 0 else f[31]   # robust scale, else raw MAD
        hub_mu = huber_irls(vals, wts)
        n_tot += 1
        samples.append(n)
        if scale > 0:
            scales.append(scale)
        # Stage 1 left the scale/dAICc story UNRESOLVED because the spot check
        # read the first 4000 records in FILE-WRITE order -- low-coverage edge
        # voxels with 1-5 samples.  These records are hash-sampled, so the same
        # statistics computed here are unbiased.
        aiccs = sorted(a for a, c in ((f[8], f[3]), (f[18], f[9]), (f[24], f[19]))
                       if c > 0.5)
        if len(aiccs) > 1 and (aiccs[1] - aiccs[0]) < 2.0:
            close += 1
        if scale and scale > 0:
            by_winner[winner].append(abs(race_mu - hub_mu) / scale)

    if not n_tot:
        print(f"{label}: no records")
        return

    # Noise adds in quadrature, so what a model costs the stack is its share of
    # the TOTAL squared deviation -- not its win rate, and not its median.  A
    # model can win 3% of voxels and still own most of the damage.
    tot_sq = sum(float((np.array(v) ** 2).sum()) for v in by_winner.values())

    print(f"=== {label} ===")
    print(f"  records: {n_tot:,}   (uniform hash sample -- unlike Stage 1's "
          f"file-order spot check)")
    print(f"  samples/voxel: median {np.median(samples):.0f}  "
          f"p10 {np.percentile(samples,10):.0f}  p90 {np.percentile(samples,90):.0f}")
    if scales:
        print(f"  robust scale:  median {np.median(scales):.8f}  "
              f"p10 {np.percentile(scales,10):.8f}  "
              f"p90 {np.percentile(scales,90):.8f}")
    print(f"  dAICc < 2 vs runner-up: {100*close/n_tot:.1f}%")
    print(f"    {'winner':<14} {'count':>9} {'share':>7} "
          f"{'med |dmu|/s':>12} {'rms':>8} {'p99':>8} "
          f"{'>1s':>7} {'var share':>10}")
    allv = []
    for w in sorted(by_winner):
        d = np.array(by_winner[w])
        allv.append(d)
        vs = 100 * float((d ** 2).sum()) / tot_sq if tot_sq > 0 else float('nan')
        print(f"    {NAMES.get(w, w):<14} {d.size:>9,} {100*d.size/n_tot:6.1f}% "
              f"{np.median(d):12.4f} {np.sqrt((d**2).mean()):8.4f} "
              f"{np.percentile(d,99):8.4f} "
              f"{100*(d>1).mean():6.1f}% {vs:9.1f}%")
    if allv:
        d = np.concatenate(allv)
        print(f"    {'ALL':<14} {d.size:>9,} {100.0:6.1f}% "
              f"{np.median(d):12.4f} {np.sqrt((d**2).mean()):8.4f} "
              f"{np.percentile(d,99):8.4f} "
              f"{100*(d>1).mean():6.1f}% {100.0:9.1f}%")
    print()


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else sys.argv[1])
