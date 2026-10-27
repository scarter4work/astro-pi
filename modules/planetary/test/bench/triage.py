"""Split-half triage (Ruling P28 fix round 3, review I-E): what is a capture's split-half excess?
Validation only, never part of a verdict. Reads the bench's output directory and the capture (NAS,
read-only); `halves` writes only under its work directory.

  python triage.py rows   <bench out dir> <capture id>
  python triage.py halves <bench out dir> <capture id> <work dir> [--region-probe PATH]

ROWS: is the row structure in D sensor banding (constant along a SENSOR row, frame by frame) or
signal that follows the target (Jupiter's belts)?
  * Additive banding is shared by the sky left and right of the disc in the same rows, so the
    covariance of the two sky segments' row means measures it with white noise cancelled. Taken on
    the raw frames' lag-1 differences (sensor rows) and on D over the full frame.
  * Disc rows: the angle at which the variance of row means over the disc peaks, for A's high-passed
    belts and for D (and the raw frames' relative lag-1 differences); the excess over angle fitted as a
    belt-aligned part (A's own angular profile) plus a sensor-row part (the same profile at 0 deg).

HALVES: is the excess the region stage's or the frames'?
  * Both halves re-stacked by stack-region-probe --fits-out: the region stack (RunStack's flow) and the
    global-only stack of the same frames (no regions, so no kept subsets and no level offsets between
    them; Task 9 Investigation 2). R per END bin for each, with its own noise-power k over A's box.
  * D of the region stacks decomposed tile by tile (32 px) in the END band (0.06-0.30): the share of
    its power explained by a gain on A (a level or gain offset between kept subsets multiplies the
    texture), a local shift (dA/dy, dA/dx: the halves' regions registered differently) and a local
    blur (lap A). The decomposition takes D after the registration and ONE global gain only
    (gain_only_difference), not after refined_difference's global gain-and-shift fit: that fit turned a
    smooth gain field into a global pseudo-shift until bench6 (a 3 % field read as a (0.10, -0.18) px shift on a
    synthetic surface; Ruling F5 fixed it there too: its shift is now fitted jointly with a level field), so the
    old decomposition read a level offset between kept subsets
    as a local shift -- 0.13 gain, 0.82 shift for a pure gain field (final review, bench M4). The tiles'
    own shift terms now explain any shift, global or local. The R it prints is the bench's, with the tile
    shifts removed (Ruling P28d, fix round 4)."""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from scipy import ndimage
from scipy.optimize import nnls

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bayer  # noqa: E402
import benchset  # noqa: E402
import halves  # noqa: E402
import metric  # noqa: E402
import noise  # noqa: E402
import refs  # noqa: E402

ANGLES = np.arange(-30, 30.5, 1.0)
TRIAGE_FRAMES = 160


def _cov(a, b):
    return float(np.mean((a - a.mean()) * (b - b.mean())))


def _sky_rows(mask, gap=12):
    """Per row crossing the (eroded) disc: (y, sky-left cols, sky-right cols)."""
    out = []
    for y in np.flatnonzero(mask.any(1)):
        xs = np.flatnonzero(mask[y])
        if xs.size >= 8 and xs[0] - gap > 4 and xs[-1] + gap < mask.shape[1] - 4:
            out.append((y, slice(0, xs[0] - gap), slice(xs[-1] + gap, mask.shape[1])))
    return out


def sky_banding(images, mask):
    """cov(row mean of the sky left of the disc, row mean of the sky right), over every image's disc
    rows: additive banding constant along the row, white noise cancelled."""
    rows = _sky_rows(mask)
    L = np.array([im[y, sl].mean() for im in images for y, sl, _ in rows])
    R = np.array([im[y, sr].mean() for im in images for y, _, sr in rows])
    return _cov(L, R), len(rows)


def angle_profile(img, mask):
    """Variance of row means over the disc after rotating by each of ANGLES (weighted by row length)."""
    out = []
    for a in ANGLES:
        r = ndimage.rotate(img, a, reshape=False, order=1)
        m = ndimage.rotate(mask.astype(float), a, reshape=False, order=1) > 0.99
        ys = [y for y in range(r.shape[0]) if m[y].sum() >= 10]
        rows = np.array([r[y][m[y]].mean() for y in ys])
        w = np.array([m[y].sum() for y in ys])
        out.append(float(np.average((rows - rows.mean()) ** 2, weights=w)))
    return np.array(out)


def split_by_angle(v_target, v_img):
    """(belt-aligned, sensor-row) parts of v_img's excess over angle, the residual's share, and the
    angle where the target's own profile peaks."""
    far = np.abs(ANGLES) >= 24
    eT, eI = v_target - np.median(v_target[far]), v_img - np.median(v_img[far])
    peak = ANGLES[eT.argmax()]
    belt = eT / eT.max()
    row = np.interp(ANGLES + peak, ANGLES, belt)
    coef, res = nnls(np.stack([belt, row], 1), eI)
    return coef, res / np.linalg.norm(eI), peak, ANGLES[eI.argmax()]


def rows(out, cid):
    c = benchset.by_id(cid)
    d = os.path.join(out, cid)
    fr = bayer.read_frames(benchset.nas(c.video), c.size, TRIAGE_FRAMES).astype(float)
    fr = fr[:, :fr.shape[1] // 2 * 2, :fr.shape[2] // 2 * 2]
    fl = ndimage.uniform_filter(fr, size=(1, 2, 2))        # 2x2 box: every CFA site in each sample
    mean = fl.mean(0)
    mask = ndimage.binary_erosion(metric.target_mask(mean), iterations=4)
    diffs = [fl[t + 1] - fl[t] for t in range(fl.shape[0] - 1)]
    b_raw, n_raw = sky_banding(diffs, mask)
    sky = ~ndimage.binary_dilation(metric.target_mask(mean), iterations=12)
    print(f"{cid} raw: sky {np.median(mean[sky]):.1f} DN, disc {np.median(mean[mask]):.1f} DN; additive row banding "
          f"cov(sky left, sky right) of lag-1 difference row means {b_raw:.3g} DN^2 over {n_raw} disc rows x "
          f"{len(diffs)} differences")
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = ys.min() - 4, ys.max() + 5, xs.min() - 4, xs.max() + 5
    mk, m = mask[y0:y1, x0:x1], mean[y0:y1, x0:x1]
    vT = angle_profile((m - ndimage.gaussian_filter(m, 4)) / m, mk)
    vR = sum(angle_profile((fl[t + 1, y0:y1, x0:x1] - fl[t, y0:y1, x0:x1]) / m, mk) for t in range(0, len(diffs), 2))
    coef, res, peak, pk = split_by_angle(vT, vR)
    print(f"  raw relative lag-1 differences on the disc: row structure peaks at {pk:+.0f} deg (belts {peak:+.0f}); "
          f"sensor-row share {coef[1] / coef.sum():.2f} (residual {res:.2f})")

    A = metric.luminance(refs.load_image(os.path.join(d, "halfA.fits")))
    B = metric.luminance(refs.load_image(os.path.join(d, "halfB.fits")))
    _, box, reg, coef, _ = halves.refined_difference(A, B, tile=None)   # D as rebuilt below: the global refinement
    Bs = np.real(np.fft.ifft2(ndimage.fourier_shift(np.fft.fft2(refs.apply_orientation(B, reg.orientation)), reg.shift)))
    gy, gx = np.gradient(A)
    D = (A - A.mean() - coef[0] * (Bs - Bs.mean()) - coef[1] * gy - coef[2] * gx) / 2   # the refinement, full frame
    mA = ndimage.binary_erosion(metric.target_mask(A), iterations=4)
    mA[:8] = mA[-8:] = False
    b_sky, n = sky_banding([D], mA)
    CL, CR = [], []
    for y in np.flatnonzero(mA.any(1)):
        xs = np.flatnonzero(mA[y])
        if xs.size >= 8:
            mid = (xs[0] + xs[-1]) // 2
            CL.append(D[y, xs[0]:mid].mean())
            CR.append(D[y, mid:xs[-1] + 1].mean())
    b_disc = _cov(np.array(CL), np.array(CR))
    print(f"  D: cov(sky left, sky right) {b_sky:.3g} vs cov(disc left half, disc right half) {b_disc:.3g} "
          f"(stack units^2, {n} rows): additive banding {b_sky / b_disc:.1e} of the disc's shared row structure")
    vA = angle_profile(A - ndimage.gaussian_filter(A, 4), mA)
    vD = angle_profile(D, mA)
    coef, res, peak, pk = split_by_angle(vA, vD)
    print(f"  D on the disc: row structure peaks at {pk:+.0f} deg (belts {peak:+.0f}); sensor-row share "
          f"{coef[1] / coef.sum():.2f} (residual {res:.2f})")


def _bandpass(x, lo=0.06, hi=0.30):
    F = np.fft.fft2(x - x.mean())
    f = np.hypot(np.fft.fftfreq(x.shape[0])[:, None], np.fft.fftfreq(x.shape[1])[None, :])
    return np.real(np.fft.ifft2(F * ((f >= lo) & (f < hi))))


def gain_only_difference(A, B):
    """(D on A's target box, A there, the box): B registered to A as refined_difference does (refs.register,
    then an exact Fourier shift), then D = (A - g B) / 2 with g the one least-squares gain of the mean-free
    box -- no global shift fit, which would absorb a smooth gain field as a pseudo-shift (module doc)."""
    reg = refs.register(B, A)
    Bs = np.real(np.fft.ifft2(ndimage.fourier_shift(np.fft.fft2(refs.apply_orientation(B, reg.orientation)), reg.shift)))
    bx = metric.target_box(A)
    a, b = A[bx.y0:bx.y1, bx.x0:bx.x1], Bs[bx.y0:bx.y1, bx.x0:bx.x1]
    am, bm = a - a.mean(), b - b.mean()
    g = float(am.ravel() @ bm.ravel()) / float(bm.ravel() @ bm.ravel())
    return (am - g * bm) / 2, a, bx


def decompose(A, B, tile=32):
    """Shares of D's END-band power explained tile by tile by a gain on A, a local shift, a local blur
    (D after the registration and one global gain: gain_only_difference)."""
    dd, a, bx = gain_only_difference(A, B)
    gy, gx = np.gradient(a)
    lap = np.gradient(gy, axis=0) + np.gradient(gx, axis=1)
    Ab, Db, Gy, Gx, L = (_bandpass(v) for v in (a, dd, gy, gx, lap))
    groups = {"gain": [Ab], "shift": [Gy, Gx], "blur": [L], "all": [Ab, Gy, Gx, L]}
    tot, P = dict.fromkeys(groups, 0.0), 0.0
    for y in range(0, Db.shape[0] - tile + 1, tile):
        for x in range(0, Db.shape[1] - tile + 1, tile):
            s = (slice(y, y + tile), slice(x, x + tile))
            yv = Db[s].ravel()
            P += float(yv @ yv)
            for k, regs in groups.items():
                X = np.stack([r[s].ravel() for r in regs], 1)
                cf, *_ = np.linalg.lstsq(X, yv, rcond=None)
                e = yv - X @ cf
                tot[k] += float(yv @ yv - e @ e)
    if P == 0:
        raise ValueError(f"the box {list(bx)} holds no {tile} px tile")
    return {k: v / P for k, v in tot.items()}


def halves_triage(out, cid, work, rprobe):
    c = benchset.by_id(cid)
    d = os.path.join(out, cid)
    os.makedirs(work, exist_ok=True)
    even, odd = os.path.join(work, "even.ser"), os.path.join(work, "odd.ser")
    if not (os.path.exists(even) and os.path.exists(odd)):
        halves.split(benchset.nas(c.video), c.size, even, odd)
    A0 = metric.luminance(refs.load_image(os.path.join(d, "halfA.fits")))
    box = metric.target_box(A0)
    boxes = {"A": box, "B": halves.box_in_b(A0, metric.luminance(refs.load_image(os.path.join(d, "halfB.fits"))), box)}
    kj = {}
    for h, ser in (("A", even), ("B", odd)):
        hd = os.path.join(work, h)
        os.makedirs(hd, exist_ok=True)
        b = boxes[h]
        # The boxes are on the bench's cropped half-stacks; the region probe's grid -- and its --fits-out stacks,
        # which ratio() below reads -- is not cropped: it reads the box at the half's crop origin (Ruling R14).
        with open(os.path.join(d, f"half{h}.json")) as fh:
            origin = benchset.crop_origin(json.load(fh))
        if origin is None:
            raise RuntimeError(f"{cid}: half{h}.json gives no crop")
        with open(os.path.join(hd, "probe.log"), "w") as log:
            subprocess.run([rprobe, ser, "--bayer", "RGGB", "--keff-box", f"{b.y0},{b.y1},{b.x0},{b.x1}",
                            "--keff-origin", f"{origin[0]},{origin[1]}",
                            "--json-out", os.path.join(hd, "keff.json"), "--fits-out", hd], check=True, stdout=log)
        with open(os.path.join(hd, "keff.json")) as fh:
            kj[h] = json.load(fh)
    ptc, rm = noise.measure_video(benchset.nas(c.video), c.size)
    cal = halves.load_calibration()["kinds"][halves.kind_of(c.kind)]
    print(f"{cid}: {ptc}; box {list(box)}; calibrated 0.21-0.30 x{cal['high_mean']:.2f}, tolerance "
          f"{cal['tolerance'][0]:.2f}-{cal['tolerance'][1]:.2f}")
    for name, fn, key in (("region stacks", "ours.fits", "keffBox"), ("global-only stacks", "global.fits", "keffBoxGlobal")):
        pa, pb = os.path.join(work, "A", fn), os.path.join(work, "B", fn)
        r = halves.ratio(pa, pb, kj["A"][key], kj["B"][key], ptc, rm)
        A = metric.luminance(refs.load_image(pa))
        B = metric.luminance(refs.load_image(pb))
        try:
            sh = decompose(A, B)
            dec = (f"; D's END-band power explained by a gain on A {sh['gain']:.3f}, a local shift {sh['shift']:.3f}, "
                   f"a local blur {sh['blur']:.3f}, all {sh['all']:.3f}")
        except ValueError as e:
            dec = f"; no decomposition: {e}"
        print(f"  {name}: k {kj['A'][key]:.0f}/{kj['B'][key]:.0f}; 0.21-0.30 x{r['high']:.2f}; per END bin "
              + " ".join(f"{v:.2f}" for v in r["ratio"][:8]) + dec)


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rows")
    r.add_argument("out")
    r.add_argument("capture")
    h = sub.add_parser("halves")
    h.add_argument("out")
    h.add_argument("capture")
    h.add_argument("work")
    h.add_argument("--region-probe", default=os.path.join(HERE, "..", "..", "build", "test", "stack-region-probe"))
    a = ap.parse_args(argv)
    if a.cmd == "rows":
        rows(a.out, a.capture)
    else:
        halves_triage(a.out, a.capture, a.work, a.region_probe)
    return 0


if __name__ == "__main__":
    sys.exit(_main())
