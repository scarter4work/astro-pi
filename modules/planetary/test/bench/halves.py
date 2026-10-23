#!/usr/bin/env python3
"""halves.py -- split-half validation of END's noise model (Ruling P28, rule 3). Validation only:
nothing here feeds END.

    python3 -I halves.py split <video.avi> WxH <even.ser> <odd.ser>    # run-stack-bench.sh

SPLIT. The capture's even frames go to one SER and its odd frames to another (8-bit, BAYER_RGGB
header; the bench passes --bayer explicitly anyway), the frames exactly as ffmpeg decodes them
(bayer.read_frames: the reader's layout). The two halves share no frame, so their stacks share no
random noise.

CHECK (check). Stack A (even) and stack B (odd) are each stacked by the engine. B is registered to
A (refs.register) and moved onto A's grid by an exact Fourier shift (it keeps B's power spectrum);
D = (A - B) / 2. Where the halves agree on the signal, D is noise only, and its power is
(N_A + N_B) / 4: END's model says N = sigma_frame^2 / k, so D's model is
sigma_frame^2 (1/k_A + 1/k_B) / 4 on A's target box, with sigma_frame^2 from the capture's raw
frames (noise.box_noise) and k_A, k_B from each half's own stack-probe JSON (benchset.ours_frames,
and ours_frames_alt for the alternative). The ratio measured / model is reported per END bin and
at 0.40-0.50 cycles/px. What it shows:
  * The model is of an UNSMOOTHED stack. Our stack interpolates (cubic B-spline) and demosaics, so
    its real noise falls toward Nyquist: a ratio under 1 in the high bins is that smoothing, which
    END charges as blur.
  * D also holds whatever the halves do not share of the SIGNAL -- each half's own frame choice
    and local alignment move fine structure by hundredths of a pixel -- which grows with the
    signal's power: a ratio far above 1, highest in the low bins where the target's power is
    largest, is that, not noise. The disc/sky power ratio of D against the photon budget's
    disc/sky ratio shows which one dominates.
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


def _ser_header(w, h, n):
    return (b"LUCAM-RECORDER" + struct.pack("<7i", 0, BAYER_RGGB, 0, w, h, 8, n) + b"\0" * 120
            + struct.pack("<2q", 0, 0))


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


def _band_powers(x):
    w = np.outer(np.hanning(x.shape[0]), np.hanning(x.shape[1]))
    x = x - (w * x).sum() / w.sum()
    p = np.abs(np.fft.fft2(x * w)) ** 2 / (w ** 2).sum()
    f = metric._radial_frequency(x.shape)
    e = metric.END_EDGES
    out = [float(p[(f >= a) & (f < b)].mean()) for a, b in zip(e[:-1], e[1:])]
    return out + [float(p[(f >= metric.NOISE_BAND[0]) & (f <= metric.NOISE_BAND[1])].mean())]


def check(run_dir, frame_noise):
    """The split-half record for a run directory holding halfA/halfB .fits + .json, or None when
    the halves were not stacked. frame_noise: (PhotonTransfer, raw mean) or a str (unmeasurable)."""
    pa, pb = (os.path.join(run_dir, f"half{h}.fits") for h in "AB")
    ja, jb = (os.path.join(run_dir, f"half{h}.json") for h in "AB")
    if not all(os.path.exists(p) for p in (pa, pb, ja, jb)):
        return None
    if frame_noise is None or isinstance(frame_noise, str):
        return {"status": "unmeasured", "reason": f"no per-frame noise: {frame_noise}"}
    ptc, raw_mean = frame_noise
    A = metric.luminance(refs.load_image(pa))
    B = metric.luminance(refs.load_image(pb))
    with open(ja) as fh:
        jA = json.load(fh)
    with open(jb) as fh:
        jB = json.load(fh)
    reg = refs.register(B, A)
    Bo = refs.apply_orientation(B, reg.orientation)
    Bs = np.real(np.fft.ifft2(ndimage.fourier_shift(np.fft.fft2(Bo), reg.shift)))
    D = (A - Bs) / 2
    box = metric.target_box(A)
    s = metric.measure(A, box).signal
    d = _band_powers(D[box.y0:box.y1, box.x0:box.x1] / s)
    raw = noise.raw_under(raw_mean, A, box)
    s2 = noise.box_noise(ptc, raw)
    out = {"status": "measured", "box": list(box), "shift": list(reg.shift), "sigma_frame2": s2,
           "bins": [f"{a:.2f}-{b:.2f}" for a, b in zip(metric.END_EDGES[:-1], metric.END_EDGES[1:])] + ["0.40-0.50"],
           "measured": d, "frames": {"A": jA.get("used"), "B": jB.get("used")}}
    for name, fn in (("judged", benchset.ours_frames), ("alt", benchset.ours_frames_alt)):
        ka, kb = fn(jA), fn(jB)
        if not ka or not kb:
            out[name] = None
            continue
        model = s2 * (1 / ka + 1 / kb) / 4
        out[name] = {"k": [ka, kb], "model": model, "ratio": [v / model for v in d]}
    # Which dominates D: noise (D's disc/sky power ratio near the photon budget's) or signal the
    # halves do not share (far above it).
    try:
        sky = metric.sky_noise_floor(D, mask=metric.sky_mask(A))   # D on A's sky
        disc_sky = d[-1] / (sky[0] / s ** 2)
        budget_ratio = s2 / noise.sky_noise(ptc, raw)
        out["disc_over_sky"] = {"D": disc_sky, "photon_budget": budget_ratio}
    except (metric.SkyUnmeasurable, noise.NoiseUnmeasurable) as e:
        out["disc_over_sky"] = {"unmeasured": str(e)}
    return out


def summary_line(rec) -> str:
    """measured / model in the 0.21-0.30 bins (mean) and at 0.40-0.50, both readings of k."""
    if rec.get("status") != "measured":
        return rec.get("reason", "unmeasured")
    parts = []
    for name in ("judged", "alt"):
        r = rec.get(name)
        if r:
            hi = float(np.mean(r["ratio"][5:8]))
            parts.append(f"{name} k {r['k'][0]:.0f}/{r['k'][1]:.0f}: 0.21-0.30 x{hi:.2f}, 0.06-0.09 "
                         f"x{r['ratio'][0]:.2f}, 0.40-0.50 x{r['ratio'][-1]:.2f}")
    ds = rec.get("disc_over_sky", {})
    if "D" in ds:
        parts.append(f"D disc/sky {ds['D']:.3g} vs photon budget {ds['photon_budget']:.3g}")
    return "; ".join(parts)


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("split")
    s.add_argument("video")
    s.add_argument("size", help="WxH")
    s.add_argument("even")
    s.add_argument("odd")
    a = ap.parse_args(argv)
    w, h = (int(v) for v in a.size.lower().split("x"))
    n = split(a.video, (w, h), a.even, a.odd)
    print(f"even {n[0]} odd {n[1]} frames")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
