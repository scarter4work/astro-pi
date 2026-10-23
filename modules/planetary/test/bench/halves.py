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

THE TOLERANCE (AGREEMENT; Ruling P28 fix round 1, ruling 3), stated before any real capture is read
and derived from the engine itself: `calibrate` builds synthetic RGGB captures whose truth is known
-- a banded disc and a lunar field, static, each frame jittered (sigma 0.3 and 0.8 px), 8-bit, noise
of variance a m + c -- splits them even/odd, stacks the halves with stack-probe, and computes the
ratio with the TRUE photon-transfer line and the noise-free mean frame. There D holds the engine's
own noise only, so the ratio is what our interpolation and demosaic make of the model, plus the
estimate's scatter: measured 0.583-1.025 over the 8 captures (disc 0.94-1.03, lunar 0.58-0.87), mean
0.849, sd 0.160. AGREEMENT = mean -+ 3 sd = 0.37-1.33 (README, "Split-half check"). A real capture outside it goes to review (bench.status_of), with the
ratio as the reason -- whatever the cause: too much power means noise or unshared signal the model
does not hold, too little means smoothing beyond the engine's own, and either way the judged noise
model is not confirmed on that capture.
"""
import argparse
import json
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
# measured / model over 0.21-0.30 cycles/px that an honest engine stack shows: calibrate's 8 synthetic
# captures (2026-10-23) read 0.583-1.025, mean 0.849, sd 0.160; AGREEMENT = mean -+ 3 sd.
AGREEMENT = (0.37, 1.33)


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


def ratio(path_a, path_b, k_a, k_b, ptc, raw_mean):
    """Per END bin (and 0.40-0.50 last): D's power over the model sigma_frame^2 (1/k_a + 1/k_b) / 4."""
    A = metric.luminance(refs.load_image(path_a))
    B = metric.luminance(refs.load_image(path_b))
    reg = refs.register(B, A)
    Bo = refs.apply_orientation(B, reg.orientation)
    Bs = np.real(np.fft.ifft2(ndimage.fourier_shift(np.fft.fft2(Bo), reg.shift)))
    D = (A - Bs) / 2
    box = metric.target_box(A)
    s = metric.measure(A, box).signal
    p = metric.periodogram(D[box.y0:box.y1, box.x0:box.x1] / s)
    d = metric.band_means(p, metric.END_EDGES) + metric.band_means(p, metric.NOISE_BAND)
    s2 = noise.box_noise(ptc, noise.raw_under(raw_mean, A, box))
    model = s2 * (1 / k_a + 1 / k_b) / 4
    return {"box": list(box), "shift": list(reg.shift), "sigma_frame2": s2, "model": model,
            "ratio": [v / model for v in d], "high": float(np.mean([v / model for v in d[HIGH_BINS]]))}


def _k_of(run_dir, half):
    """(judged k, mean-kept k, why) for a half from its stack-probe and stack-region-probe JSON."""
    with open(os.path.join(run_dir, f"half{half}.json")) as fh:
        probe = json.load(fh)
    keff_path = os.path.join(run_dir, f"half{half}.keff.json")
    keff = None
    if os.path.exists(keff_path):
        with open(keff_path) as fh:
            keff = json.load(fh)
    k, why = benchset.ours_frames_box(probe, keff)
    return k, benchset.ours_frames_mean(probe), why


def check(run_dir, frame_noise):
    """The split-half record of a run directory holding halfA/halfB .fits + .json (+ .keff.json), or
    None when the halves were not stacked."""
    pa, pb = (os.path.join(run_dir, f"half{h}.fits") for h in "AB")
    if not all(os.path.exists(p) for p in (pa, pb, os.path.join(run_dir, "halfA.json"),
                                          os.path.join(run_dir, "halfB.json"))):
        return None
    if frame_noise is None or isinstance(frame_noise, str):
        return {"status": "unmeasured", "reason": f"no per-frame noise: {frame_noise}"}
    ptc, raw_mean = frame_noise
    (ka, ka_m, why_a), (kb, kb_m, why_b) = _k_of(run_dir, "A"), _k_of(run_dir, "B")
    out = {"tolerance": list(AGREEMENT)}
    if ka is None or kb is None:
        return dict(out, status="unmeasured", reason=f"halves' k: {why_a or why_b}")
    r = ratio(pa, pb, ka, kb, ptc, raw_mean)
    rm = ratio(pa, pb, ka_m, kb_m, ptc, raw_mean) if (ka_m and kb_m) else None
    ok = AGREEMENT[0] <= r["high"] <= AGREEMENT[1]
    out.update(status="agrees" if ok else "outside", judged=dict(r, k=[ka, kb]),
               mean_k=(dict(rm, k=[ka_m, kb_m]) if rm else None))
    return out


def summary_line(rec) -> str:
    if not rec:
        return "not stacked (run-stack-bench.sh --no-split)"
    if rec.get("status") not in ("agrees", "outside"):
        return f"{rec.get('status')}: {rec.get('reason', '')}"
    j = rec["judged"]
    line = (f"**{rec['status']}** ({AGREEMENT[0]}-{AGREEMENT[1]}): 0.21-0.30 x{j['high']:.2f} (k {j['k'][0]:.0f}/"
            f"{j['k'][1]:.0f}), 0.06-0.09 x{j['ratio'][0]:.2f}, 0.40-0.50 x{j['ratio'][-1]:.2f}")
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
    conditions = conditions or [(k, j, s) for k in ("disc", "lunar") for j in (0.3, 0.8) for s in (1, 2)]
    out = []
    for kind, jitter, seed in conditions:
        frames, mean, ptc = synthetic_frames(kind, 600, jitter, 0.15, 0.6, seed)
        tag = f"{kind}-j{jitter}-s{seed}"
        k = {}
        for half, sl in (("A", slice(0, None, 2)), ("B", slice(1, None, 2))):
            ser = os.path.join(work, f"{tag}-{half}.ser")
            write_ser(ser, frames[sl])
            fits = os.path.join(work, f"{tag}-{half}.fits")
            probe = json.loads(_run([stack_probe, ser, "--bayer", "RGGB", "--out", fits, "--json"]))
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
        out.append({"condition": tag, "k": [k["A"], k["B"]], "high": r["high"], "ratio": r["ratio"]})
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
    a = ap.parse_args(argv)
    if a.cmd == "split":
        w, h = (int(v) for v in a.size.lower().split("x"))
        n = split(a.video, (w, h), a.even, a.odd)
        print(f"even {n[0]} odd {n[1]} frames")
        return 0
    res = calibrate(a.work, a.stack_probe, a.region_probe)
    hs = [r["high"] for r in res]
    print(f"range {min(hs):.3f}-{max(hs):.3f}, mean {np.mean(hs):.3f}, sd {np.std(hs):.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
