#!/usr/bin/env python3
"""Goldens for the EZHazeKill engine (modules/ez-tools/src/hazekill) from the reference hk_lib.hk_v11.

Every case is synthesised from a splitmix64 stream; test/util/Synth.h implements the same
generator in C++, so neither side stores pixels. Per-sample math uses Python's `math`
(glibc libm, as the C++ side), not numpy's SIMD kernels, so both sides build identical images.

  python3 modules/ez-tools/test/make_golden.py   -> modules/ez-tools/test/golden/hazekill_golden.json
"""
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from hk_lib import hk_v11  # noqa: E402

M64 = (1 << 64) - 1
N_SAMPLES = 4096


class SplitMix64:
    def __init__(self, seed):
        self.s = seed & M64

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)

    def uniform(self):                       # [0, 1)
        return (self.next() >> 11) * 2.0 ** -53

    def gauss(self):                         # Box-Muller, one value per pair
        u1 = self.uniform()
        u2 = self.uniform()
        return math.sqrt(-2.0 * math.log(1.0 - u1)) * math.cos(2.0 * math.pi * u2)


# name: (seed, width, height, sky per channel, sigma, blob amplitude, kind)
# kind "noise": sky_c + sigma*g + blob;  "constant": sky_c;  "clipped": 0 with p=0.6 else 0.05*u
CASES = {
    "rgb_sky_object": (1, 640, 448, (0.03, 0.035, 0.028), 3e-4, 0.2, "noise"),
    "stretched_like": (2, 640, 448, (0.08, 0.08, 0.08), 0.01, 0.2, "noise"),
    "mono":           (3, 640, 448, (0.04,), 5e-4, 0.2, "noise"),
    "tiny_40x30_rgb": (4, 40, 30, (0.05, 0.052, 0.048), 1e-3, 0.0, "noise"),
    "flat_constant":  (5, 256, 256, (0.1, 0.1, 0.1), 0.0, 0.0, "constant"),
    "black_clipped":  (6, 256, 256, (0.0, 0.0, 0.0), 0.0, 0.0, "clipped"),
}
BLOB_SIGMA = 60.0


def synth(seed, w, h, sky, sigma, blob, kind):
    """Channel-major, row-major float64 samples, (h, w, C) array."""
    rng = SplitMix64(seed)
    C = len(sky)
    out = np.empty((h, w, C), dtype=np.float64)
    cx, cy = 0.5 * w, 0.5 * h
    for c in range(C):
        for y in range(h):
            for x in range(w):
                if kind == "constant":
                    v = sky[c]
                elif kind == "clipped":
                    u = rng.uniform()
                    v = 0.0 if u < 0.6 else 0.05 * rng.uniform()
                else:
                    v = sky[c] + sigma * rng.gauss()
                    if blob:
                        d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy)
                        v += blob * math.exp(-d2 / (2.0 * BLOB_SIGMA * BLOB_SIGMA))
                out[y, x, c] = v
    return out


def main():
    cases = []
    for name, (seed, w, h, sky, sigma, blob, kind) in CASES.items():
        img = synth(seed, w, h, sky, sigma, blob, kind)
        out, info = hk_v11(img)
        C = img.shape[2]
        total = w * h * C
        pick = SplitMix64(seed + 1000)
        samples = []
        for _ in range(N_SAMPLES):
            i = pick.next() % total                       # channel-major flat index
            c, r = divmod(i, w * h)
            y, x = divmod(r, w)
            samples.append([i, float(out[y, x, c])])
        cases.append(dict(
            name=name, seed=seed, width=w, height=h, sky_in=list(sky), sigma_in=sigma, blob=blob, kind=kind,
            amount=0.5, neutralize=1.0, target=0.05,
            sky=[float(v) for v in info["sky"]], sigma=[float(v) for v in info["sigma"]],
            target_eff=float(info["target"]), ped=float(info["ped"]),
            offset=[float(v) for v in info["offset"]], samples=samples))
        print("  %-16s %dx%dx%d  sky %s  ped %.6f" % (name, w, h, C, np.round(info["sky"], 6), info["ped"]))
    path = os.path.join(HERE, "golden", "hazekill_golden.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump({"generator": "make_golden.py (hk_lib.hk_v11)", "blob_sigma": BLOB_SIGMA, "cases": cases}, f)
    print("wrote %s (%d cases)" % (path, len(cases)))


if __name__ == "__main__":
    main()
