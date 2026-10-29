#!/usr/bin/env python3
"""Planet processing real-data acceptance checks (planet Task 5), over run-planet-real.sh's output.

    planet_check.py [--png-dir DIR] [--no-png] <run-dir> [<id>[:<role>] ...]

With no ids, every <run-dir>/<id>/full/probe.json is checked (role "acceptance"). Each <run-dir>/<id>/ holds full/
(and, for the split-half measures, even/ and odd/): stack-probe --planet's probe.json, planet.fits (the defaults),
planet_s<A>.fits (Sharpen A), planet_c<S>.fits (Chroma blur S), planet_k0.fits (Saturation 0), stages/ (display,
display_unaligned, lum_sharp, lin_unsharp, lin_sharp, calibrated, masks) and, in full/, stack.fits. <run-dir>/gate/
holds the gating run (auto.exit / RGGB.exit and their JSON). numpy only (Pillow for the PNGs, imported when they are
written), so the ctest case runs wherever numpy does.

Every requirement prints its number, its bar and PASS / FAIL / NOT JUDGED. NOT JUDGED is for a requirement whose
input this capture does not have -- the stacker refused it (Venus saturated, a Neptune with no target), no disc was
found (an accepted outcome, ruling Q3a), a step skipped and HISTORY says why -- never for a missing file, which fails
(fail-closed). The requirements (spec "Acceptance (the bar)", with rulings P2 and P3):

  1 calib     linear calibrated RGB (stages/calibrated.fits: after calibration and the sky plane), disc pixels with no
              stack sample >= 0.999: mean B/G and R/G within 2 % of the catalogue's (Planet.cpp PlanetReflectance).
              A skipped calibration is NOT JUDGED when HISTORY gives the reason, FAIL when it does not.
  2 sky       the mineral B3 measure: the sky's 3-sigma-clipped mean per channel <= 0.5 % of the disc's mean, R and B
              within one sigma (robust std of the per-pixel c - G) of G.
  3 disp R/B  dispersion after the alignment, by an estimator independent of the engine's (the half-contrast limb
              radius per channel along rays from the disc's centroid; R - G and B - G fitted as a cos + b sin + c,
              1-sigma by a jackknife over 8 groups of rays): |shift| <= 0.1 px. A residual over 0.1 px but within 2
              sigma of it (this estimator cannot tell) is NOT JUDGED, never PASS. The engine's own before / after
              (MeasurePlanetDispersion; after is circular for a resampled channel: the engine's estimate of its own
              correction) are printed with it.
  4 halo      sharpened - unsharpened linear L in the band 1-6 px outside the disc mask: 0.5 / 99.5 percentiles'
              larger magnitude <= 2 % of the disc mean (B4).
  4 fringe    ruling P3: (sum R - sum B) / sum L on each side of the disc along the measured dispersion axis (the
              engine's B - R before the alignment; with none measured, the worst of 8 directions), in the ring 1-4 px
              outside the half-contrast line, sides within 60 deg of the axis; the difference between the sides on the
              output, against 1.5 x the 95th percentile of the same on 64 zero-dispersion synthetic copies of the same
              disc (the display's luminance in the disc's mean colour, each channel with the sky's own noise, clipped
              at 0 as the display is, then the colour step's chroma blur and ratio), R and B moved 0.1 px either side
              of G along the axis: acceptance 3's tolerance (Task 5 ruling candidate: with zero shift the synthetic
              reads 0.0005-0.002 on the real discs, while a 0.05 px residual -- acceptance 3 PASS -- reads 0.04, so the
              bar as written failed every capture however well aligned). Before / after the alignment (the display
              RGB) and the zero-shift synthetic are printed with it.
  5 skynoise  ruling P2: the output's L robust std over the sky, at the default Sharpen / at Sharpen 0: <= 1.5.
  6 clip      every output finite, in [0, 1]; no sample >= 0.999 whose stack sample is < 0.999.
  7 natural   Saturation 1 (the run's own saturation must be 1): the output's mean B/G and R/G over the disc within
              2 % of the display's (the calibrated colour, aligned); also printed against the catalogue's.
  7 grey      Saturation 0: max |R - G|, |B - G| <= 1e-6.

INFO rows: the disc's px / equivalent radius / box, the rings' place in the mask on Saturn (spec P6: the share of
the pixels at or above 10 % of the disc median that the mask holds), the slider time. The series (every capture):
Sharpen 0, 1, 1.5, 2, 3, 4 -- the limb's crispness (mean |grad L| within 1.5 px of the half-contrast line), the
disc's fine detail (starlet layers 1-2 rms), the split-half noise gain on the disc interior (the halves registered:
phase correlation, then Lucas-Kanade; both Fourier-shifted half-way), the over- / undershoot of the output's L
against Sharpen 0 over the disc (Saturn's rings, Task 3's finding), the sky's noise -- each as a ratio to Sharpen 0;
Chroma blur 0, 0.5, 1, 1.5, 2, 2.5, 3, 4, 6 -- the sky's chroma rms (each channel less the channels' mean), the
disc's split-half chroma noise (as a ratio to blur 0 and in 8-bit levels) and the disc's colour rms over its interior
(what the blur takes from the bands' colour), and the blur picked by the brief's rule (the smallest of 0-2.5 that
halves the sky's chroma noise on every capture with a sky).

PNGs (--png-dir, default <run-dir>/png; 8-bit, the disc's crop upscaled x4 nearest): <id>_plain_vs_processed.png
(the stack at the display's white point beside the output), <id>_sharpen_series.png, <id>_chroma_series.png, each
panel labelled, the default marked. Writes <id>/check.json and <run-dir>/summary.json. Exit 0 when nothing failed,
1 otherwise (any FAIL, a missing file, no capture, the gating run missing or not refused).
"""
import json
import math
import os
import re
import sys

import numpy as np

CLIP = 0.999
# Planet.cpp PlanetReflectance: (B/G, R/G), Mallama, Krobusek & Pavlov 2017, Table 7.
REFLECTANCE = {"Mercury": (0.739, 1.113), "Venus": (0.955, 0.955), "Mars": (0.518, 1.471), "Jupiter": (0.823, 0.954),
               "Saturn": (0.679, 1.295), "Uranus": (1.150, 0.541), "Neptune": (1.271, 0.511)}
SHARPENS = (0.0, 1.0, 1.5, 2.0, 3.0, 4.0)
BLURS = (0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0)
EPS = 0.002  # FinishPlanet's eps (Planet.h)
SYNTH_SEEDS = 64
TOLERANCE = 0.1  # acceptance 3's residual (px): the fringe bar's synthetic carries it (Task 5 ruling candidate)


class HarnessError(Exception):
    pass


# ---- I/O ----

def read_fits(path):
    """stack-probe's FITS (ProbeFits.h): (C, H, W) float64, row 0 = top; C = 1 for a 2-axis file."""
    if not os.path.isfile(path):
        raise HarnessError("missing %s" % path)
    cards = {}
    with open(path, "rb") as f:
        done = False
        while not done:
            block = f.read(2880)
            if len(block) < 2880:
                raise HarnessError("%s: short FITS header" % path)
            for i in range(0, 2880, 80):
                c = block[i:i + 80].decode("ascii", "replace")
                if c[:8].strip() == "END":
                    done = True
                    break
                if c[8:10] == "= ":
                    cards[c[:8].strip()] = c[10:].split("/")[0].strip()
        w, h = int(cards["NAXIS1"]), int(cards["NAXIS2"])
        n = int(cards.get("NAXIS3", "1")) if cards["NAXIS"] == "3" else 1
        raw = f.read(w * h * n * 4)
    if len(raw) != w * h * n * 4:
        raise HarnessError("%s: short data" % path)
    return np.frombuffer(raw, ">f4").reshape(n, h, w).astype(np.float64)


def load_json(path):
    if not os.path.isfile(path):
        raise HarnessError("missing %s" % path)
    try:
        return json.load(open(path))
    except ValueError as e:
        raise HarnessError("%s: %s" % (path, e))


class Report:
    def __init__(self, cid):
        self.cid, self.rows = cid, []

    def add(self, req, value, bar, verdict, **extra):
        row = {"req": req, "value": value, "bar": bar, "verdict": verdict}
        row.update(extra)
        self.rows.append(row)
        print("  %-11s %-10s %s  [bar: %s]" % (req, verdict, value, bar), flush=True)

    def judge(self, req, value, bar, ok, **extra):
        self.add(req, value, bar, "PASS" if ok else "FAIL", **extra)

    def info(self, req, value, **extra):
        self.add(req, value, "-", "INFO", **extra)

    def failed(self):
        return [r for r in self.rows if r["verdict"] == "FAIL"]


def history(j, prefix):
    return next((h for h in j["planet"]["history"] if h.startswith(prefix)), "")


# ---- numpy image helpers (no scipy: the ctest case runs where numpy does) ----

def gaussian(x, sigma):
    """Separable Gaussian, radius 4 sigma, mirrored edges (as ChromaLayer); sigma 0 returns a copy."""
    if sigma <= 0:
        return x.copy()
    r = max(1, int(math.ceil(4 * sigma)))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = x
    for axis in (-1, -2):
        p = [(0, 0)] * out.ndim
        p[axis] = (r, r)
        q = np.pad(out, p, mode="symmetric")
        acc = np.zeros_like(out)
        n = out.shape[axis]
        for i, w in enumerate(k):
            acc += w * np.take(q, np.arange(i, i + n), axis=axis)
        out = acc
    return out


def offsets(r):
    rr = int(math.floor(r))
    return [(dy, dx) for dy in range(-rr, rr + 1) for dx in range(-rr, rr + 1) if dy * dy + dx * dx <= r * r + 1e-9]


def shifted(m, dy, dx, fill=False):
    """m moved by (dy, dx): out[y, x] = m[y - dy, x - dx]; outside filled."""
    h, w = m.shape
    out = np.full_like(m, fill)
    ys, yd = (slice(0, h - dy), slice(dy, h)) if dy >= 0 else (slice(-dy, h), slice(0, h + dy))
    xs, xd = (slice(0, w - dx), slice(dx, w)) if dx >= 0 else (slice(-dx, w), slice(0, w + dx))
    out[yd, xd] = m[ys, xs]
    return out


def dilate(m, r):
    out = np.zeros_like(m)
    for dy, dx in offsets(r):
        out |= shifted(m, dy, dx)
    return out


def erode(m, r):
    out = np.ones_like(m)
    for dy, dx in offsets(r):
        out &= shifted(m, dy, dx, fill=True)
    return out


def dist_outside(m, rmax):
    """Euclidean distance to the nearest pixel of m, up to rmax (inf beyond); 0 on m."""
    d = np.full(m.shape, np.inf)
    for dy, dx in offsets(rmax):
        d = np.where(shifted(m, dy, dx), np.minimum(d, math.hypot(dy, dx)), d)
    return d


def fill_holes(m):
    """m with every region of ~m not connected (4-connected) to the border filled."""
    outside = np.zeros_like(m)
    outside[0, :], outside[-1, :], outside[:, 0], outside[:, -1] = ~m[0, :], ~m[-1, :], ~m[:, 0], ~m[:, -1]
    while True:
        grown = outside.copy()
        grown[1:, :] |= outside[:-1, :]
        grown[:-1, :] |= outside[1:, :]
        grown[:, 1:] |= outside[:, :-1]
        grown[:, :-1] |= outside[:, 1:]
        grown &= ~m
        if (grown == outside).all():
            return ~outside
        outside = grown


def bilinear(img, ys, xs):
    """img sampled at (ys, xs), linear interpolation; nan outside."""
    h, w = img.shape
    y0, x0 = np.floor(ys).astype(int), np.floor(xs).astype(int)
    fy, fx = ys - y0, xs - x0
    ok = (y0 >= 0) & (x0 >= 0) & (y0 + 1 < h) & (x0 + 1 < w)
    y0c, x0c = np.clip(y0, 0, h - 2), np.clip(x0, 0, w - 2)
    v = (img[y0c, x0c] * (1 - fy) * (1 - fx) + img[y0c, x0c + 1] * (1 - fy) * fx + img[y0c + 1, x0c] * fy * (1 - fx) +
         img[y0c + 1, x0c + 1] * fy * fx)
    return np.where(ok, v, np.nan)


def starlet(x, n=2):
    """The B3-spline starlet's detail layers 1..n (a trous, mirrored edges), as the engine's."""
    k = np.array([1, 4, 6, 4, 1]) / 16.0
    c, out = x, []
    for j in range(n):
        step = 2 ** j
        cn = c
        for axis in (0, 1):
            r = 2 * step
            p = [(0, 0), (0, 0)]
            p[axis] = (r, r)
            q = np.pad(cn, p, mode="symmetric")
            acc = np.zeros_like(cn)
            for i, wt in enumerate(k):
                acc += wt * np.take(q, np.arange(i * step, i * step + cn.shape[axis]), axis=axis)
            cn = acc
        out.append(c - cn)
        c = cn
    return out


def robust_std(v):
    v = np.asarray(v, float)
    return 1.4826 * float(np.median(np.abs(v - np.median(v)))) if v.size > 1 else float("nan")


def clipped_mean(v, rounds=3, k=3.0):
    m = np.ones(v.size, dtype=bool)
    for _ in range(rounds):
        mu, sd = v[m].mean(), v[m].std()
        m = np.abs(v - mu) <= k * sd
    return float(v[m].mean())


def fourier_shift(img, dy, dx):
    """img moved by (dy, dx) px (band-limited, periodic): out(p) = img(p - (dy, dx))."""
    h, w = img.shape
    fy, fx = np.fft.fftfreq(h)[:, None], np.fft.fftfreq(w)[None, :]
    return np.fft.ifft2(np.fft.fft2(img) * np.exp(-2j * np.pi * (fy * dy + fx * dx))).real


# ---- the measures ----

def crop_box(box, shape, margin=None):
    x0, y0, x1, y1 = box
    m = margin if margin is not None else max(16, int(0.6 * max(x1 - x0 + 1, y1 - y0 + 1)))
    return max(0, y0 - m), min(shape[0], y1 + m + 1), max(0, x0 - m), min(shape[1], x1 + m + 1)


def half_contrast(L, disk, sky):
    """The region at or above half way between the sky's level and the disc's median, holes filled."""
    s = float(np.median(L[sky])) if sky.sum() > 20 else float(np.percentile(L, 5))
    d = float(np.median(L[disk]))
    return fill_holes(L >= s + 0.5 * (d - s))


def limb_shift(img3, disk, sky, frame_edge):
    """Independent dispersion estimate: per channel the radius of the half-contrast crossing (each channel
    less its own sky, each ray at half its own peak, so a disc brighter on one side than the other moves its
    crossings less than one level for the whole disc would) along rays from the disc's centroid, outermost downward
    crossing;
    R - G and B - G radii fitted as a cos(t) + b sin(t) + c (two passes, 3 x MAD outliers out); a, b = where the
    channel sits against G (+x right, +y down). 1-sigma per axis by a delete-one-group jackknife over 8 groups of
    neighbouring rays. frame_edge: pixels on the image's border (a ray reaching one is not a limb). None when under
    12 rays give all three crossings."""
    sm = np.stack([gaussian(c, 0.7) for c in img3])
    core = erode(disk, 2)
    if core.sum() < 5:
        core = disk
    norm = []
    for c in sm:
        s = float(np.median(c[sky])) if sky.sum() > 20 else float(np.percentile(c, 5))
        lev = float(np.median(c[core]))
        if not lev > s:
            return None
        norm.append((c - s) / (lev - s))
    ys, xs = np.nonzero(disk)
    cy, cx = ys.mean(), xs.mean()
    R = math.sqrt(disk.sum() / math.pi)
    rmax = float(np.hypot(ys - cy, xs - cx).max()) + 6
    nray = max(24, int(2 * math.pi * R))
    th = 2 * math.pi * (np.arange(nray) + 0.5) / nray
    r = np.arange(0.0, rmax, 0.05)
    radii = np.full((3, nray), np.nan)
    for k, t in enumerate(th):
        py, px = cy + r * math.sin(t), cx + r * math.cos(t)
        iy, ix = np.clip(np.rint(py).astype(int), 0, disk.shape[0] - 1), np.clip(np.rint(px).astype(int), 0, disk.shape[1] - 1)
        if frame_edge[iy, ix].any():
            continue
        for c in range(3):
            v = bilinear(norm[c], py, px)
            if np.isnan(v).any():
                break
            v = v / np.max(v)  # half of this ray's own peak
            above = v >= 0.5
            idx = np.nonzero(above[:-1] & ~above[1:])[0]
            if idx.size == 0:
                break
            i = idx[-1]
            radii[c, k] = r[i] + (v[i] - 0.5) / (v[i] - v[i + 1]) * (r[i + 1] - r[i])
    ok = np.isfinite(radii).all(axis=0)
    if ok.sum() < 12:
        return None
    t, rad = th[ok], radii[:, ok]
    groups = (np.arange(ok.size)[ok] * 8 // nray)

    def fit(delta, keep):
        A = np.stack([np.cos(t), np.sin(t), np.ones_like(t)], axis=1)
        sel = keep.copy()
        for _ in range(2):
            p = np.linalg.lstsq(A[sel], delta[sel], rcond=None)[0]
            res = delta - A @ p
            mad = 1.4826 * np.median(np.abs(res[sel])) + 1e-9
            sel = keep & (np.abs(res) <= 3 * mad)
        return np.linalg.lstsq(A[sel], delta[sel], rcond=None)[0]

    out = {}
    for name, c in (("R", 0), ("B", 2)):
        delta = rad[c] - rad[1]
        allk = np.ones(t.size, bool)
        a, b, _ = fit(delta, allk)
        jx, jy = [], []
        for g in range(8):
            keep = groups != g
            if keep.sum() < 9 or keep.all():
                continue
            pa, pb, _ = fit(delta, keep)
            jx.append(pa)
            jy.append(pb)
        n = len(jx)
        ux = math.sqrt((n - 1) / n * np.sum((np.array(jx) - np.mean(jx)) ** 2)) if n > 1 else float("inf")
        uy = math.sqrt((n - 1) / n * np.sum((np.array(jy) - np.mean(jy)) ** 2)) if n > 1 else float("inf")
        out[name] = {"dx": float(a), "dy": float(b), "u": float(math.hypot(ux, uy)), "mag": float(math.hypot(a, b))}
    out["rays"] = int(ok.sum())
    return out


def side_asym(rgb, ring, cy, cx, axis):
    """(sum R - sum B) / sum L on the + side less the same on the - side of the ring along axis (within 60 deg)."""
    ys, xs = np.nonzero(ring)
    vy, vx = ys - cy, xs - cx
    n = np.hypot(vy, vx) + 1e-12
    cosang = (vx * axis[0] + vy * axis[1]) / n
    L = rgb.mean(axis=0)
    vals = []
    for sel in (cosang >= 0.5, cosang <= -0.5):
        yy, xx = ys[sel], xs[sel]
        sl = L[yy, xx].sum()
        if sel.sum() < 3 or not sl > 0:
            return float("nan")
        vals.append(float((rgb[0][yy, xx].sum() - rgb[2][yy, xx].sum()) / sl))
    return vals[0] - vals[1]


AXES8 = [(math.cos(k * math.pi / 8), math.sin(k * math.pi / 8)) for k in range(8)]


def fringe_measure(rgb, ring, cy, cx, axis):
    if axis is not None:
        return abs(side_asym(rgb, ring, cy, cx, axis))
    return max(abs(side_asym(rgb, ring, cy, cx, a)) for a in AXES8)


def colour_step(display, Lsharp, sigma):
    """FinishPlanet's colour at Saturation 1: out_c = L (C_c + eps) / (mean(C) + eps), C = the chroma blur."""
    C = np.stack([gaussian(c, sigma) for c in display])
    return Lsharp[None] * (C + EPS) / (C.mean(axis=0)[None] + EPS)


def register(g0, g1, region):
    """(dy, dx): g1 moved by (dy, dx) matches g0. Integer by phase correlation, then Lucas-Kanade (Fourier shifts)
    over region; None when it does not converge."""
    h, w = min(g0.shape[0], g1.shape[0]), min(g0.shape[1], g1.shape[1])
    a, b = g0[:h, :w] - g0[:h, :w].mean(), g1[:h, :w] - g1[:h, :w].mean()
    cross = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    cross /= np.abs(cross) + 1e-12
    c = np.fft.ifft2(cross).real
    y, x = np.unravel_index(int(np.argmax(c)), c.shape)
    dy, dx = float(y if y < h // 2 else y - h), float(x if x < w // 2 else x - w)
    A, B = g0[:h, :w], g1[:h, :w]
    m = region[:h, :w]
    for _ in range(30):
        s = fourier_shift(B, dy, dx)
        gy, gx = np.gradient(0.5 * (A + s))
        e = (A - s)[m]
        J = np.stack([gy[m], gx[m]], axis=1)
        jtj = J.T @ J
        if abs(np.linalg.det(jtj)) < 1e-18:
            return None
        # A - s(d + delta) ~ e + grad(s) . delta (s(p) = B(p - d), so ds/dd = -grad s): delta = -(J^T J)^-1 J^T e
        step = -np.linalg.solve(jtj, J.T @ e)
        dy, dx = dy + step[0], dx + step[1]
        if abs(step).max() < 1e-3:
            return dy, dx
    return None


# ---- one capture ----

def outputs(d, j):
    p = j["planet"]
    o = {"planet": read_fits(os.path.join(d, "planet.fits"))}
    for e in p.get("sharpenAlso", []):
        o["s%g" % e["sharpen"]] = read_fits(e["output"] if os.path.isabs(e["output"]) else os.path.join(d, os.path.basename(e["output"])))
    for e in p.get("chromaBlurAlso", []):
        o["c%g" % e["chromaBlur"]] = read_fits(e["output"] if os.path.isabs(e["output"]) else os.path.join(d, os.path.basename(e["output"])))
    for e in p.get("saturationAlso", []):
        o["k%g" % e["saturation"]] = read_fits(e["output"] if os.path.isabs(e["output"]) else os.path.join(d, os.path.basename(e["output"])))
    return o


def load_part(d, need_stack=False):
    j = load_json(os.path.join(d, "probe.json"))
    if "planet" not in j or "image" not in j:
        return {"json": j}
    s = os.path.join(d, "stages")
    r = {"json": j, "out": outputs(d, j),
         "display": read_fits(os.path.join(s, "display.fits")),
         "unaligned": read_fits(os.path.join(s, "display_unaligned.fits")),
         "lin_unsharp": read_fits(os.path.join(s, "lin_unsharp.fits"))[0],
         "lin_sharp": read_fits(os.path.join(s, "lin_sharp.fits"))[0],
         "calibrated": read_fits(os.path.join(s, "calibrated.fits"))}
    m = read_fits(os.path.join(s, "masks.fits"))
    r["disk"], r["sky"] = m[0] > 0.5, m[1] > 0.5
    if need_stack:
        r["stack"] = read_fits(os.path.join(d, "stack.fits"))
    return r


def series_noise(full, even, odd, rep, cid):
    """Split-half measures for the series: {"sharpen": {A: (noise gain, detail gain)}, "chroma": {S: ratio}}."""
    res = {"sharpen": {}, "chroma": {}}
    if not even or not odd or "out" not in even or "out" not in odd or even["disk"].sum() == 0 or odd["disk"].sum() == 0:
        rep.info("split-half", "even / odd runs missing or without a disc: noise gains not measured")
        return res
    ge, go = even["display"].mean(axis=0), odd["display"].mean(axis=0)
    h, w = min(ge.shape[0], go.shape[0]), min(ge.shape[1], go.shape[1])
    region = dilate(even["disk"][:h, :w], 3)
    reg = register(ge[:h, :w], go[:h, :w], region)
    if reg is None:
        rep.info("split-half", "the halves did not register")
        return res
    dy, dx = reg
    half = lambda im, f: fourier_shift(im[:h, :w], f * dy, f * dx)  # noqa: E731
    disk_e = half(even["disk"].astype(float), -0.5) > 0.5
    disk_o = half(odd["disk"].astype(float), 0.5) > 0.5
    interior = erode(disk_e & disk_o, 2)
    interior[:8, :] = interior[-8:, :] = False
    interior[:, :8] = interior[:, -8:] = False
    if interior.sum() < 30:
        rep.info("split-half", "%d interior px shared by the halves (under 30)" % interior.sum())
        return res
    rep.info("split-half", "halves registered: odd moved by (%.2f, %.2f) px; %d interior px" % (dx, dy, interior.sum()))
    base = None
    for a in SHARPENS:
        k = "s%g" % a
        if k not in even["out"] or k not in odd["out"]:
            continue
        le = half(even["out"][k].mean(axis=0), -0.5)
        lo = half(odd["out"][k].mean(axis=0), 0.5)
        noise = float(np.std((le - lo)[interior]))
        lay = starlet(0.5 * (le + lo), 2)
        detail = float(np.sqrt(np.mean((lay[0] + lay[1])[interior] ** 2)))
        if base is None:
            base = (noise, detail)
        res["sharpen"][a] = (noise / base[0], detail / base[1])
    cbase = None
    for s in BLURS:
        k = "c%g" % s
        if k not in even["out"] or k not in odd["out"]:
            continue
        ce = np.stack([half(c, -0.5) for c in even["out"][k]])
        co = np.stack([half(c, 0.5) for c in odd["out"][k]])
        ch = (ce - ce.mean(axis=0)) - (co - co.mean(axis=0))
        v = float(np.sqrt(np.mean(ch[:, interior] ** 2)) / math.sqrt(2))
        if cbase is None:
            cbase = v
        res["chroma"][s] = (v / cbase if cbase > 0 else float("nan"), v)
    return res


def check(run, cid, role, png_dir):
    print("== %s (%s)" % (cid, role), flush=True)
    rep = Report(cid)
    base = os.path.join(run, cid)
    full = load_part(os.path.join(base, "full"), need_stack=True)
    j = full["json"]
    result = {"id": cid, "role": role, "rows": rep.rows, "series": {}}
    if "image" not in j:
        d = j.get("diagnosis", {})
        rep.add("stack", "%s: %s" % (d.get("kind", "?"), d.get("message", "")), "an image", "NOT JUDGED")
        return rep, result
    if "planet" not in j:
        raise HarnessError("%s: probe.json has an image but no planet result" % cid)
    pj = j["planet"]
    disc = pj["disc"]
    target = j["target"]
    print("  %dx%d stack, %d of %d frames used; target %s, filter %s" % (j["image"]["width"], j["image"]["height"],
          j["used"], j["measured"], target, j["filter"]))
    rep.info("disc", "%d px, equivalent radius %.1f px, box %s; sky %d px; %s" % (disc["pixels"], disc["equivRadius"],
             disc["box"], disc["sky"], history(j, "Planet disc")), pixels=disc["pixels"], radius=disc["equivRadius"])
    outs = full["out"]
    stack = full["stack"]

    # 6. clipping, on every output (also with no disc)
    bad, newclip = [], 0
    for name, im in outs.items():
        if not np.isfinite(im).all() or im.min() < 0 or im.max() > 1.0:
            bad.append("%s [%g, %g]" % (name, np.nanmin(im), np.nanmax(im)))
        newclip += int(((im >= CLIP) & (stack < CLIP)).sum())
    rep.judge("6 clip", "%d outputs, max %.4f; %d samples newly >= %.3f (stack had %d)%s" % (
        len(outs), max(im.max() for im in outs.values()), newclip, CLIP, int((stack >= CLIP).sum()),
        " -- out of range: " + ", ".join(bad) if bad else ""), "in [0, 1], 0 new", not bad and newclip == 0)
    rep.info("time", "slider %.4f s at %d threads, %dx%d" % (pj["sliderSeconds"], j["threads"], j["image"]["width"],
             j["image"]["height"]))

    if pj["notice"]:
        same = np.array_equal(outs["planet"], np.clip(stack, 0, 1).astype(np.float32).astype(np.float64))
        rep.judge("no disc", "notice: %s; output == the stack clipped to [0, 1]: %s" % (pj["notice"], same),
                  "Q3a: the stack clipped, with the reason", same)
        for req in ("1 calib", "2 sky", "3 disp", "4 halo", "4 fringe", "5 skynoise", "7 natural", "7 grey"):
            rep.add(req, "no disc: %s" % pj["notice"], "-", "NOT JUDGED")
        return rep, result

    disk, sky = full["disk"], full["sky"]
    H, W = disk.shape
    disp, unal, cal = full["display"], full["unaligned"], full["calibrated"]
    P = outs["planet"]

    # P6: the rings' place in the mask (Saturn)
    Ld = disp.mean(axis=0)
    if target == "Saturn":
        lit = Ld >= 0.1 * float(np.median(Ld[disk]))
        y0, y1, x0, x1 = crop_box(disc["box"], disk.shape)
        litc = lit[y0:y1, x0:x1]
        ys, xs = np.nonzero(litc)
        frac = float((litc & disk[y0:y1, x0:x1]).sum() / max(1, litc.sum()))
        rep.info("P6 rings", "%.1f%% of the %d px at >= 10%% of the disc median are in the mask; lit box %dx%d px, mask "
                 "box %dx%d px" % (100 * frac, litc.sum(), xs.max() - xs.min() + 1, ys.max() - ys.min() + 1,
                                   disc["box"][2] - disc["box"][0] + 1, disc["box"][3] - disc["box"][1] + 1),
                 rings_in_mask=frac)

    # 1. calibration
    line = history(j, "Colour calibration")
    tgt = REFLECTANCE.get(target)
    if line.startswith("Colour calibration skipped"):
        why = line.split(":", 1)[1].strip() if ":" in line else ""
        rep.add("1 calib", line, "within 2 % of the catalogue", "NOT JUDGED" if why else "FAIL")
    elif tgt is None:
        rep.add("1 calib", "no catalogue colour for %s" % target, "within 2 %", "FAIL")
    else:
        unsat = disk & (stack < CLIP).all(axis=0)
        m = [float(cal[c][unsat].mean()) for c in range(3)]
        bg, rg = m[2] / m[1], m[0] / m[1]
        err = max(abs(bg / tgt[0] - 1), abs(rg / tgt[1] - 1))
        rep.judge("1 calib", "disc B/G %.4f R/G %.4f vs %s's %.3f %.3f: %.2f%%" % (bg, rg, target, tgt[0], tgt[1],
                  100 * err), "<= 2 %", err <= 0.02, error=err)

    # 2. background
    if history(j, "Background").startswith("Background skipped") or sky.sum() < 100:
        rep.add("2 sky", history(j, "Background") or "no sky", "<= 0.5 % of disc, equal within noise", "NOT JUDGED")
    else:
        sm = [clipped_mean(cal[c][sky]) for c in range(3)]
        dmean = float(cal.mean(axis=0)[disk].mean())
        rel = max(abs(x) for x in sm) / dmean
        dn = [robust_std(cal[c][sky] - cal[1][sky]) for c in (0, 2)]
        spread = [abs(sm[c] - sm[1]) / n if n > 0 else float("inf") for c, n in zip((0, 2), dn)]
        rep.judge("2 sky", "sky mean R %.2e G %.2e B %.2e: max %.3f%% of the disc mean; R-G, B-G %.2f, %.2f sigmas" % (
            sm[0], sm[1], sm[2], 100 * rel, *spread), "<= 0.5 %, |c - G| <= 1 sigma", rel <= 0.005 and max(spread) <= 1)

    # crop for the limb measures
    y0, y1, x0, x1 = crop_box(disc["box"], disk.shape)
    cdisk, csky = disk[y0:y1, x0:x1], sky[y0:y1, x0:x1]
    edge = np.zeros(disk.shape, bool)
    edge[0, :] = edge[-1, :] = edge[:, 0] = edge[:, -1] = True
    cedge = edge[y0:y1, x0:x1]
    cd, cu, cP = disp[:, y0:y1, x0:x1], unal[:, y0:y1, x0:x1], P[:, y0:y1, x0:x1]

    # 3. dispersion
    line = history(j, "Channel alignment")
    eng = pj["dispersion"]
    if line.startswith("Channel alignment skipped"):
        rep.add("3 disp", line, "<= 0.1 px", "NOT JUDGED")
    else:
        before = limb_shift(cu, cdisk, csky, cedge)
        after = limb_shift(cd, cdisk, csky, cedge)
        for name, key in (("R", "r"), ("B", "b")):
            eb, ea = eng["before"][key], eng["after"][key]
            txt = "engine %+.3f,%+.3f (+-%.3f) -> %+.3f,%+.3f, %s" % (eb["dx"], eb["dy"], math.hypot(eb["ux"], eb["uy"]),
                                                                     ea["dx"], ea["dy"], "applied" if eb["applied"] else "declined")
            if after is None or before is None:
                rep.add("3 disp " + name, txt + "; limb estimator: under 12 rays", "<= 0.1 px", "NOT JUDGED")
                continue
            b, a = before[name], after[name]
            txt += "; limb estimator %+.3f,%+.3f (+-%.3f) -> %+.3f,%+.3f (+-%.3f) = %.3f px (%d rays)" % (
                b["dx"], b["dy"], b["u"], a["dx"], a["dy"], a["u"], a["mag"], after["rays"])
            if a["mag"] <= 0.1:
                rep.add("3 disp " + name, txt, "<= 0.1 px", "PASS", before=b, after=a, engine=eb)
            elif a["mag"] - 2 * a["u"] <= 0.1:
                # Over 0.1 px but within 2 sigma of it: this estimator cannot tell (never a PASS).
                rep.add("3 disp " + name, txt + "; %s, within 2 sigma of 0.1" % ("applied" if eb["applied"] else "declined"),
                        "<= 0.1 px", "NOT JUDGED", before=b, after=a, engine=eb)
            else:
                rep.add("3 disp " + name, txt, "<= 0.1 px", "FAIL", before=b, after=a, engine=eb)

    # 4. halo (B4)
    dout = dist_outside(disk, 6)
    band = (dout >= 1) & (dout <= 6) & ~dilate(edge, 6)
    if band.sum() > 20:
        delta = (full["lin_sharp"] - full["lin_unsharp"])[band]
        dm = float(full["lin_unsharp"][disk].mean())
        halo = max(abs(np.percentile(delta, 99.5)), abs(np.percentile(delta, 0.5))) / dm
        rep.judge("4 halo", "%.3f%% of the disc mean (%d band px)" % (100 * halo, band.sum()), "<= 2 %", halo <= 0.02)
    else:
        rep.add("4 halo", "no limb band inside the frame", "<= 2 %", "NOT JUDGED")

    # 4. fringe (P3)
    Hc = half_contrast(cd.mean(axis=0), cdisk, csky)
    ring = (dist_outside(Hc, 4) >= 1) & (dist_outside(Hc, 4) <= 4) & ~dilate(cedge, 1)
    ys, xs = np.nonzero(Hc)
    cy, cx = ys.mean(), xs.mean()
    axis = None
    eb = eng["before"]
    if eb is not None:
        vx = (eb["b"]["dx"] if eb["b"]["measured"] else 0) - (eb["r"]["dx"] if eb["r"]["measured"] else 0)
        vy = (eb["b"]["dy"] if eb["b"]["measured"] else 0) - (eb["r"]["dy"] if eb["r"]["measured"] else 0)
        if math.hypot(vx, vy) > 1e-6:
            axis = (vx / math.hypot(vx, vy), vy / math.hypot(vx, vy))
    noise_c = [robust_std(cal[c][sky]) / pj["curve"]["white"] for c in range(3)] if sky.sum() >= 100 else None
    if ring.sum() < 12 or noise_c is None:
        rep.add("4 fringe", "ring %d px, sky %d px" % (ring.sum(), sky.sum()), "<= 1.5 x synthetic", "NOT JUDGED")
    else:
        real = fringe_measure(cP, ring, cy, cx, axis)
        fb, fa = fringe_measure(cu, ring, cy, cx, axis), fringe_measure(cd, ring, cy, cx, axis)
        Lb = cd.mean(axis=0)
        ratio = [float(cd[c][cdisk].sum() / Lb[cdisk].sum()) for c in range(3)]
        Lp = cP.mean(axis=0)
        sigma = pj["chromaBlur"] * max(1.0, W / 2000.0)
        rng = np.random.default_rng(20261029)
        refs = {}
        for tol in (0.0, TOLERANCE):
            syn = []
            for _ in range(SYNTH_SEEDS):
                vals = []
                for a in ([axis] if axis else AXES8):
                    # R and B tol px either side of G along a (acceptance 3's tolerance), then the sky's noise.
                    planes = [fourier_shift(Lb * ratio[c], s * tol * a[1], s * tol * a[0]) for c, s in ((0, 1), (1, 0), (2, -1))]
                    ds = np.stack([np.clip(planes[c] + rng.normal(0, noise_c[c], Lb.shape), 0, 1) for c in range(3)])
                    vals.append(abs(side_asym(colour_step(ds, Lp, sigma), ring, cy, cx, a)))
                syn.append(max(vals))
            refs[tol] = float(np.percentile(syn, 95))
        ref = refs[TOLERANCE]
        rep.judge("4 fringe", "|side difference of (R-B)/L| %.4f (display before alignment %.4f, after %.4f) vs 1.5 x %.4f "
                  "(95th pct of %d synthetics, R and B %.1f px either side of G) = %.4f [zero shift: %.4f]; axis %s; ring %d px" % (
                      real, fb, fa, ref, SYNTH_SEEDS, TOLERANCE, 1.5 * ref, refs[0.0],
                      "(%.2f, %.2f)" % axis if axis else "worst of 8", ring.sum()),
                  "<= 1.5 x synthetic", real <= 1.5 * ref, value_real=real, before=fb, after=fa, synthetic=ref,
                  synthetic_zero=refs[0.0])

    # 5. sky noise (P2)
    s0 = outs.get("s0")
    if s0 is None:
        raise HarnessError("%s: no Sharpen 0 output (planet_s0.fits)" % cid)
    if sky.sum() >= 100:
        n0 = robust_std(s0.mean(axis=0)[sky])
        n1 = robust_std(P.mean(axis=0)[sky])
        if n0 > 0:
            rep.judge("5 skynoise", "%.3f (robust std %.2e -> %.2e at Sharpen %.2f)" % (n1 / n0, n0, n1, pj["sharpen"]),
                      "<= 1.5", n1 / n0 <= 1.5)
        else:
            rep.add("5 skynoise", "the sky's robust std is 0 at Sharpen 0 (clipped black)", "<= 1.5", "NOT JUDGED")
    else:
        rep.add("5 skynoise", "%d sky px" % sky.sum(), "<= 1.5", "NOT JUDGED")

    # 7. saturation
    if abs(pj["saturation"] - 1) > 1e-9:
        raise HarnessError("%s: the run's saturation is %g, the natural colour check needs 1" % (cid, pj["saturation"]))
    mo = [float(P[c][disk].sum()) for c in range(3)]
    md = [float(disp[c][disk].sum()) for c in range(3)]
    err = max(abs((mo[2] / mo[1]) / (md[2] / md[1]) - 1), abs((mo[0] / mo[1]) / (md[0] / md[1]) - 1))
    tg = REFLECTANCE.get(target, (float("nan"), float("nan")))
    terr = max(abs((mo[2] / mo[1]) / tg[0] - 1), abs((mo[0] / mo[1]) / tg[1] - 1))
    rep.judge("7 natural", "output B/G %.4f R/G %.4f vs the display's %.4f %.4f: %.2f%% (vs the catalogue: %.2f%%)" % (
        mo[2] / mo[1], mo[0] / mo[1], md[2] / md[1], md[0] / md[1], 100 * err, 100 * terr), "<= 2 %", err <= 0.02,
        error=err, catalogue_error=terr)
    k0 = outs.get("k0")
    if k0 is None:
        raise HarnessError("%s: no Saturation 0 output (planet_k0.fits)" % cid)
    grey = float(max(np.abs(k0[0] - k0[1]).max(), np.abs(k0[2] - k0[1]).max()))
    rep.judge("7 grey", "max |R-G|, |B-G| %.2e" % grey, "<= 1e-6", grey <= 1e-6)

    # ---- the series ----
    even = odd = None
    if os.path.isfile(os.path.join(base, "even", "probe.json")) and os.path.isfile(os.path.join(base, "odd", "probe.json")):
        even, odd = load_part(os.path.join(base, "even")), load_part(os.path.join(base, "odd"))
    split = series_noise(full, even, odd, rep, cid)
    dm = float(Ld[disk].mean())
    Hfull = np.zeros(disk.shape, bool)
    Hfull[y0:y1, x0:x1] = Hc
    limb = (dist_outside(Hfull, 1.5) <= 1.5) & (dist_outside(~Hfull, 1.5) <= 1.5) & ~dilate(edge, 2)
    inner = erode(disk, 2)
    l0 = s0.mean(axis=0)
    sharpen_rows = []
    base_vals = None
    for a in SHARPENS:
        im = outs.get("s%g" % a)
        if im is None:
            raise HarnessError("%s: no Sharpen %g output" % (cid, a))
        L = im.mean(axis=0)
        gy, gx = np.gradient(L)
        crisp = float(np.hypot(gy, gx)[limb].mean()) / dm if limb.sum() else float("nan")
        lay = starlet(L, 2)
        fine = float(np.sqrt(np.mean((lay[0] + lay[1])[inner] ** 2))) / dm if inner.sum() else float("nan")
        dl = (L - l0)[disk]
        over, under = float(np.percentile(dl, 99.5)) / dm, float(-np.percentile(dl, 0.5)) / dm
        skyn = robust_std(L[sky]) if sky.sum() >= 100 else float("nan")
        if base_vals is None:
            base_vals = (crisp, fine, skyn)
        ng, dg = split["sharpen"].get(a, (float("nan"), float("nan")))
        row = {"amount": a, "crisp": crisp / base_vals[0], "fine": fine / base_vals[1], "noise_gain": ng,
               "detail_gain": dg, "over": over, "under": under,
               "sky": skyn / base_vals[2] if base_vals[2] and base_vals[2] > 0 else float("nan")}
        sharpen_rows.append(row)
        rep.info("S %-4g" % a, "crisp x%.3f, fine detail x%.3f, disc noise gain x%.3f (detail x%.3f), over %.2f%% under "
                 "%.2f%% of the disc mean, sky noise x%.3f%s" % (row["crisp"], row["fine"], ng, dg, 100 * over, 100 * under,
                                                               row["sky"], "  <- default" if abs(a - pj["sharpen"]) < 1e-9 else ""))
    chroma_rows = []
    cbase = None
    for s in BLURS:
        im = outs.get("c%g" % s)
        if im is None:
            raise HarnessError("%s: no Chroma blur %g output" % (cid, s))
        ch = im - im.mean(axis=0)[None]
        v = float(np.sqrt(np.mean(ch[:, sky] ** 2))) if sky.sum() >= 100 else float("nan")
        if cbase is None:
            cbase = v
        dr, dv = split["chroma"].get(s, (float("nan"), float("nan")))
        # The disc's colour structure (chroma rms over the interior, signal and noise) as a share of blur 0's: what
        # the blur takes from the bands' and the rings' colour.
        colour = float(np.sqrt(np.mean(ch[:, inner] ** 2))) if inner.sum() else float("nan")
        if s == BLURS[0]:
            cbase_disc = colour
        row = {"sigma": s, "sky": v, "sky_ratio": v / cbase if cbase and cbase > 0 else float("nan"),
               "disc_ratio": dr, "disc_abs": dv, "colour_ratio": colour / cbase_disc if cbase_disc > 0 else float("nan")}
        chroma_rows.append(row)
        rep.info("C %-4g" % s, "sky chroma rms %.2e (%.3f of 8-bit levels) x%.3f; disc split-half chroma noise x%.3f "
                 "(%.2f 8-bit levels); disc colour rms x%.3f%s" % (
                     v, 255 * v, row["sky_ratio"], dr, 255 * dv, row["colour_ratio"],
                     "  <- default" if abs(s - pj["chromaBlur"]) < 1e-9 else ""))
    result["series"] = {"sharpen": sharpen_rows, "chroma": chroma_rows, "defaults": {"sharpen": pj["sharpen"],
                        "chromaBlur": pj["chromaBlur"]}, "radius": disc["equivRadius"], "has_sky": bool(sky.sum() >= 100)}
    if png_dir:
        pngs(png_dir, cid, full, stack, pj, outs, disc)
    return rep, result


# ---- PNGs ----

def pngs(png_dir, cid, full, stack, pj, outs, disc):
    from PIL import Image, ImageDraw
    os.makedirs(png_dir, exist_ok=True)
    y0, y1, x0, x1 = crop_box(disc["box"], stack.shape[1:], margin=max(10, int(0.35 * max(
        disc["box"][2] - disc["box"][0] + 1, disc["box"][3] - disc["box"][1] + 1))))

    def panel(rgb, label):
        a = (np.clip(np.moveaxis(rgb[:, y0:y1, x0:x1], 0, -1), 0, 1) * 255 + 0.5).astype(np.uint8)
        a = np.repeat(np.repeat(a, 4, axis=0), 4, axis=1)
        strip = Image.new("RGB", (a.shape[1], 18), (0, 0, 0))
        ImageDraw.Draw(strip).text((3, 3), label, fill=(255, 255, 0))
        return np.concatenate([np.asarray(strip), a], axis=0)

    def row(panels, path):
        h = max(p.shape[0] for p in panels)
        sep = np.full((h, 6, 3), 128, np.uint8)
        parts = []
        for i, p in enumerate(panels):
            parts += ([sep] if i else []) + [np.pad(p, ((0, h - p.shape[0]), (0, 0), (0, 0)))]
        Image.fromarray(np.concatenate(parts, axis=1)).save(path)

    white = pj["curve"]["white"]
    row([panel(stack / white, "plain stack (/ white %.3f)" % white),
         panel(outs["planet"], "processed: Sharpen %g, blur %g" % (pj["sharpen"], pj["chromaBlur"]))],
        os.path.join(png_dir, "%s_plain_vs_processed.png" % cid))
    row([panel(outs["s%g" % a], "Sharpen %g%s" % (a, " (default)" if abs(a - pj["sharpen"]) < 1e-9 else ""))
         for a in SHARPENS], os.path.join(png_dir, "%s_sharpen_series.png" % cid))
    row([panel(outs["c%g" % s], "Chroma blur %g%s" % (s, " (default)" if abs(s - pj["chromaBlur"]) < 1e-9 else ""))
         for s in BLURS], os.path.join(png_dir, "%s_chroma_series.png" % cid))


# ---- the gate and the run ----

def gate(run):
    print("== gate (2026-10-05 Saturn HaO3)", flush=True)
    rep = Report("gate")
    for b in ("auto", "RGGB"):
        ex = os.path.join(run, "gate", "%s.exit" % b)
        if not os.path.isfile(ex):
            rep.add("gate " + b, "missing %s" % ex, "exit 4, narrowband", "FAIL")
            continue
        code = open(ex).read().strip()
        try:
            refusal = load_json(os.path.join(run, "gate", "%s.json" % b)).get("planet", {}).get("refusal") or ""
        except HarnessError as e:
            refusal = str(e)
        rep.judge("gate " + b, "exit %s: %s" % (code, refusal), "exit 4, narrowband", code == "4" and "narrowband" in refusal)
    return rep


def pick_blur(results, key="sky_ratio", sigmas=(0.0, 0.5, 1.0, 1.5, 2.0, 2.5)):
    """The brief's rule: the smallest Chroma blur of 0-2.5 that halves the sky's chroma noise (key "sky_ratio"; or the
    disc's split-half one, "disc_ratio") on every capture that has it."""
    have = [r for r in results if any(np.isfinite(c.get(key, float("nan"))) for c in r.get("series", {}).get("chroma", []))]
    if not have:
        return None
    for s in sigmas:
        if all(any(abs(c["sigma"] - s) < 1e-9 and c[key] <= 0.5 for c in r["series"]["chroma"]) for r in have):
            return s
    return None


def main(argv):
    png_dir, no_png, args = None, False, []
    i = 0
    while i < len(argv):
        if argv[i] == "--png-dir":
            png_dir = argv[i + 1]
            i += 2
        elif argv[i] == "--no-png":
            no_png = True
            i += 1
        else:
            args.append(argv[i])
            i += 1
    if not args:
        print(__doc__)
        return 1
    run = args[0]
    ids = args[1:]
    if not ids:
        ids = sorted(d for d in os.listdir(run) if os.path.isfile(os.path.join(run, d, "full", "probe.json"))) \
            if os.path.isdir(run) else []
    if not ids:
        print("FAIL: no capture under %s" % run)
        return 1
    if no_png:
        png_dir = None
    elif png_dir is None:
        png_dir = os.path.join(run, "png")
    reports, results, failed = [], [], []
    for spec in ids:
        cid, _, role = spec.partition(":")
        try:
            rep, res = check(run, cid, role or "acceptance", png_dir)
        except HarnessError as e:
            rep, res = Report(cid), {"id": cid}
            rep.add("harness", str(e), "every input present", "FAIL")
            res["rows"] = rep.rows
        reports.append(rep)
        results.append(res)
        try:
            json.dump(res, open(os.path.join(run, cid, "check.json"), "w"), indent=1, default=float)
        except OSError:
            pass
    g = gate(run)
    reports.append(g)
    blur = pick_blur(results)
    disc_blur = pick_blur(results, "disc_ratio")
    print("== Chroma blur by the brief's rule (smallest of 0-2.5 halving the sky's chroma noise on every capture with a sky): "
          "%s; by the disc's split-half chroma noise: %s" % ("%g" % blur if blur is not None else "none",
                                                             "%g" % disc_blur if disc_blur is not None else "none"))
    for r in reports:
        failed += ["%s %s" % (r.cid, row["req"]) for row in r.failed()]
    json.dump({"captures": results, "gate": g.rows, "chromaBlurPick": blur, "chromaBlurPickDisc": disc_blur, "failed": failed},
              open(os.path.join(run, "summary.json"), "w"), indent=1, default=float)
    print("== %s" % ("FAIL: " + ", ".join(failed) if failed else "PASS: nothing failed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
