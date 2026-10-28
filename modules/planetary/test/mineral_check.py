#!/usr/bin/env python3
"""Mineral moon real-data acceptance checks (Task B9), over run-mineral-real.sh's output.

    mineral_check.py <out-dir> <id>:<role> ...

Each <out-dir>/<id>/ holds full/, even/ and odd/: stack-probe --mineral runs on every frame and on the even and
the odd frames alone (probe.json, mineral.fits, stages/: display, lum_sharp, lin_unsharp, lin_sharp, calibrated,
masks; full/ also stack.fits and, for 64.6F, mineral_k5.fits / mineral_k12.fits). Every requirement prints its
number, its bar and PASS / FAIL / NOT JUDGED (a requirement whose input is missing on this capture -- a skipped
step, a side with no disk -- is not judged, never failed; Ruling R16). The requirements (spec Part B, numbered as
there, and the B9 dispatch):

  notice      MineralOutcome::notice empty (a notice on a real Moon is a FAIL).
  range       every mineral output (k 8, the extra k, both halves): finite, in [0, 1].
  clip        no new clipping: output samples >= 0.999 whose stack sample is < 0.999 (kClippedLuma): 0.
  time        the mineral steps' stepSeconds total <= 5 s (32 threads; the frame size is printed).
  2 calib     linear calibrated RGB, disk pixels with no sample >= 0.999 in the stack: mean R, B within 1 % of G.
  3 sky       linear, sky pixels, 3-sigma-clipped means: |mean| per channel <= 0.5 % of the disk mean, and R's and B's
              mean within one sigma of G's, sigma = the robust std of the per-pixel difference (c - G).
  4 DAN       DAN_interior (bench/metric.py) of the linear L, sharpened vs not (lin_sharp vs lin_unsharp: both
              through the inverse display transfer), on the unsharpened image's box and interior window: rise
              >= 10 % (Ruling R25's bar carried to real data).
  4 skynoise  linear L's robust std over the sky, sharpened / not: <= 1.5.
  4 halo      sharpened - unsharpened linear L in the band 1-6 px outside the disk mask (frame border excluded):
              its 0.5 / 99.5 percentiles' larger magnitude <= 2 % of the disk mean.
  5 chroma    chroma NOISE (Ruling R12: noise, not colour) of the display RGB, split-half: the halves' chroma (each
              channel less the channels' mean) differenced / sqrt 2, rms over the disc and over the sky (16 px clear
              of their edges), after the chroma blur (Gaussian, sigma 2.5 px x max(1, width / 2000), as ChromaLayer)
              <= 0.5 x before. (A single image's chroma std counts the Moon's colour texture: it read x0.6-0.85.)
              Sky not judged when the display transfer clipped it black.
  5 lumdetail DAN_interior of the mineral output's luminance vs the sharpened L's (display space): within 2 %.
  7 cyan      brightest 1 % of disk pixels (output luminance): mean(min(G, B) - R) / mean(L) <= 0.02; the line also
              gives the same pixels' cyan before the colour step and the fade weight w there.
  8 median    disk median of the output's luminance in [0.25, 0.45] (the transfer aims at 0.35).
  R12 edge    edge_chroma --noise on the even / odd mineral outputs, per frame side, bands 0:6 vs 20:40, along the
              longest run where both bands are disc 12 px clear of the limb (disc_span; at least 50 px, else that
              side is not judged): every judgeable side <= 1.5x; none judgeable = NOT JUDGED. Each judged side
              also prints its per-depth ratios (depth 0-3) and the same ratio on the display RGB (before the
              colour step), the stage of origin.
  R31 gain    sharpening's noise gain on the disc: the halves registered (on the mineral images' G; every stage
              is on that grid; edge_chroma.register, both resampled half-way), half 1 fitted to half 0 by a gain
              and offset over the interior (disk in both, 24 px from its edge), then std(s0 - s1') / std(u0 - u1')
              over the interior, s = sharpened L, u = unsharpened display L; also over the flattest 10 % of the
              interior. Ruling R37: on real data even the flattest decile is texture, so the difference is noise
              riding boosted texture and scales with the gain; the bar is noise gain <= detail gain, the rms of the
              sharpened bands (starlet layers 1-4) of the halves' mean, sharpened over unsharpened (the limiter
              failing would boost noise beyond the detail). The noise-only guarantee is the unit tests' (flat disc).
  M3 align    the Channel alignment HISTORY line of the full stack and both halves (shift +- uncertainty,
              applied or not); a channel resampled at |shift| < 0.2 px is flagged LOOK (a limb-fringe look).
  M4 stretch  the Display transfer line, and the disk's top / median (99.99th percentile of unsaturated / median
              of the linear L) and display-L std: informational.

PNGs: <out-dir>/<id>_side.png, the display RGB (calibrated, stretched, unboosted) beside the k 8 mineral image,
and <id>_side_view.png at half scale. For the reference capture, <id>_k{5,8,12}_vs_ref.png and
<id>_k8_vs_ref1026.png: ours registered to Scott's PNG by phase correlation and cropped to the common field,
with <id>_grid_view.png (k 5, k 8, k 12, reference at half scale). Task SHARP: the reference capture's Sharpen
series (full/mineral_s<A>.fits, stack-probe --sharpen-also, plus the run's own amount) at k 8 beside the reference:
<id>_sharpen_crop100.png (x 820..1300, y 560..980 at 100 %, the Copernicus / ray area) and
<id>_sharpen_view.png (the whole frame at half scale), each panel labelled, the reference last; and "sharp crisp",
the crispness of each amount against the reference (both luminances on one grid, the reference's quantile-mapped
onto our amount-0 output over the disc interior -- disk eroded 40 px -- so the tone curves drop out): starlet layers
1-5 rms over the interior as a ratio to the reference's, and "rims" = layers 2-3 together. Writes <id>/check.json and summary.json.
Exit 0 when nothing failed, 1 otherwise.
"""
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, "bench")]

import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402
from scipy import ndimage  # noqa: E402

import edge_chroma  # noqa: E402
import metric  # noqa: E402

CLIP = 0.999
REF = os.path.expanduser("~/Desktop/MineralMoon.png")
REF_1026 = os.path.expanduser("~/Desktop/Mineral_Moon_10_26_2026.png")
SIDES = ("left", "right", "top", "bottom")


def read_fits(path):
    """stack-probe's FITS (ProbeFits.h): (C, H, W) float64, row 0 = top; C = 1 for a 2-axis file."""
    cards = {}
    with open(path, "rb") as f:
        done = False
        while not done:
            block = f.read(2880)
            if len(block) < 2880:
                raise ValueError("%s: short FITS header" % path)
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
        raise ValueError("%s: short data" % path)
    return np.frombuffer(raw, ">f4").reshape(n, h, w).astype(np.float64)


class Report:
    def __init__(self, cid):
        self.cid, self.rows = cid, []

    def add(self, req, value, bar, verdict, **extra):
        row = {"req": req, "value": value, "bar": bar, "verdict": verdict}
        row.update(extra)
        self.rows.append(row)
        print("  %-11s %-9s %s  [bar: %s]" % (req, verdict, value, bar), flush=True)

    def judge(self, req, value, bar, ok, **extra):
        self.add(req, value, bar, "PASS" if ok else "FAIL", **extra)

    def failed(self):
        return [r for r in self.rows if r["verdict"] == "FAIL"]


def load(d, full=False):
    j = json.load(open(os.path.join(d, "probe.json")))
    s = os.path.join(d, "stages")
    r = {"json": j, "mineral": read_fits(os.path.join(d, "mineral.fits")),
         "display": read_fits(os.path.join(s, "display.fits")),
         "lum_sharp": read_fits(os.path.join(s, "lum_sharp.fits"))[0],
         "lin_unsharp": read_fits(os.path.join(s, "lin_unsharp.fits"))[0],
         "lin_sharp": read_fits(os.path.join(s, "lin_sharp.fits"))[0],
         "calibrated": read_fits(os.path.join(s, "calibrated.fits"))}
    m = read_fits(os.path.join(s, "masks.fits"))
    r["disk"], r["sky"] = m[0] > 0.5, m[1] > 0.5
    if full:
        r["stack"] = read_fits(os.path.join(d, "stack.fits"))
        r["also"] = {a["strength"]: read_fits(a["output"]) for a in j["mineral"]["also"]}
        r["sharpen"] = {a["sharpen"]: read_fits(a["output"]) for a in j["mineral"].get("sharpenAlso", [])}
        r["sharpen"][j["mineral"]["sharpen"]] = r["mineral"]
    return r


def history(j, prefix):
    return next((h for h in j["mineral"]["history"] if h.startswith(prefix)), "")


ALIGN = re.compile(r"R ([+-][\d.]+), ([+-][\d.]+) px \(\+-([\d.]+)\), B ([+-][\d.]+), ([+-][\d.]+) px \(\+-([\d.]+)\)")


def alignment(j):
    """(line, {"R": |shift|, "B": |shift|}, resampled channels)."""
    line = history(j, "Channel alignment")
    m = ALIGN.search(line)
    shifts = {}
    if m:
        v = [float(x) for x in m.groups()]
        shifts = {"R": math.hypot(v[0], v[1]), "R_unc": v[2], "B": math.hypot(v[3], v[4]), "B_unc": v[5]}
    applied = []
    if "R and B resampled" in line:
        applied = ["R", "B"]
    elif "R resampled" in line:
        applied = ["R"]
    elif "B resampled" in line:
        applied = ["B"]
    return line, shifts, applied


def robust_std(v):
    return 1.4826 * float(np.median(np.abs(v - np.median(v)))) if v.size > 1 else float("nan")


def clipped_mean(v, rounds=3, k=3.0):
    m = np.ones(v.size, dtype=bool)
    for _ in range(rounds):
        mu, sd = v[m].mean(), v[m].std()
        m = np.abs(v - mu) <= k * sd
    return float(v[m].mean())


def lum(rgb):
    return rgb.mean(axis=0)


def dan_interior(anchor, img):
    box = metric.target_box(anchor)
    iw = metric.interior_window(anchor, box)
    a = metric.measure(anchor, box, interior=iw)
    b = metric.measure(img, box, interior=iw)
    return a, b, iw


def chroma_blur(rgb):
    sigma = 2.5 * max(1.0, rgb.shape[2] / 2000.0)
    return np.stack([ndimage.gaussian_filter(c, sigma, mode="mirror", truncate=4.0) for c in rgb])


def register_halves(even, odd):
    """Half 1 onto half 0, on the mineral images' G (edge_chroma.register: whole pixels by phase correlation, then
    Lucas-Kanade). Every stage of a half is on its mineral image's grid. (The display L picked a wrong whole-pixel
    offset on the hazy capture, whose display sky is black and whose glow reads as disk.)"""
    return edge_chroma.register(even["mineral"][1], odd["mineral"][1])


def registered(even, odd, reg, planes):
    """The halves' `planes` (lists of HxW arrays), each resampled half-way and cut to the overlap: (o0, o1)."""
    h0 = edge_chroma.catmull_rom(np.stack(planes(even)), reg["ry"] / 2, reg["rx"] / 2)
    h1 = edge_chroma.catmull_rom(np.stack(planes(odd)), -reg["ry"] / 2, -reg["rx"] / 2)
    o0, o1, _ = edge_chroma.overlap(h0, h1, reg["dy"], reg["dx"])
    return o0, o1


def disc_span(inner, side, depth=40, min_run=50):
    """Along `side` of the boolean (H, W) `inner` (disk at least 12 px from the limb, the frame's edge not counting as
    one): the longest run of positions whose first `depth` px in from that side are all inner, as (a, b), or None
    when it is under `min_run` px. The bands then hold disc only: where an edge band crosses the limb, the halves'
    registration residual on the limb step reads 3.8-4.6x even with no processing at all (64.6F, rows 740-800)."""
    o = edge_chroma.oriented(inner[None], side)[0]
    ok = o[:, :depth].all(axis=1)
    best, start = None, None
    for i, v in enumerate(np.append(ok, False)):
        if v and start is None:
            start = i
        elif not v and start is not None:
            if best is None or i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    return best if best is not None and best[1] - best[0] >= min_run else None


def fmt_ratio(a, b):
    return "n/a" if a is None or not b else "%.1f" % (a / b)


def split_half_edges(rep, even, odd, reg, req="R12 edge", info=False):
    """R12 on the mineral images, and the same on the display RGB (before the colour step) as the stage of origin."""
    if "why" in reg:
        rep.add(req, "halves do not register: " + reg["why"], "each judgeable side <= 1.5x", "INFO" if info else "FAIL")
        return
    p0, p1 = even["mineral"], odd["mineral"]
    _, _, (y0, x0) = edge_chroma.overlap(p0, p1, reg["dy"], reg["dx"])
    h, w = min(p0.shape[1], p1.shape[1] + reg["dy"]) - y0, min(p0.shape[2], p1.shape[2] + reg["dx"]) - x0
    disk = np.pad(even["disk"], 64, mode="edge")
    inner = (ndimage.distance_transform_edt(disk) > 12)[64:-64, 64:-64][y0:y0 + h, x0:x0 + w]
    results, parts, origin = {}, [], []
    for side in SIDES:
        rows = disc_span(inner, side)
        if rows is None:
            parts.append("%s n/j" % side)
            results[side] = {"verdict": "not judged", "why": "the disc (12 px in from the limb) reaches this side for "
                                                              "under 50 px"}
            continue
        r = edge_chroma.measure_noise(p0, p1, side, rows, (0, 6), (20, 40))
        results[side] = {"verdict": r["verdict"], "rows": "%d:%d" % rows, "B/G": r["B/G"]["ratio"], "R/G": r["R/G"]["ratio"],
                         "unmeasurable": r.get("unmeasurable")}
        if r["verdict"] == "not judged":
            parts.append("%s n/j" % side)
            continue
        depth = []
        for d in range(4):
            q = edge_chroma.measure_noise(p0, p1, side, rows, (d, d + 1), (20, 40))
            depth.append("/".join(fmt_ratio(q[k]["edge_noise"], q[k]["ref_noise"]) for k in ("B/G", "R/G")))
        disp = edge_chroma.measure_noise(even["display"], odd["display"], side, rows, (0, 6), (20, 40))
        results[side]["display"] = [disp["B/G"]["ratio"], disp["R/G"]["ratio"]]
        parts.append("%s[%d:%d] %s/%s" % (side, rows[0], rows[1], fmt_ratio(r["B/G"]["ratio"], 1), fmt_ratio(r["R/G"]["ratio"], 1)))
        origin.append("%s depth 0-3 %s, display stage %.2f/%.2f" % (side, " ".join(depth), disp["B/G"]["ratio"], disp["R/G"]["ratio"]))
    judged = {s: r for s, r in results.items() if r["verdict"] != "not judged"}
    value = "B/G / R/G edge/interior noise: " + ", ".join(parts) + ("; " + "; ".join(origin) if origin else "")
    if not judged:
        rep.add(req, value + " (no side judgeable)", "each judgeable side <= 1.5x", "NOT JUDGED", sides=results)
    else:
        ok = all(r["verdict"] == "pass" for r in judged.values())
        rep.add(req, value, "each judgeable side <= 1.5x", "INFO" if info else ("PASS" if ok else "FAIL"),
                  sides=results)


def chroma_noise_split(rep, even, odd, reg):
    """Spec step 5 on real data as noise, not colour (Ruling R12): the halves' chroma (each channel less the channels'
    mean) differenced, / sqrt 2, rms over the disc and the sky (16 px clear of their edges), before and after the chroma
    blur. A single image's chroma std would count the Moon's colour texture as noise (it read x0.6-0.85)."""
    if "why" in reg:
        rep.add("5 chroma", "halves do not register: " + reg["why"], "<= 0.5x", "FAIL")
        return
    out = {}
    for label, fn in (("before", lambda x: x), ("after", chroma_blur)):
        o0, o1 = registered(even, odd, reg, lambda p: list(fn(p["display"])) + [p["disk"].astype(float), p["sky"].astype(float)])
        dk = ndimage.distance_transform_edt((o0[3] > 0.99) & (o1[3] > 0.99)) > 16
        sk = ndimage.distance_transform_edt((o0[4] > 0.99) & (o1[4] > 0.99)) > 16
        d = ((o0[:3] - o0[:3].mean(axis=0)) - (o1[:3] - o1[:3].mean(axis=0))) / np.sqrt(2)
        out[label] = [float(np.sqrt((d[:, m] ** 2).mean())) if m.sum() > 1000 else None for m in (dk, sk)]
    for i, name in ((0, "5 chroma"), (1, "5 chroma sky")):
        b, a = out["before"][i], out["after"][i]
        if b is None or not b > 1e-6:
            rep.add(name, "%s (%s)" % ("too few px" if b is None else "%.2e" % b, "under 1000 px 16 px clear" if b is None else
                                       "clipped black by the display transfer"), "<= 0.5x", "NOT JUDGED")
        else:
            rep.judge(name, "split-half chroma noise %.2e -> %.2e (x%.3f)" % (b, a, a / b), "<= 0.5x", a <= 0.5 * b)


def noise_gain(rep, even, odd, reg):
    if "why" in reg:
        rep.add("R31 gain", "halves do not register: " + reg["why"], "<= 1.5", "FAIL")
        return
    o0, o1 = registered(even, odd, reg, lambda p: [lum(p["display"]), p["lum_sharp"], p["disk"].astype(float)])
    both = (o0[2] > 0.99) & (o1[2] > 0.99)
    both[:2, :] = both[-2:, :] = False
    both[:, :2] = both[:, -2:] = False
    interior = ndimage.distance_transform_edt(both) > 24
    if interior.sum() < 1000:
        rep.add("R31 gain", "%d interior px" % interior.sum(), "<= 1.5", "NOT JUDGED")
        return
    a, b = np.polyfit(o1[0][interior], o0[0][interior], 1)
    du = (o0[0] - (a * o1[0] + b))[interior]
    ds = (o0[1] - (a * o1[1] + b))[interior]
    g = float(np.std(ds) / np.std(du))
    gr = robust_std(ds) / robust_std(du)
    # The flattest tenth of the interior (gradient magnitude at 2 px): where texture mismatch between the halves is
    # least, so the gain there is closest to the noise's own.
    tex = ndimage.gaussian_gradient_magnitude(0.5 * (o0[0] + o1[0]), 2.0)
    flat = interior & (tex <= np.percentile(tex[interior], 10))
    gf = float(np.std(ds[flat[interior]]) / np.std(du[flat[interior]]))
    # Ruling R37: on real data even the flattest decile is texture, so the halves' difference is noise riding boosted
    # texture and grows with the gain by construction. The limiter fails only when the noise is boosted more than the
    # detail: the detail gain is the rms of the sharpened bands (starlet layers 1-4) of the halves' mean, sharpened over
    # unsharpened, over the same interior.
    mu = 0.5 * (o0[0] + a * o1[0] + b)
    ms = 0.5 * (o0[1] + a * o1[1] + b)
    lu, ls = starlet(mu), starlet(ms)
    per = [float(np.sqrt((s_[interior] ** 2).mean() / (u_[interior] ** 2).mean())) for u_, s_ in zip(lu, ls)]
    detail = float(np.sqrt(sum((s_[interior] ** 2).mean() for s_ in ls) / sum((u_[interior] ** 2).mean() for u_ in lu)))
    # Per band: the difference's own layers, sharpened over unsharpened. The whole-image detail gain weighs the layers
    # by the detail's energy, which sits in the coarse layers 3-4 (nominal gain 1), the noise's in layers 1-2 (1.5, 2.5).
    nu = starlet(o0[0] - (a * o1[0] + b))
    ns = starlet(o0[1] - (a * o1[1] + b))
    pern = [float(np.sqrt((s_[interior] ** 2).mean() / (u_[interior] ** 2).mean())) for u_, s_ in zip(nu, ns)]
    ok = all(n <= d for n, d in zip(pern, per))
    rep.judge("R31 gain", "noise gain %.3f (robust %.3f, flattest 10%% %.3f) vs detail gain %.3f; per layer 1-4 noise / detail: "
              "%s; diff std unsharp %.5f, sharp %.5f; %d px; halves at (%d, %d) + (%.2f, %.2f), G match %.3f" % (
                  g, gr, gf, detail, " ".join("%.2f/%.2f" % nd for nd in zip(pern, per)), np.std(du), np.std(ds),
                  interior.sum(), reg["dx"], reg["dy"], reg["rx"], reg["ry"], reg["match"]),
              "per layer, noise gain <= detail gain (R37)", ok, gain=g, robust=gr, flat=gf, detail=detail, layers=per,
              noiseLayers=pern, wholeImagePass=g <= detail)


def starlet(x, n=4):
    """The B3-spline starlet's detail layers 1..n of a 2-D image (a trous, mirrored edges), as the engine's."""
    k = np.array([1, 4, 6, 4, 1]) / 16.0
    c, out = x, []
    for j in range(n):
        kk = np.zeros(4 * 2 ** j + 1)
        kk[::2 ** j] = k
        cn = ndimage.convolve1d(ndimage.convolve1d(c, kk, axis=0, mode="mirror"), kk, axis=1, mode="mirror")
        out.append(c - cn)
        c = cn
    return out


def to8(rgb):
    return (np.clip(np.moveaxis(rgb, 0, -1), 0, 1) * 255 + 0.5).astype(np.uint8)


def save(path, arr8, view=False):
    Image.fromarray(arr8).save(path)
    if view:
        im = Image.fromarray(arr8)
        im.resize((im.width // 2, im.height // 2), Image.LANCZOS).save(path.replace(".png", "_view.png"))


def side_by_side(*imgs, gap=8):
    h = max(i.shape[0] for i in imgs)
    out = [np.pad(i, ((0, h - i.shape[0]), (0, 0), (0, 0))) for i in imgs]
    sep = np.full((h, gap, 3), 128, np.uint8)
    row = [out[0]]
    for i in out[1:]:
        row += [sep, i]
    return np.concatenate(row, axis=1)


def register_to_ref(ours8, ref8):
    """(dy, dx, corr): ours' pixel (y, x) is the reference's (y + dy, x + dx), by phase correlation on luminance
    zero-padded to a common size; corr: the luminance correlation over the common field."""
    a, b = ours8.mean(axis=2), ref8.mean(axis=2)
    H, W = max(a.shape[0], b.shape[0]), max(a.shape[1], b.shape[1])
    pa = np.zeros((H, W))
    pb = np.zeros((H, W))
    pa[:a.shape[0], :a.shape[1]] = a - a.mean()
    pb[:b.shape[0], :b.shape[1]] = b - b.mean()
    cross = np.fft.fft2(pb) * np.conj(np.fft.fft2(pa))
    cross /= np.abs(cross) + 1e-12
    c = np.fft.ifft2(cross).real
    y, x = np.unravel_index(int(np.argmax(c)), c.shape)
    dy, dx = (y if y < H // 2 else y - H), (x if x < W // 2 else x - W)
    o, r = common(ours8, ref8, dy, dx)
    corr = float(np.corrcoef(o.mean(axis=2).ravel(), r.mean(axis=2).ravel())[0, 1])
    return int(dy), int(dx), corr


def common(ours8, ref8, dy, dx):
    y0, x0 = max(0, -dy), max(0, -dx)
    y1, x1 = min(ours8.shape[0], ref8.shape[0] - dy), min(ours8.shape[1], ref8.shape[1] - dx)
    return ours8[y0:y1, x0:x1], ref8[y0 + dy:y1 + dy, x0 + dx:x1 + dx]


def labelled(arr8, text):
    im = Image.fromarray(np.ascontiguousarray(arr8))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 7 * len(text) + 8, 15], fill=(0, 0, 0))
    d.text((4, 2), text, fill=(255, 255, 0))
    return np.asarray(im)


def sharpen_series(out, cid, full, ref, reg, rep):
    """Task SHARP: the Sharpen amounts beside the reference (crops at 100 %, the frame at half scale) and their
    crispness against it (see the module doc)."""
    series = full.get("sharpen", {})
    if len(series) < 2:
        return
    amounts = sorted(series)
    crop = (slice(560, 980), slice(820, 1300))
    crops, views = [], []
    for a in amounts + ["ref"]:
        if a == "ref":
            o, r = common(to8(series[amounts[0]]), ref, reg[0], reg[1])
            img, text = r, "MineralMoon.png"
        else:
            img, _ = common(to8(series[a]), ref, reg[0], reg[1])
            text = "Sharpen %.2f" % a
        crops.append(labelled(img[crop], text))
        half = np.asarray(Image.fromarray(img).resize((img.shape[1] // 2, img.shape[0] // 2), Image.LANCZOS))
        views.append(labelled(half, text))
    save(os.path.join(out, "%s_sharpen_crop100.png" % cid), side_by_side(*crops))
    save(os.path.join(out, "%s_sharpen_view.png" % cid), side_by_side(*views))
    # Crispness: luminance on our grid; the reference (on our grid at the registration offset) quantile-mapped onto
    # our amount-0 output over the disc interior.
    interior = ndimage.distance_transform_edt(np.pad(full["disk"], 64, mode="edge"))[64:-64, 64:-64] > 40
    H, W = interior.shape
    refL = np.zeros((H, W))
    y0, x0 = max(0, -reg[0]), max(0, -reg[1])
    y1, x1 = min(H, ref.shape[0] - reg[0]), min(W, ref.shape[1] - reg[1])
    refL[y0:y1, x0:x1] = ref[y0 + reg[0]:y1 + reg[0], x0 + reg[1]:x1 + reg[1]].mean(axis=2) / 255.0
    inside = np.zeros_like(interior)
    inside[y0:y1, x0:x1] = True
    interior &= inside
    base = lum(series[amounts[0]])
    rs, bs = np.sort(refL[interior]), np.sort(base[interior])
    q = np.linspace(0, 1, rs.size)
    er = [float(np.sqrt((l[interior] ** 2).mean())) for l in starlet(np.interp(np.interp(refL, rs, q), q, bs), 5)]
    rows = {}
    for a in amounts:
        e = [float(np.sqrt((l[interior] ** 2).mean())) for l in starlet(lum(series[a]), 5)]
        rims = float(np.sqrt((e[1] ** 2 + e[2] ** 2) / (er[1] ** 2 + er[2] ** 2)))
        rows["%.2f" % a] = {"layers": [x / y for x, y in zip(e, er)], "rims": rims}
    text = "; ".join("amount %s: layers 1-5 %s, rims %.3f" % (a, " ".join("%.2f" % v for v in r["layers"]), r["rims"])
                     for a, r in rows.items())
    rep.add("sharp crisp", "ours / MineralMoon.png starlet rms over the disc interior (%d px): %s" % (interior.sum(), text),
            "informational (Scott's eye picks the default)", "INFO", amounts=rows)


def reference_pngs(out, cid, full, rep):
    ref = np.asarray(Image.open(REF).convert("RGB"))
    ks = {8.0: full["mineral"]}
    ks.update(full["also"])
    grid = []
    reg = None
    for k in sorted(ks):
        o8 = to8(ks[k])
        if reg is None:
            reg = register_to_ref(to8(full["mineral"]), ref)
        o, r = common(o8, ref, reg[0], reg[1])
        save(os.path.join(out, "%s_k%d_vs_ref.png" % (cid, k)), side_by_side(o, r))
        grid.append(o)
    grid.append(r)
    half = [np.asarray(Image.fromarray(g).resize((g.shape[1] // 2, g.shape[0] // 2), Image.LANCZOS)) for g in grid]
    top, bottom = side_by_side(half[0], half[1]), side_by_side(half[2], half[3])
    Image.fromarray(np.concatenate([top, np.full((8, top.shape[1], 3), 128, np.uint8), bottom])).save(
        os.path.join(out, "%s_grid_view.png" % cid))
    info = {"ref": REF, "offset": [reg[1], reg[0]], "corr": reg[2], "common": [int(o.shape[1]), int(o.shape[0])]}
    rep.add("ref reg", "ours (0, 0) = MineralMoon.png (%d, %d); luminance corr %.3f over %d x %d" % (
        reg[1], reg[0], reg[2], o.shape[1], o.shape[0]), "registration sanity (corr >= 0.9)",
        "PASS" if reg[2] >= 0.9 else "FAIL", **info)
    sharpen_series(out, cid, full, ref, reg, rep)
    if os.path.exists(REF_1026):
        r26 = np.asarray(Image.open(REF_1026).convert("RGB"))
        o8 = to8(full["mineral"])
        g = register_to_ref(o8, r26)
        o, r = common(o8, r26, g[0], g[1])
        save(os.path.join(out, "%s_k8_vs_ref1026.png" % cid), side_by_side(o, r))
        rep.add("ref1026", "ours (0, 0) = 10-26 PNG (%d, %d); luminance corr %.3f over %d x %d" % (
            g[1], g[0], g[2], o.shape[1], o.shape[0]), "informational", "INFO")
    return info


def check(out, cid, role):
    print("== %s (%s)" % (cid, role), flush=True)
    rep = Report(cid)
    base = os.path.join(out, cid)
    full = load(os.path.join(base, "full"), full=True)
    even, odd = load(os.path.join(base, "even")), load(os.path.join(base, "odd"))
    j = full["json"]
    mj = j["mineral"]
    H, W = full["mineral"].shape[1:]
    disk, sky = full["disk"], full["sky"]
    print("  %dx%d stack, %d of %d frames used; %s" % (W, H, j["used"], j["measured"], history(j, "Lunar disk")))

    for name, part in (("full", full), ("even", even), ("odd", odd)):
        n = part["json"]["mineral"]["notice"]
        rep.judge("notice", "%s: %s" % (name, n or "(none)"), "empty", n == "")

    outs = {"k8": full["mineral"], "even": even["mineral"], "odd": odd["mineral"]}
    outs.update({"k%g" % k: v for k, v in full["also"].items()})
    bad = []
    for name, im in outs.items():
        if not np.isfinite(im).all() or im.min() < 0 or im.max() > 1.0:
            bad.append("%s [%g, %g]" % (name, np.nanmin(im), np.nanmax(im)))
    rep.judge("range", "; ".join("%s max %.4f" % (n, im.max()) for n, im in outs.items()) + (
        " -- out of range: " + ", ".join(bad) if bad else ""), "finite, in [0, 1]", not bad)

    new_clip = int(((full["mineral"] >= CLIP) & (full["stack"] < CLIP)).sum())
    for k, im in full["also"].items():
        new_clip_k = int(((im >= CLIP) & (full["stack"] < CLIP)).sum())
        rep.judge("clip k%g" % k, "%d samples newly >= %.3f" % (new_clip_k, CLIP), "0", new_clip_k == 0)
    rep.judge("clip", "%d samples newly >= %.3f (stack had %d)" % (new_clip, CLIP, int((full["stack"] >= CLIP).sum())),
              "0", new_clip == 0)

    rep.judge("time", "%.3f s at %d threads, %dx%d (%s)" % (mj["seconds"], j["threads"], W, H, ", ".join(
        "%s %.3f" % kv for kv in mj["stepSeconds"].items())), "<= 5 s", mj["seconds"] <= 5.0 and j["threads"] == 32,
        seconds=mj["seconds"])

    # 2. calibration
    cal = full["calibrated"]
    unsat = disk & (full["stack"] < CLIP).all(axis=0)
    if history(j, "Colour calibration").startswith("Colour calibration skipped"):
        rep.add("2 calib", history(j, "Colour calibration"), "R, B within 1 % of G", "NOT JUDGED")
    else:
        m = [float(cal[c][unsat].mean()) for c in range(3)]
        dev = max(abs(m[0] / m[1] - 1), abs(m[2] / m[1] - 1))
        rep.judge("2 calib", "disk mean R %.5f G %.5f B %.5f: max dev %.3f%%" % (m[0], m[1], m[2], 100 * dev),
                  "<= 1 %", dev <= 0.01)

    # 3. background
    if history(j, "Background").startswith("Background skipped") or sky.sum() == 0:
        rep.add("3 sky", history(j, "Background") or "no sky", "<= 0.5 % of disk, equal within noise", "NOT JUDGED")
    else:
        # Means with three rounds of 3-sigma rejection (the background step's own fit rejects so; plain means carry the
        # limb's glow and stars), and the noise of a channel difference measured on the per-pixel difference itself
        # (robust std): from the channels' own robust stds it collapses where a channel's sky is quantised -- 0528a's
        # G sky, 54.8% of it one value, read 8.4e-6, which put B-G at 3.51 "sigma"; measured directly, 0.90.
        sm = [clipped_mean(cal[c][sky]) for c in range(3)]
        dm = [float(cal[c][disk].mean()) for c in range(3)]
        rel = max(abs(sm[c]) / dm[c] for c in range(3))
        dn = [robust_std(cal[c][sky] - cal[1][sky]) for c in (0, 2)]
        spread = [abs(sm[c] - sm[1]) / n if n > 0 else float("inf") for c, n in zip((0, 2), dn)]
        rep.judge("3 sky", "sky mean (3-sigma clipped) R %.2e G %.2e B %.2e: max %.3f%% of disk; R-G, B-G %.2f, %.2f "
                  "sigmas of the per-pixel difference (robust std %.1e, %.1e)" % (sm[0], sm[1], sm[2], 100 * rel, *spread, *dn),
                  "<= 0.5 % of disk, |c - G| <= 1 sigma", rel <= 0.005 and max(spread) <= 1)

    # 4. sharpening
    try:
        a, b, iw = dan_interior(full["lin_unsharp"], full["lin_sharp"])
        if not iw.judged:
            rep.add("4 DAN", "interior %.0f px across (< %g)" % (iw.diameter, metric.INTERIOR_MIN_DIAMETER), ">= +10 %",
                    "NOT JUDGED")
        else:
            rise = b.interior / a.interior - 1
            rep.judge("4 DAN", "DAN_interior %.5f -> %.5f: %+.1f%% (DAN full %+.1f%%; window %.0f px across)" % (
                a.interior, b.interior, 100 * rise, 100 * (b.value / a.value - 1), iw.diameter), ">= +10 %", rise >= 0.10,
                rise=rise)
    except ValueError as e:
        rep.add("4 DAN", "metric error: %s" % e, ">= +10 %", "FAIL")
    su = robust_std(full["lin_unsharp"][sky]) if sky.sum() > 100 else 0.0
    if su > 0:
        ratio = robust_std(full["lin_sharp"][sky]) / su
        rep.judge("4 skynoise", "%.3f" % ratio, "<= 1.5", ratio <= 1.5)
    else:
        rep.add("4 skynoise", "%d sky px, robust std %.1e (the display transfer clipped the sky black; the sharpening's "
                "boost is 0 off the disc)" % (sky.sum(), su), "<= 1.5", "NOT JUDGED")
    dout = ndimage.distance_transform_edt(~disk)
    band = (dout >= 1) & (dout <= 6)
    band[:6, :] = band[-6:, :] = False
    band[:, :6] = band[:, -6:] = False
    if band.sum() > 100:
        delta = (full["lin_sharp"] - full["lin_unsharp"])[band]
        dmean = float(full["lin_unsharp"][disk].mean())
        halo = max(abs(np.percentile(delta, 99.5)), abs(np.percentile(delta, 0.5))) / dmean
        rep.judge("4 halo", "%.3f%% of the disk mean (%d band px)" % (100 * halo, band.sum()), "<= 2 %", halo <= 0.02)
    else:
        rep.add("4 halo", "no limb inside the frame", "<= 2 %", "NOT JUDGED")

    # 5. chroma noise reduction, luminance detail
    disp = full["display"]
    reg = register_halves(even, odd)
    chroma_noise_split(rep, even, odd, reg)
    try:
        a, b, iw = dan_interior(full["lum_sharp"], lum(full["mineral"]))
        if iw.judged:
            d = b.interior / a.interior - 1
            # Where the +-0.06 clamp holds a channel, the channels' mean of d is no longer 0 and the output's luminance
            # leaves L (Mineral.h): the share of disc pixels where it moved by over 0.001.
            moved = float((np.abs(lum(full["mineral"]) - full["lum_sharp"])[disk] > 1e-3).mean())
            rep.judge("5 lumdetail", "DAN_interior L %.5f, output luminance %.5f: %+.2f%% (luminance moved > 0.001 on "
                      "%.2f%% of the disc)" % (a.interior, b.interior, 100 * d, 100 * moved), "within 2 %", abs(d) <= 0.02)
        else:
            rep.add("5 lumdetail", "interior too small", "within 2 %", "NOT JUDGED")
    except ValueError as e:
        rep.add("5 lumdetail", "metric error: %s" % e, "within 2 %", "FAIL")

    # 7. highlights, 8. display level
    Lo = lum(full["mineral"])
    thr = np.percentile(Lo[disk], 99)
    top = disk & (Lo >= thr)
    r_, g_, b_ = (full["mineral"][c][top] for c in range(3))
    cyan = float((np.minimum(g_, b_) - r_).mean() / Lo[top].mean())
    sat = float(((np.max([r_, g_, b_], axis=0) - np.min([r_, g_, b_], axis=0)) / np.maximum(Lo[top], 1e-6)).mean())
    # Context: the same pixels before the colour step (display RGB), and the fade weight w there.
    dr, dg, db = (disp[c][top] for c in range(3))
    cyan0 = float((np.minimum(dg, db) - dr).mean() / lum(disp)[top].mean())
    C = chroma_blur(disp)
    wt = np.clip((mj["satThreshold"] - np.maximum(C[0], C[1])) / 0.12, 0, 1)[top]
    rep.judge("7 cyan", "brightest 1%% (L >= %.3f): cyan %.4f, mean saturation %.4f (display stage cyan %.4f; fade "
              "threshold %.3f, mean w there %.2f)" % (thr, cyan, sat, cyan0, mj["satThreshold"], float(wt.mean())),
              "cyan <= 0.02", cyan <= 0.02)
    med = float(np.median(Lo[disk]))
    rep.judge("8 median", "%.3f" % med, "0.25-0.45", 0.25 <= med <= 0.45)

    # R12, R31
    # R12 is judged on the halves cut to their common field (even_eq / odd_eq, stack-probe --mineral-crop): the halves'
    # stacks reach 1-4 px apart on each side, and a chroma blur at a frame's edge sees only its own field, so on the
    # whole fields the halves differ by the colour beyond the nearer edge -- 64.6F with no channel alignment at all
    # read left 6.37/10.95, bottom 2.11/3.00 there, against 1.17/1.08 and 1.02/0.96 before the colour step. The
    # whole-field figure is kept, as INFO.
    eq = [os.path.join(base, p, "probe.json") for p in ("even_eq", "odd_eq")]
    if all(os.path.exists(p) for p in eq):
        ev_eq, od_eq = load(os.path.join(base, "even_eq")), load(os.path.join(base, "odd_eq"))
        split_half_edges(rep, ev_eq, od_eq, register_halves(ev_eq, od_eq))
        split_half_edges(rep, even, odd, reg, "R12 full", info=True)
    else:
        split_half_edges(rep, even, odd, reg)
    noise_gain(rep, even, odd, reg)

    # M3, M4
    looks = []
    for name, part in (("full", full), ("even", even), ("odd", odd)):
        line, shifts, applied = alignment(part["json"])
        flag = [c for c in applied if shifts.get(c, 1) < 0.2]
        looks += ["%s %s" % (name, c) for c in flag]
        rep.add("M3 align", "%s: %s" % (name, line), "resampled at |shift| < 0.2 px -> LOOK", "LOOK" if flag else "INFO",
                shifts=shifts, applied=applied)
    lin_l = lum(cal)
    unsat_l = lin_l[unsat]
    ratio = float(np.percentile(unsat_l, 99.99) / np.median(unsat_l)) if unsat_l.size else float("nan")
    rep.add("M4 stretch", "%s; linear disk top/median %.3f; display-L disk std %.4f" % (
        history(j, "Display transfer"), ratio, float(np.std(lum(disp)[disk]))), "informational (Scott's eye)", "INFO",
        top_median=ratio)

    # PNGs
    save(os.path.join(out, "%s_side.png" % cid), side_by_side(to8(disp), to8(full["mineral"])), view=True)
    ref = reference_pngs(out, cid, full, rep) if role == "reference" else None

    res = {"id": cid, "role": role, "size": [W, H], "rows": rep.rows, "failed": [r["req"] for r in rep.failed()],
           "history": mj["history"], "wall": {p: open(os.path.join(base, p, "wall.txt")).read().strip()
                                              for p in ("full", "even", "odd") if os.path.exists(os.path.join(base, p, "wall.txt"))},
           "ref": ref}
    json.dump(res, open(os.path.join(base, "check.json"), "w"), indent=1)
    return res


def gate(out):
    print("== gate (HaO3)", flush=True)
    ok = True
    for name, want in (("rggb", "narrowband"), ("auto", "mono")):
        try:
            code = int(open(os.path.join(out, "gate", name + ".exit")).read())
            j = json.load(open(os.path.join(out, "gate", name + ".json")))
            text = j["mineral"]["refusal"] or ""
        except (OSError, ValueError, KeyError, TypeError) as e:
            code, text = -1, "unreadable: %s" % e
        good = code == 4 and want in text
        ok &= good if name == "rggb" else True
        print("  gate %-5s %-4s exit %d: %s  [bar: %s]" % (name, "PASS" if good else ("FAIL" if name == "rggb" else "INFO"), code,
                                                       text, "exit 4 with the %s text" % want))
    return ok


def equal_field(cdir):
    """The --mineral-crop of the even and the odd half (L,T,R,B each) that cuts both to their common field, from their
    mineral images' whole-pixel registration (the sub-pixel residual, under 1 px, stays)."""
    p0, p1 = (read_fits(os.path.join(cdir, p, "mineral.fits")) for p in ("even", "odd"))
    reg = edge_chroma.register(p0[1], p1[1])
    if "why" in reg:
        raise ValueError("%s: the halves do not register: %s" % (cdir, reg["why"]))
    dy, dx = reg["dy"], reg["dx"]
    (h0, w0), (h1, w1) = p0.shape[1:], p1.shape[1:]
    x0, x1, y0, y1 = max(0, dx), min(w0, w1 + dx), max(0, dy), min(h0, h1 + dy)
    if x1 - x0 < 16 or y1 - y0 < 16:
        raise ValueError("%s: the halves share under 16 x 16 px" % cdir)
    return "%d,%d,%d,%d" % (x0, y0, w0 - x1, h0 - y1), "%d,%d,%d,%d" % (x0 - dx, y0 - dy, w1 - (x1 - dx), h1 - (y1 - dy))


def main(argv):
    if len(argv) == 3 and argv[1] == "--equal-field":
        print(" ".join(equal_field(argv[2])))
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 1
    out = argv[1]
    results, fails = [], 0
    for spec in argv[2:]:
        cid, _, role = spec.partition(":")
        try:
            r = check(out, cid, role or "acceptance")
        except (OSError, ValueError, KeyError) as e:
            print("  ERROR %s: %s" % (cid, e))
            r = {"id": cid, "role": role, "failed": ["harness: %s" % e]}
        fails += bool(r["failed"])
        results.append(r)
    gate_ok = gate(out)
    print("== summary")
    for r in results:
        print("  %-12s %-12s %s" % (r["id"], r["role"], "FAIL: " + ", ".join(r["failed"]) if r["failed"] else "all judged PASS"))
    print("  gate         %s" % ("PASS" if gate_ok else "FAIL"))
    json.dump({"captures": results, "gate": gate_ok}, open(os.path.join(out, "summary.json"), "w"), indent=1)
    return 0 if fails == 0 and gate_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
