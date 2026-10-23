#!/usr/bin/env python3
"""halves.py -- split-half validation of END's noise model (Ruling P28, rule 3). Validation only:
nothing here feeds END.

    python3 -I halves.py split <video.avi> WxH <even.ser> <odd.ser>   # run-stack-bench.sh
    python3 -I halves.py calibrate <work-dir> [--stack-probe P] [--region-probe P]

SPLIT. The capture's even frames go to one SER and its odd frames to another (8-bit, BAYER_RGGB
header; the bench passes --bayer explicitly anyway), the frames exactly as ffmpeg decodes them
(bayer.read_frames: the reader's layout). The two halves share no frame, so their stacks share no
random noise.

THE RATIO (ratio). Stack A (even) and stack B (odd) are each stacked by the engine. B is registered
to A (refs.register) and moved onto A's grid by an exact Fourier shift (it keeps B's power spectrum);
D = (A - B) / 2, after B's gain, a residual global shift and each 32 px tile's local shift field are
removed (refined_difference; the tiles: Ruling P28d, fix round 4 -- the halves' region stacks register
differently region by region, and that is registration, not noise). Where the halves agree on the
signal, D is noise only, of power (N_A + N_B) / 4.
END's model says N = sigma_frame^2 / k, so D's model is sigma_frame^2 (1/k_A + 1/k_B) / 4 on A's
target box: sigma_frame^2 from the raw frames (noise.box_noise), k_A and k_B each half's own
noise-power k over the box (stack-region-probe --keff-box, the judged reading; the mean-kept-fraction
reading beside it). The measured / model ratio is taken per END bin and summarised over
0.21-0.30 cycles/px, where END's restoration error is decided.

THE CALIBRATION (calibrate; Ruling P28 fix rounds 1-4). The engine is run on synthetic RGGB captures
whose truth is known -- a banded disc and a lunar field, static, 6 scenes per kind (CAL_SEEDS) each at
0.3 and 0.7 px of per-frame jitter (CAL_JITTERS), each without and with a 3 DN black pedestal
(CAL_PEDESTALS; re-review 3 N3-m6: every real capture but one fits a negative intercept), 8-bit,
noise of variance a s + c, 600 frames -- split even/odd, stacked with stack-probe, both halves' k over
A's target box from stack-region-probe, and the ratio taken with the MEASURED photon-transfer line
(noise.photon_transfer on the first FRAMES frames, as a real capture is measured; review I-F), so the
estimator's read-high (+7 % on a disc) sits in R_cal as it sits in R. D holds the engine's own noise
only (the synthetics are static: the halves never choose different frames, so the real-data term
"unshared signal" is absent by construction -- on real data it can only raise R). Per kind it gives
R_cal(f), the engine's honest ratio per END bin (the mean over every run), and the tolerance over
0.21-0.30 (tolerance): exp(mean -+ k s) of log R (s ddof 1; on log R since fix round 4, N3-m1), k the
two-sided normal tolerance factor at 99 % coverage and 95 % confidence for n_eff = the number of SCENES
(the scene, not the jitter or the pedestal, dominates the spread: review N2-m2).
`calibrate --write` freezes it in bench/calibration.json.
What is guaranteed about real data: calibrate opens no capture (synthetic_frames only), and
calibration.json is committed on its own, before the real-data run that uses it -- git history shows
the order. What is NOT claimed: that the procedure predates real data. Earlier rounds read real
split-half ratios, and the refinement in refined_difference was adopted after Mars's real x10.3
(review N2-m7).

USE. bench.status_of: a capture whose 0.21-0.30 ratio lies outside its kind's tolerance, in either
direction, cannot pass (review I-G) -- above it the model does not hold what the halves measure, below
it ours smooths more than the honest engine or sigma_frame is over-read, and either way the charge is
not confirmed. Ours is charged its measured noise (Ruling P28b): N_ours(f) = budget x
max(1, R(f) / R_cal(f)) per END bin (charge). The references stay on the budget.
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import benchset  # noqa: E402
import metric    # noqa: E402
import noise     # noqa: E402
import refs      # noqa: E402

BAYER_RGGB = 8
HIGH_BINS = slice(5, 8)          # END_EDGES bins 0.21-0.24, 0.24-0.27, 0.27-0.30
CALIBRATION_FILE = os.path.join(HERE, "calibration.json")
CAL_SEEDS = (1, 2, 3, 4, 5, 6)        # scenes per kind (each at every CAL_JITTERS and CAL_PEDESTALS)
CAL_JITTERS = (0.3, 0.7)              # px of per-frame jitter
CAL_PEDESTALS = (0.0, 3.0)            # DN of black offset (re-review 3 N3-m6: the real captures' regime)
CAL_SHOT, CAL_READ, CAL_PEDESTAL_READ = 0.15, 0.6, 0.05   # noise a s + c; c with the pedestal (a sky that rounds flat)
COVERAGE, CONFIDENCE = 0.99, 0.95     # the tolerance interval: 99 % of honest runs, at 95 % confidence
TILE = 32                             # px: refined_difference removes each tile's local shift (Ruling P28d)


def tolerance_factor(n, coverage=COVERAGE, confidence=CONFIDENCE):
    """Two-sided normal tolerance factor (Howe 1969): mean -+ k s covers `coverage` of the population
    with `confidence`, from n samples (s with ddof = 1). n = 8 at 99 % / 95 %: 4.91."""
    from scipy import stats
    z = stats.norm.ppf((1 + coverage) / 2)
    chi2 = stats.chi2.ppf(1 - confidence, n - 1)
    return math.sqrt((n - 1) * (1 + 1 / n) * z * z / chi2)


def load_calibration(path=CALIBRATION_FILE):
    """The frozen calibration (calibrate --write): per target kind ("disc" for planets, "lunar"), the
    engine's honest measured / model ratio per END bin (r_cal) and the tolerance over 0.21-0.30."""
    with open(path) as fh:
        return json.load(fh)


def kind_of(capture_kind: str) -> str:
    return "disc" if capture_kind == "planet" else "lunar"


def agreement(kind: str, cal=None):
    cal = cal or load_calibration()
    return tuple(cal["kinds"][kind]["tolerance"])


def _ser_header(w, h, n):
    return (b"LUCAM-RECORDER" + struct.pack("<7i", 0, BAYER_RGGB, 0, w, h, 8, n) + b"\0" * 120
            + struct.pack("<2q", 0, 0))


def write_ser(path, frames):
    frames = np.asarray(frames, dtype=np.uint8)
    n, h, w = frames.shape
    with open(path, "wb") as fh:
        fh.write(_ser_header(w, h, n))
        fh.write(frames.tobytes())


def split(video, size, even_path, odd_path):
    """Even frames to even_path, odd to odd_path (SER, 8-bit). Returns (even count, odd count)."""
    w, h = size
    proc = subprocess.Popen(["ffmpeg", "-v", "error", "-nostdin", "-i", video, "-f", "rawvideo",
                             "-pix_fmt", "gray", "-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    n = [0, 0]
    with open(even_path, "wb") as fa, open(odd_path, "wb") as fb:
        fa.write(_ser_header(w, h, 0))
        fb.write(_ser_header(w, h, 0))
        i = 0
        while True:
            buf = proc.stdout.read(w * h)
            if len(buf) < w * h:
                break
            (fa if i % 2 == 0 else fb).write(buf)
            n[i % 2] += 1
            i += 1
        proc.wait()
        for f, k in ((fa, n[0]), (fb, n[1])):
            f.seek(0)
            f.write(_ser_header(w, h, k))
    if min(n) == 0:
        raise ValueError(f"{video}: split gave {n} frames")
    return tuple(n)


def _tile_edges(n, tile):
    """Tile starts over 0..n: whole `tile`-px tiles, the remainder (under a tile) joined to the last."""
    k = max(1, n // tile)
    return [i * tile for i in range(k)] + [n]


def remove_tile_shifts(e, gy, gx, tile=TILE):
    """(e minus each tile's local shift, the per-tile (dy, dx) at its centre in px). Per `tile`-px tile
    of the box (_tile_edges) the local shift is fitted as a first-order field, dy(u, v) = dy0 + dy_u u +
    dy_v v and dx likewise (u, v the tile's centred coordinates over half a tile), by least squares:
    e ~ c + dy(u, v) dA/dy + dx(u, v) dA/dx. Only the shift terms are removed; the intercept c is
    fitted so a local level offset is not read as a shift, and it stays in e, because a level offset
    between the halves is not registration (Ruling P28d).
    Why first order, not one constant shift per tile (fix round 3's estimate): the engine blends its
    regions with sin^2 weights on a grid of half-overlapping windows (Regions.h), so the halves' shift
    difference varies smoothly from node to node, 64 px apart on a 1080p surface. Measured on a
    synthetic pair with that field (nodes 64 px apart, 0.1 px sd per node; test_end.py's P28d tests):
    the 0.06-0.09 bin reads x4.75 the noise unremoved, x2.77 with one shift per tile, x1.09 first order.
    Cost: 6 of each tile's 1024 noise degrees of freedom; on noise alone the 0.06-0.09 bin reads x0.90
    against x0.97 unremoved, the 0.15-0.30 bins within 1 % (the calibration measures with it)."""
    out, shifts = e.copy(), []
    ys, xs = _tile_edges(e.shape[0], tile), _tile_edges(e.shape[1], tile)
    for y0, y1 in zip(ys[:-1], ys[1:]):
        for x0, x1 in zip(xs[:-1], xs[1:]):
            s = (slice(y0, y1), slice(x0, x1))
            u, v = np.mgrid[y0:y1, x0:x1].astype(float)
            u, v = (u - u.mean()) / (tile / 2), (v - v.mean()) / (tile / 2)
            terms = [gy[s], u * gy[s], v * gy[s], gx[s], u * gx[s], v * gx[s]]
            X = np.stack([np.ones(e[s].size)] + [t.ravel() for t in terms], 1)
            c, *_ = np.linalg.lstsq(X, e[s].ravel(), rcond=None)
            out[s] = e[s] - (X[:, 1:] @ c[1:]).reshape(e[s].shape)
            shifts.append((c[1], c[4]))
    return out, np.array(shifts)


def refined_difference(A, B, tile=TILE):
    """(D, box, registration, coef, tile shifts): B registered to A (refs.register, then an exact
    Fourier shift, which keeps B's power spectrum), and then a least-squares refinement on A's target
    box -- B's gain and a residual sub-pixel shift (A ~ g B + dy dA/dy + dx dA/dx), removed before
    D = (A - g B') / 2. Without it a residual misregistration of hundredths of a pixel leaks the signal
    into D: on the 13 px Mars disc 0.017 px of it read as x10 the model (fix round 2, review I-E).
    Then each `tile`-px tile's own residual shift field is removed (remove_tile_shifts; Ruling P28d;
    tile=None skips it): the halves' region stacks register differently from region to region, and that
    local shift is registration, which the full stack carries as a geometric offset END does not see,
    not noise (fix round 3 triage: a local shift was 85 % of the 2024-02-01 Moon's END-band D). The
    calibration measures with it, so its small cost in noise sits in R_cal as it sits in R."""
    reg = refs.register(B, A)
    Bo = refs.apply_orientation(B, reg.orientation)
    Bs = np.real(np.fft.ifft2(ndimage.fourier_shift(np.fft.fft2(Bo), reg.shift)))
    box = metric.target_box(A)
    a, b = A[box.y0:box.y1, box.x0:box.x1], Bs[box.y0:box.y1, box.x0:box.x1]
    am, bm = a - a.mean(), b - b.mean()
    gy, gx = np.gradient(a)
    X = np.stack([bm.ravel(), gy.ravel(), gx.ravel()], 1)
    coef, *_ = np.linalg.lstsq(X, am.ravel(), rcond=None)
    e = am - (X @ coef).reshape(a.shape)
    shifts = np.zeros((0, 2))
    if tile:
        e, shifts = remove_tile_shifts(e, gy, gx, tile)
    D = np.zeros_like(A)
    D[box.y0:box.y1, box.x0:box.x1] = e / 2
    return D, box, reg, coef, shifts


def ratio(path_a, path_b, k_a, k_b, ptc, raw_mean):
    """Per END bin (and 0.40-0.50 last): D's power over the model sigma_frame^2 (1/k_a + 1/k_b) / 4."""
    A = metric.luminance(refs.load_image(path_a))
    B = metric.luminance(refs.load_image(path_b))
    D, box, reg, coef, shifts = refined_difference(A, B)
    s = metric.measure(A, box).signal
    p = metric.periodogram(D[box.y0:box.y1, box.x0:box.x1] / s)
    d = metric.band_means(p, metric.END_EDGES) + metric.band_means(p, metric.NOISE_BAND)
    s2 = noise.box_noise(ptc, noise.raw_under(raw_mean, A, box))
    model = s2 * (1 / k_a + 1 / k_b) / 4
    mag = np.hypot(shifts[:, 0], shifts[:, 1]) if len(shifts) else np.zeros(1)
    return {"box": list(box), "shift": list(reg.shift), "refine": [float(v) for v in coef], "sigma_frame2": s2,
            "tiles": {"n": int(len(shifts)), "shift_rms": float(np.sqrt(np.mean(mag ** 2))), "shift_max": float(mag.max())},
            "model": model, "ratio": [v / model for v in d], "high": float(np.mean([v / model for v in d[HIGH_BINS]]))}


def _k_of(run_dir, half, box):
    """(judged k, mean-kept k, why) for a half from its stack-probe and stack-region-probe JSON; the
    region probe's box must be `box` (A's target box, where D is measured, in that half's own grid:
    for B, A's box moved by the registration's integer shift; reviews N-m3, N2-m5)."""
    with open(os.path.join(run_dir, f"half{half}.json")) as fh:
        probe = json.load(fh)
    keff_path = os.path.join(run_dir, f"half{half}.keff.json")
    keff = None
    if os.path.exists(keff_path):
        with open(keff_path) as fh:
            keff = json.load(fh)
        if list(keff.get("box", [])) != list(box):
            return None, None, f"half {half}'s k was taken over {keff.get('box')}, not A's box {list(box)}"
    k, why = benchset.ours_frames_box(probe, keff)
    return k, benchset.ours_frames_mean(probe), why


def box_in_b(A, B, box):
    """A's box in B's grid: B registered to A (anchor[y, x] ~ B[y - dy, x - dx]), so A's pixel (y, x)
    is B's (y - dy, x - dx), rounded to whole pixels (review N2-m5)."""
    dy, dx = refs.register(B, A).int_shift
    return metric.Box(box.y0 - dy, box.y1 - dy, box.x0 - dx, box.x1 - dx)


def charge(ratio_per_bin, r_cal):
    """Ruling P28b: our noise charge per END bin, max(1, R(f) / R_cal(f)) -- the split halves' measured
    / model over the engine's calibrated honest ratio for the target's kind."""
    n = len(metric.END_EDGES) - 1
    return [max(1.0, float(r) / float(c)) for r, c in zip(ratio_per_bin[:n], r_cal[:n])]


def check(run_dir, frame_noise, kind):
    """The split-half record of a run directory holding halfA/halfB .fits + .json + .keff.json, or None
    when the halves were not stacked. `kind`: "disc" or "lunar" (kind_of). The record carries the
    measured ratio per END bin with each reading of k, the tolerance verdict, and the per-bin charge
    for our noise (charge)."""
    pa, pb = (os.path.join(run_dir, f"half{h}.fits") for h in "AB")
    if not all(os.path.exists(p) for p in (pa, pb, os.path.join(run_dir, "halfA.json"),
                                          os.path.join(run_dir, "halfB.json"))):
        return None
    cal = load_calibration()["kinds"][kind]
    out = {"kind": kind, "tolerance": list(cal["tolerance"])}
    if frame_noise is None or isinstance(frame_noise, str):
        return dict(out, status="unmeasured", reason=f"no per-frame noise: {frame_noise}")
    ptc, raw_mean = frame_noise
    A = metric.luminance(refs.load_image(pa))
    box = metric.target_box(A)
    box_b = box_in_b(A, metric.luminance(refs.load_image(pb)), box)
    (ka, ka_m, why_a), (kb, kb_m, why_b) = _k_of(run_dir, "A", box), _k_of(run_dir, "B", box_b)
    if ka is None or kb is None:
        return dict(out, status="unmeasured", reason=f"halves' k: {why_a or why_b}")
    r = ratio(pa, pb, ka, kb, ptc, raw_mean)
    rm = ratio(pa, pb, ka_m, kb_m, ptc, raw_mean) if (ka_m and kb_m) else None
    lo, hi = cal["tolerance"]
    out.update(status="agrees" if lo <= r["high"] <= hi else "outside", judged=dict(r, k=[ka, kb]),
               mean_k=(dict(rm, k=[ka_m, kb_m]) if rm else None),
               factor=charge(r["ratio"], cal["r_cal"]),
               factor_alt=(charge(rm["ratio"], cal["r_cal"]) if rm else None))
    return out


def summary_line(rec) -> str:
    if not rec:
        return "not stacked (run-stack-bench.sh --no-split)"
    if rec.get("status") not in ("agrees", "outside"):
        return f"{rec.get('status')}: {rec.get('reason', '')}"
    j, (lo, hi) = rec["judged"], rec["tolerance"]
    line = (f"**{rec['status']}** ({rec['kind']} {lo:.2f}-{hi:.2f}): 0.21-0.30 x{j['high']:.2f} (k {j['k'][0]:.0f}/"
            f"{j['k'][1]:.0f}), 0.06-0.09 x{j['ratio'][0]:.2f}, 0.40-0.50 x{j['ratio'][-1]:.2f}; our charge per bin "
            + " ".join(f"{v:.2f}" for v in rec["factor"]))
    m = rec.get("mean_k")
    if m:
        line += f"; mean-kept k {m['k'][0]:.0f}/{m['k'][1]:.0f}: 0.21-0.30 x{m['high']:.2f}"
    return line


# --- calibration: the engine's own ratio on synthetic captures ---------------------------------

def synthetic_frames(kind, n, jitter, a, c, seed, blur=None, pedestal=0.0):
    """(frames n x h x w uint8 RGGB mosaic, mean frame in DN (noise-free, pedestal included),
    PhotonTransfer truth). Noise of variance a s + c on the signal s.
    `blur`: None (every frame at the scene's own seeing, as the calibration uses), or n per-frame
    Gaussian sigmas in px applied on top -- per-frame seeing, a real reason for the engine to keep a
    subset (ruling E3). The jitter and noise draws are the same either way.
    `pedestal` b > 0: the capture regime of every real bench capture but one (re-review 3 N3-m6) -- the
    sky has no signal (a 4 / 6 DN sky becomes 0 / a 0-2 DN strip), b DN of black offset is added after
    the noise, so a sky with a small c rounds to one 8-bit value, and the line is v = a m + (c + 1/12 -
    a b): a negative intercept, as the real captures fit (-0.03 to -3.17)."""
    from conftest import lunar_scene, planet_scene
    rng = np.random.default_rng(seed)
    sky = 0.0 if pedestal else (4.0 if kind == "disc" else 6.0)
    if kind == "disc":
        scene = sky + 200 * planet_scene(size=(240, 320), radius=55, seed=seed, seeing=1.2)
    else:
        scene = sky + 180 * lunar_scene(size=(288, 384), seed=seed, seeing=1.2)
        scene[:, :60] = sky + 2 * rng.random((scene.shape[0], 60))   # sky beside a limb
        scene = ndimage.gaussian_filter(scene, 0.8)
    gains = np.empty_like(scene)
    gains[0::2, 0::2], gains[0::2, 1::2], gains[1::2, 0::2], gains[1::2, 1::2] = 1.0, 0.8, 0.8, 0.55
    # Shift the scene's mirror (even) extension, which is periodic without a step, and crop it back: a
    # circular shift of the scene itself wraps one edge onto the other (the lunar sky strip onto its
    # bright right edge), and a frame moved by ~0.5 px gains a hard edge the scene does not have.
    h, w = scene.shape
    F = np.fft.fft2(np.pad(scene, ((0, h), (0, w)), mode="symmetric"))
    if blur is not None:
        blur = np.asarray(blur, dtype=float)
        if blur.shape != (n,) or (blur < 0).any():
            raise ValueError(f"blur: {n} sigmas >= 0, not {blur.shape}")
        f2 = np.fft.fftfreq(2 * h)[:, None] ** 2 + np.fft.fftfreq(2 * w)[None, :] ** 2
    frames, total = [], np.zeros_like(scene)
    for i in range(n):
        Fi = F if blur is None or blur[i] == 0 else F * np.exp(-2 * np.pi ** 2 * blur[i] ** 2 * f2)
        shifted = np.real(np.fft.ifft2(ndimage.fourier_shift(Fi, rng.normal(0, jitter, 2))))[:h, :w] * gains
        total += shifted + pedestal
        noisy = shifted + rng.standard_normal(scene.shape) * np.sqrt(a * np.clip(shifted, 0, None) + c)
        frames.append(np.clip(np.round(noisy + pedestal), 0, 255))
    ptc = noise.PhotonTransfer(a, c + noise.QUANTISATION - a * pedestal, (), 0, n - 1)
    return np.array(frames, dtype=np.uint8), total / n, ptc


def _run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed ({r.returncode}): {r.stderr[-400:]}")
    return r.stdout


def calibration_conditions():
    """(kind, jitter, scene, pedestal) of every calibration run: each scene at each jitter, without and
    with the black pedestal (CAL_PEDESTALS; re-review 3 N3-m6)."""
    return [(k, j, s, p) for k in ("disc", "lunar") for j in CAL_JITTERS for s in CAL_SEEDS for p in CAL_PEDESTALS]


def condition_tag(kind, jitter, seed, pedestal=0.0):
    return f"{kind}-j{jitter}-s{seed}" + (f"-p{pedestal:g}" if pedestal else "")


def tolerance(highs, n_eff):
    """(lo, hi, log mean, log sd, factor): the tolerance interval of the 0.21-0.30 ratio, taken on log R
    (re-review 3 N3-m1): R is a ratio of powers, positive, and a scene's leak scales it (x1.5 noise
    uncharged reads x1.5), so its spread is multiplicative. On the linear scale mean - k sd ran below 0
    for lunar (0.866 - 5.81 x 0.152 in fix round 3), and a band whose lower end is 0 can never block a
    capture from below (review I-G). exp(mean log R -+ k sd log R) is a real bound at both ends."""
    lg = np.log(np.asarray(highs, dtype=float))
    m, sd, kf = float(lg.mean()), float(lg.std(ddof=1)), tolerance_factor(n_eff)
    return math.exp(m - kf * sd), math.exp(m + kf * sd), m, sd, kf


def calibrate(work, stack_probe, region_probe, conditions=None):
    """The engine's measured / model over 0.21-0.30 on synthetic captures (see THE CALIBRATION)."""
    os.makedirs(work, exist_ok=True)
    conditions = conditions or calibration_conditions()
    out = []
    for kind, jitter, seed, pedestal in conditions:
        c = CAL_PEDESTAL_READ if pedestal else CAL_READ
        frames, mean, truth = synthetic_frames(kind, 600, jitter, CAL_SHOT, c, seed, pedestal=pedestal)
        # The MEASURED photon-transfer line, from the capture's first frames as noise.measure_video takes
        # a real capture's (review I-F): the estimator's own bias then sits in R_cal as it does in R.
        ptc = noise.photon_transfer(frames[:noise.FRAMES])
        tag = condition_tag(kind, jitter, seed, pedestal)
        k, probes, sers, fits = {}, [], {}, {}
        for half, sl in (("A", slice(0, None, 2)), ("B", slice(1, None, 2))):
            sers[half] = os.path.join(work, f"{tag}-{half}.ser")
            write_ser(sers[half], frames[sl])
            fits[half] = os.path.join(work, f"{tag}-{half}.fits")
            probes.append(json.loads(_run([stack_probe, sers[half], "--bayer", "RGGB", "--out", fits[half], "--json"])))
        A_lum = metric.luminance(refs.load_image(fits["A"]))
        box = metric.target_box(A_lum)          # both halves' k over A's box (in B's grid for B)
        boxes = {"A": box, "B": box_in_b(A_lum, metric.luminance(refs.load_image(fits["B"])), box)}
        for half, probe in zip("AB", probes):
            b = boxes[half]
            kj = os.path.join(work, f"{tag}-{half}.keff.json")
            _run([region_probe, sers[half], "--bayer", "RGGB", "--plan-only", "--keff-box",
                  f"{b.y0},{b.y1},{b.x0},{b.x1}", "--json-out", kj])
            with open(kj) as fh:
                kv, why = benchset.ours_frames_box(probe, json.load(fh))
            if kv is None:
                raise RuntimeError(f"{tag} half {half}: {why}")
            k[half] = kv
            os.remove(sers[half])
        r = ratio(os.path.join(work, f"{tag}-A.fits"), os.path.join(work, f"{tag}-B.fits"), k["A"], k["B"], ptc, mean)
        out.append({"condition": tag, "kind": kind, "scene": seed, "jitter": jitter, "pedestal": pedestal,
                    "k": [k["A"], k["B"]], "high": r["high"], "ratio": r["ratio"], "tiles": r["tiles"],
                    "ptc": [ptc.a, ptc.c], "ptc_true": [truth.a, truth.c],
                    "mean_kept_k": [benchset.ours_frames_mean(pr) for pr in probes]})
        print(f"{tag}: k {k['A']:.0f}/{k['B']:.0f}, 0.21-0.30 x{r['high']:.3f}, per bin "
              + " ".join(f"{v:.2f}" for v in r["ratio"]), flush=True)
    return out


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("split")
    s.add_argument("video")
    s.add_argument("size", help="WxH")
    s.add_argument("even")
    s.add_argument("odd")
    c = sub.add_parser("calibrate")
    c.add_argument("work")
    build = os.path.join(os.path.dirname(os.path.dirname(HERE)), "build", "test")
    c.add_argument("--stack-probe", default=os.path.join(build, "stack-probe"))
    c.add_argument("--region-probe", default=os.path.join(build, "stack-region-probe"))
    c.add_argument("--write", help="write the frozen calibration JSON (bench/calibration.json)")
    a = ap.parse_args(argv)
    if a.cmd == "split":
        w, h = (int(v) for v in a.size.lower().split("x"))
        n = split(a.video, (w, h), a.even, a.odd)
        print(f"even {n[0]} odd {n[1]} frames")
        return 0
    res = calibrate(a.work, a.stack_probe, a.region_probe)
    kinds = {}
    for kind in ("disc", "lunar"):
        rs = [r for r in res if r["kind"] == kind]
        hs = np.array([r["high"] for r in rs])
        n, m, sd = len(hs), float(hs.mean()), float(hs.std(ddof=1))
        n_eff = len({r["scene"] for r in rs})      # the scene dominates the spread (review N2-m2)
        lo, hi, lm, lsd, kf = tolerance(hs, n_eff)
        by_ped = {f"{p:g}": float(np.mean([r["high"] for r in rs if r["pedestal"] == p])) for p in CAL_PEDESTALS}
        kinds[kind] = {"n": n, "n_eff": n_eff, "high_mean": m, "high_sd": sd, "high_log_mean": lm, "high_log_sd": lsd,
                       "high_min": float(hs.min()), "high_max": float(hs.max()), "high_mean_by_pedestal": by_ped,
                       "tolerance_factor": kf, "tolerance": [lo, hi],
                       "r_cal": [float(v) for v in np.mean([r["ratio"] for r in rs], axis=0)],
                       "runs": rs}
        print(f"{kind}: n {n} (scenes {n_eff}), 0.21-0.30 mean {m:.3f} sd {sd:.3f} (ddof 1), runs {hs.min():.3f}-"
              f"{hs.max():.3f}, mean by pedestal " + ", ".join(f"{p} DN {v:.3f}" for p, v in by_ped.items())
              + f"; log mean {lm:.3f} sd {lsd:.3f}, k {kf:.2f}: tolerance {lo:.3f}-{hi:.3f}; r_cal per bin "
              + " ".join(f"{v:.2f}" for v in kinds[kind]["r_cal"]))
    if a.write:
        import datetime
        with open(a.write, "w") as fh:
            json.dump({"generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                       "by": "halves.py calibrate: synthetic captures only (synthetic_frames), the engine's stack-probe "
                             "and stack-region-probe, the MEASURED photon-transfer line; no real capture is read",
                       "coverage": COVERAGE, "confidence": CONFIDENCE, "bins": "END_EDGES bins, then 0.40-0.50",
                       "kinds": kinds}, fh, indent=1)
        print(f"wrote {a.write}")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
