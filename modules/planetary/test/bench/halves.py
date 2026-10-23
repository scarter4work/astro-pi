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
D = (A - B) / 2. Where the halves agree on the signal, D is noise only, of power (N_A + N_B) / 4.
END's model says N = sigma_frame^2 / k, so D's model is sigma_frame^2 (1/k_A + 1/k_B) / 4 on A's
target box: sigma_frame^2 from the raw frames (noise.box_noise), k_A and k_B each half's own
noise-power k over the box (stack-region-probe --keff-box, the judged reading; the mean-kept-fraction
reading beside it). The measured / model ratio is taken per END bin and summarised over
0.21-0.30 cycles/px, where END's restoration error is decided.

THE CALIBRATION (calibrate; Ruling P28 fix rounds 1-2). The engine is run on synthetic RGGB captures
whose truth is known -- a banded disc and a lunar field, static, each frame jittered (sigma 0.2-0.8 px),
8-bit, noise of variance a m + c, 600 frames, 8 runs per kind -- split even/odd, stacked with
stack-probe, both halves' k over A's target box from stack-region-probe, and the ratio taken with the
TRUE photon-transfer line and the noise-free mean frame. There D holds the engine's own noise only (the
synthetics are static: the halves never choose different frames, so the real-data term "unshared
signal" is absent by construction -- on real data it can only raise R, which over-charges ours). Per
kind it gives R_cal(f), the engine's honest ratio per END bin, and the tolerance over 0.21-0.30: mean
-+ k s (ddof 1), k the two-sided normal tolerance factor for n = 8 at 99 % coverage and 95 % confidence
(4.91). `calibrate --write` freezes it in bench/calibration.json.
How "before any real data is read" is guaranteed: calibrate reads no capture (synthetic_frames only;
nothing here opens the NAS), and calibration.json is committed on its own, before the real-data run
that uses it -- git history shows the order. (Earlier rounds did read real split-half ratios; the
frozen procedure -- conditions, statistics, factor -- does not depend on them.)

USE. bench.status_of sends a capture whose 0.21-0.30 ratio lies outside its kind's tolerance to review
(the noise model is not confirmed there), and ours is charged its measured noise (Ruling P28b):
N_ours(f) = budget x max(1, R(f) / R_cal(f)) per END bin (charge). The references stay on the budget.
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
COVERAGE, CONFIDENCE = 0.99, 0.95     # the tolerance interval: 99 % of honest runs, at 95 % confidence


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


def refined_difference(A, B):
    """(D, box, registration): B registered to A (refs.register, then an exact Fourier shift, which
    keeps B's power spectrum), and then a least-squares refinement on A's target box -- B's gain and a
    residual sub-pixel shift (A ~ g B + dy dA/dy + dx dA/dx), removed before D = (A - g B') / 2.
    Without it a residual misregistration of hundredths of a pixel leaks the signal into D: on the
    13 px Mars disc 0.017 px of it read as x10 the model (fix round 2, review I-E)."""
    reg = refs.register(B, A)
    Bo = refs.apply_orientation(B, reg.orientation)
    Bs = np.real(np.fft.ifft2(ndimage.fourier_shift(np.fft.fft2(Bo), reg.shift)))
    box = metric.target_box(A)
    a, b = A[box.y0:box.y1, box.x0:box.x1], Bs[box.y0:box.y1, box.x0:box.x1]
    am, bm = a - a.mean(), b - b.mean()
    gy, gx = np.gradient(a)
    X = np.stack([bm.ravel(), gy.ravel(), gx.ravel()], 1)
    coef, *_ = np.linalg.lstsq(X, am.ravel(), rcond=None)
    D = np.zeros_like(A)
    D[box.y0:box.y1, box.x0:box.x1] = (am - (X @ coef).reshape(a.shape)) / 2
    return D, box, reg, coef


def ratio(path_a, path_b, k_a, k_b, ptc, raw_mean):
    """Per END bin (and 0.40-0.50 last): D's power over the model sigma_frame^2 (1/k_a + 1/k_b) / 4."""
    A = metric.luminance(refs.load_image(path_a))
    B = metric.luminance(refs.load_image(path_b))
    D, box, reg, coef = refined_difference(A, B)
    s = metric.measure(A, box).signal
    p = metric.periodogram(D[box.y0:box.y1, box.x0:box.x1] / s)
    d = metric.band_means(p, metric.END_EDGES) + metric.band_means(p, metric.NOISE_BAND)
    s2 = noise.box_noise(ptc, noise.raw_under(raw_mean, A, box))
    model = s2 * (1 / k_a + 1 / k_b) / 4
    return {"box": list(box), "shift": list(reg.shift), "refine": [float(v) for v in coef], "sigma_frame2": s2,
            "model": model, "ratio": [v / model for v in d], "high": float(np.mean([v / model for v in d[HIGH_BINS]]))}


def _k_of(run_dir, half, box):
    """(judged k, mean-kept k, why) for a half from its stack-probe and stack-region-probe JSON; the
    region probe's box must be `box` (A's target box, where D is measured; review N-m3)."""
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
    box = metric.target_box(metric.luminance(refs.load_image(pa)))
    (ka, ka_m, why_a), (kb, kb_m, why_b) = _k_of(run_dir, "A", box), _k_of(run_dir, "B", box)
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

def synthetic_frames(kind, n, jitter, a, c, seed):
    """(frames n x h x w uint8 RGGB mosaic, noise-free mean frame in DN, PhotonTransfer truth)."""
    from conftest import lunar_scene, planet_scene
    rng = np.random.default_rng(seed)
    if kind == "disc":
        scene = 4 + 200 * planet_scene(size=(240, 320), radius=55, seed=seed, seeing=1.2)
    else:
        scene = 6 + 180 * lunar_scene(size=(288, 384), seed=seed, seeing=1.2)
        scene[:, :60] = 6 + 2 * rng.random((scene.shape[0], 60))   # sky beside a limb
        scene = ndimage.gaussian_filter(scene, 0.8)
    gains = np.empty_like(scene)
    gains[0::2, 0::2], gains[0::2, 1::2], gains[1::2, 0::2], gains[1::2, 1::2] = 1.0, 0.8, 0.8, 0.55
    F = np.fft.fft2(scene)
    frames, total = [], np.zeros_like(scene)
    for _ in range(n):
        shifted = np.real(np.fft.ifft2(ndimage.fourier_shift(F, rng.normal(0, jitter, 2)))) * gains
        total += shifted
        noisy = shifted + rng.standard_normal(scene.shape) * np.sqrt(a * np.clip(shifted, 0, None) + c)
        frames.append(np.clip(np.round(noisy), 0, 255))
    ptc = noise.PhotonTransfer(a, c + noise.QUANTISATION, (), 0, n - 1)
    return np.array(frames, dtype=np.uint8), total / n, ptc


def _run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed ({r.returncode}): {r.stderr[-400:]}")
    return r.stdout


def calibrate(work, stack_probe, region_probe, conditions=None):
    """The engine's measured / model over 0.21-0.30 on synthetic captures (see THE TOLERANCE)."""
    os.makedirs(work, exist_ok=True)
    conditions = conditions or [(k, j, s) for k in ("disc", "lunar") for j in (0.2, 0.4, 0.6, 0.8) for s in (1, 2)]
    out = []
    for kind, jitter, seed in conditions:
        frames, mean, ptc = synthetic_frames(kind, 600, jitter, 0.15, 0.6, seed)
        tag = f"{kind}-j{jitter}-s{seed}"
        k, probes, box = {}, [], None
        for half, sl in (("A", slice(0, None, 2)), ("B", slice(1, None, 2))):
            ser = os.path.join(work, f"{tag}-{half}.ser")
            write_ser(ser, frames[sl])
            fits = os.path.join(work, f"{tag}-{half}.fits")
            probe = json.loads(_run([stack_probe, ser, "--bayer", "RGGB", "--out", fits, "--json"]))
            probes.append(probe)
            if box is None:              # both halves' k over A's target box, where D is measured
                box = metric.target_box(metric.luminance(refs.load_image(fits)))
            kj = os.path.join(work, f"{tag}-{half}.keff.json")
            _run([region_probe, ser, "--bayer", "RGGB", "--plan-only", "--keff-box",
                  f"{box.y0},{box.y1},{box.x0},{box.x1}", "--json-out", kj])
            with open(kj) as fh:
                kv, why = benchset.ours_frames_box(probe, json.load(fh))
            if kv is None:
                raise RuntimeError(f"{tag} half {half}: {why}")
            k[half] = kv
            os.remove(ser)
        r = ratio(os.path.join(work, f"{tag}-A.fits"), os.path.join(work, f"{tag}-B.fits"), k["A"], k["B"], ptc, mean)
        out.append({"condition": tag, "kind": kind, "k": [k["A"], k["B"]], "high": r["high"], "ratio": r["ratio"],
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
        kf = tolerance_factor(n)
        kinds[kind] = {"n": n, "high_mean": m, "high_sd": sd, "tolerance_factor": kf,
                       "tolerance": [max(m - kf * sd, 0.0), m + kf * sd],
                       "r_cal": [float(v) for v in np.mean([r["ratio"] for r in rs], axis=0)],
                       "runs": rs}
        print(f"{kind}: n {n}, 0.21-0.30 mean {m:.3f} sd {sd:.3f} (ddof 1), k {kf:.2f}: tolerance "
              f"{kinds[kind]['tolerance'][0]:.3f}-{kinds[kind]['tolerance'][1]:.3f}; r_cal per bin "
              + " ".join(f"{v:.2f}" for v in kinds[kind]["r_cal"]))
    if a.write:
        import datetime
        with open(a.write, "w") as fh:
            json.dump({"generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                       "by": "halves.py calibrate: synthetic captures only (synthetic_frames), the engine's stack-probe "
                             "and stack-region-probe; no real capture is read",
                       "coverage": COVERAGE, "confidence": CONFIDENCE, "bins": "END_EDGES bins, then 0.40-0.50",
                       "kinds": kinds}, fh, indent=1)
        print(f"wrote {a.write}")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
