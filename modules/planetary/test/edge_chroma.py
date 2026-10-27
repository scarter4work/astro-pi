#!/usr/bin/env python3
"""Edge chroma check on a stack-probe colour stack (Rulings R8, R12): is the chroma noisier at a frame edge?

    edge_chroma.py <stack.fits> [--rows 320:760] [--edge 0:6] [--ref 20:40] [--side left|right|top|bottom] [--json]
    edge_chroma.py --noise <half0.fits> <half1.fits> [same options]

Split-half mode (--noise; Ruling R12, the acceptance check on real data): the two inputs are independent
half-stacks of one capture (`stack-probe --parity even` and `--parity odd`). Colour the light carries is the
same in both, so the per-pixel difference of a ratio between them is noise alone: for each band it reports
std((ratio0 - ratio1) / sqrt 2) of B/G and of R/G (one half-stack's noise), over the pixels that are disk
(G >= 0.05) in both, and exits 0 when the edge band's is <= 1.5 x the reference band's for both ratios.
Registration. Each half-stack is a run of its own, with its own crop and its own reference, and the reference
sits at the mean position of that half's sharpest frames (Align.h BuildReference). So the halves' grids differ
by whole pixels plus an arbitrary fraction (x / y): 0.16 / 0.12 px on the 64.6F, 0.16 / 0.60 px on the 70.0F.
Left in, the fraction makes the difference carry the scene's colour gradient: registered to the whole pixel
only, the 70.0F top band read 3x its noise. So half 1 is registered onto half 0 on G. First comes the whole-pixel offset, by phase correlation:
"offset" x, y says half 1's pixel (0, 0) is half 0's (x, y). Then the sub-pixel residual ("residual_x",
"residual_y"), by Lucas-Kanade iterated to convergence over pixels that are disk in both, 2 px clear of the
limb. Both halves are then resampled half-way, half 0 by +residual/2 and half 1 by -residual/2. Each uses
Catmull-Rom (the engine's own interpolator) on its whole image, so a half's own pixels beyond the overlap feed
the overlap's border (edge-replicated only past its own edge). Both halves get the same interpolation
filter, so the ratio edge / reference is unchanged by it, though the noise values come out up to ~20% lower
than an unresampled half's (a quarter pixel on both axes).
The bands are taken on the overlap of the two -- its edges and its --rows -- whose pixel (0, 0) is half 0's
("overlap" x, y). It fails closed (1) when the registered halves' G correlates under 0.9 over the disk (they
do not show one scene) or the residual does not converge under 1 px. Two half-stacks see each frame once
between them, so the overlap's outermost pixels are the ones fewer frames reach. This is the same thinning a
full stack has at its edge, though the overlap's edge can lie a few px inside either half's own.

Input: `stack-probe --out x.fits` (32-bit float, NAXIS3 = 3 planes R, G, B, first row = top). Per pixel
it forms B/G and R/G over the disk (G >= 0.05; sky is left out), then takes each ratio's std over the
edge band and over the reference band, and reports edge std / reference std.

  --side   the frame edge measured (default left).
  --rows   the span ALONG that edge, in image pixels: rows for left/right, columns for top/bottom
           (default 320:760, the spec's band for the 2026-10-23 64.6F Moon, whose disk also covers columns
           320-760 of the top and bottom rows). On that capture the default takes in the limb at rows
           740-763 of the left edge, which raises the edge std: left B/G 3.80x with 320:760, 2.46x with 320:740.
  --edge   the edge band, as distances from that edge in pixels (default 0:6, i.e. the 6 outermost).
  --ref    the reference band, the same way (default 20:40).

Ranges are half-open A:B. Exit 0 when both ratios are <= 1.5, 1 otherwise -- including whenever a band
cannot be measured (fewer than 100 disk pixels, or a non-finite sample in it): the check fails closed.
Exit 2 for a bad argument, an unreadable file, a malformed header (NAXIS1 / NAXIS2 missing) or a stack
that is not 3 float planes.

Limit (Rulings R8, R12): the single-stack std of B/G and R/G measures colour structure of any origin, not
noise alone -- what the frames recorded counts as much as what the engine adds. A ratio over 1.5 says
the edge band is more colourful than the reference band, not why. Read it as a verdict on the engine
only on a stack whose input has no colour structure of its own (a synthetic capture of uniform,
white-balanced colour). On real data it is a measuring tool, not an acceptance bar: the 64.6F edge colour
it flags is real lunar colour (Ruling R11's 70.0F cross-check). The acceptance check is --noise.
"""
import argparse
import json
import sys

import numpy as np

BAR = 1.5        # edge std / reference std above this is a defect
DISK_G = 0.05    # pixels with G below this are sky, not measured
MIN_PIXELS = 100  # a band with fewer disk pixels is unmeasurable
MIN_MATCH = 0.9  # --noise: registered halves whose G correlates under this over the disk are not one scene


def read_fits(path):
    """stack-probe's FITS (test/ProbeFits.h): (3, H, W) float64, row 0 = top."""
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
        if cards.get("BITPIX") != "-32" or cards.get("NAXIS") != "3" or cards.get("NAXIS3") != "3":
            raise ValueError("%s: expected a 32-bit float stack of 3 planes (BITPIX -32, NAXIS 3, NAXIS3 3), got "
                             "BITPIX %s NAXIS %s NAXIS3 %s" % (path, cards.get("BITPIX"), cards.get("NAXIS"),
                                                                cards.get("NAXIS3")))
        try:
            w, h = int(cards["NAXIS1"]), int(cards["NAXIS2"])
        except (KeyError, ValueError):
            raise ValueError("%s: NAXIS1 / NAXIS2 missing or not integers (%r, %r)"
                             % (path, cards.get("NAXIS1"), cards.get("NAXIS2")))
        if w < 1 or h < 1:
            raise ValueError("%s: NAXIS1 %d x NAXIS2 %d is empty" % (path, w, h))
        raw = f.read(w * h * 3 * 4)
        if len(raw) != w * h * 3 * 4:
            raise ValueError("%s: short data (%d of %d bytes)" % (path, len(raw), w * h * 3 * 4))
    return np.frombuffer(raw, ">f4").reshape(3, h, w).astype(np.float64)


def span(text):
    try:
        a, b = (int(v) for v in text.split(":"))
    except ValueError:
        raise argparse.ArgumentTypeError("expected A:B (integers), got %r" % text)
    if not 0 <= a < b:
        raise argparse.ArgumentTypeError("expected 0 <= A < B, got %r" % text)
    return a, b


def oriented(planes, side):
    """(3, along, depth): axis 1 runs along the edge, axis 2 away from it (depth 0 = the edge)."""
    if side == "left":
        return planes
    if side == "right":
        return planes[:, :, ::-1]
    if side == "top":
        return planes.transpose(0, 2, 1)
    return planes[:, ::-1, :].transpose(0, 2, 1)  # bottom


def band_stats(planes, along, depth):
    """{"B/G": std, "R/G": std, "pixels": n} over one band, or None for a field that cannot be measured."""
    r, g, b = (p[along[0]:along[1], depth[0]:depth[1]] for p in planes)
    if not (np.isfinite(r).all() and np.isfinite(g).all() and np.isfinite(b).all()):
        return {"pixels": 0, "B/G": None, "R/G": None, "why": "non-finite sample in the band"}
    disk = g >= DISK_G
    n = int(disk.sum())
    if n < MIN_PIXELS:
        return {"pixels": n, "B/G": None, "R/G": None,
                "why": "%d disk pixels (G >= %g), need %d" % (n, DISK_G, MIN_PIXELS)}
    gd = g[disk]
    return {"pixels": n, "B/G": float(np.std(b[disk] / gd)), "R/G": float(np.std(r[disk] / gd))}


def measure(planes, side, rows, edge, ref):
    o = oriented(planes, side)
    along_n, depth_n = o.shape[1], o.shape[2]
    for name, (a, b), n in (("--rows", rows, along_n), ("--edge", edge, depth_n), ("--ref", ref, depth_n)):
        if b > n:
            raise ValueError("%s %d:%d is outside the image (%d px along that axis for --side %s)"
                             % (name, a, b, n, side))
    e, f = band_stats(o, rows, edge), band_stats(o, rows, ref)
    out = {"side": side, "rows": "%d:%d" % rows, "edge": "%d:%d" % edge, "ref": "%d:%d" % ref, "bar": BAR,
           "edge_pixels": e["pixels"], "ref_pixels": f["pixels"]}
    why = [w for w in (e.get("why"), f.get("why")) if w]
    ok = not why
    for k in ("B/G", "R/G"):
        es, rs = e[k], f[k]
        ratio = es / rs if es is not None and rs is not None and rs > 0 else None
        out[k] = {"edge_std": es, "ref_std": rs, "ratio": ratio}
        ok = ok and ratio is not None and ratio <= BAR
    if why:
        out["unmeasurable"] = "; ".join(why)
    out["pass"] = ok
    return out


def catmull_rom(p, sy, sx):
    """(C, H, W) sampled at (y + sy, x + sx) for |sy|, |sx| <= 1: Catmull-Rom (the engine's bicubic) along each
    axis, the image's own edge replicated past it."""
    out = p
    for axis, s in ((1, sy), (2, sx)):
        i = int(np.floor(s))
        f = s - i
        if i == 0 and f == 0:
            continue
        w = ((-f ** 3 + 2 * f ** 2 - f) / 2, (3 * f ** 3 - 5 * f ** 2 + 2) / 2, (-3 * f ** 3 + 4 * f ** 2 + f) / 2,
             (f ** 3 - f ** 2) / 2)
        pad = [(0, 0), (0, 0), (0, 0)]
        pad[axis] = (2, 3)
        q = np.pad(out, pad, mode="edge")
        n = out.shape[axis]
        out = sum(wk * np.take(q, np.arange(n) + 2 + i + k - 1, axis=axis) for k, wk in enumerate(w))
    return out


def overlap(p0, p1, dy, dx):
    """The common rectangle of two (C, H, W) stacks with p0(y, x) ~ p1(y - dy, x - dx): (o0, o1, (y0, x0)),
    (y0, x0) being the overlap's pixel (0, 0) on p0's grid."""
    (h0, w0), (h1, w1) = p0.shape[1:], p1.shape[1:]
    y0, y1, x0, x1 = max(0, dy), min(h0, h1 + dy), max(0, dx), min(w0, w1 + dx)
    if y1 <= y0 or x1 <= x0:
        raise ValueError("the halves do not overlap at offset (%d, %d)" % (dx, dy))
    return p0[:, y0:y1, x0:x1], p1[:, y0 - dy:y1 - dy, x0 - dx:x1 - dx], (y0, x0)


def register(g0, g1):
    """Half 1 onto half 0 on G: {"dy", "dx", "ry", "rx", "peak", "match"} with g0(p) ~ g1(p - (dy + ry, dx + rx)),
    or {"why": ...} when they cannot be registered. (dy, dx): phase correlation over the common top-left size;
    (ry, rx): Lucas-Kanade on the overlap, iterated (half 1 resampled by Catmull-Rom each time) to convergence,
    over the pixels that are disk in both 2 px clear of the limb and of the overlap's border (a step there is
    no small shift's gradient); match: G's correlation over those pixels once registered."""
    g0, g1 = np.nan_to_num(g0), np.nan_to_num(g1)  # a non-finite sample fails its band later, not this
    h, w = min(g0.shape[0], g1.shape[0]), min(g0.shape[1], g1.shape[1])
    a, b = g0[:h, :w] - g0[:h, :w].mean(), g1[:h, :w] - g1[:h, :w].mean()
    cross = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    cross /= np.abs(cross) + 1e-12
    c = np.fft.ifft2(cross).real
    y, x = np.unravel_index(int(np.argmax(c)), c.shape)
    dy, dx = (int(y) if y < h // 2 else int(y) - h), (int(x) if x < w // 2 else int(x) - w)
    o0, o1, _ = overlap(g0[None], g1[None], dy, dx)
    disk = np.pad((o0[0] >= DISK_G) & (o1[0] >= DISK_G), 2, constant_values=False)
    m = np.ones(o0.shape[1:], dtype=bool)
    for sy in range(5):
        for sx in range(5):
            m &= disk[sy:sy + m.shape[0], sx:sx + m.shape[1]]
    reg = {"dy": dy, "dx": dx, "ry": None, "rx": None, "peak": float(c[y, x]), "match": None}
    if m.sum() < MIN_PIXELS:
        reg["why"] = "the halves share %d disk pixels at offset (%d, %d), need %d" % (m.sum(), dx, dy, MIN_PIXELS)
        return reg
    r = np.zeros(2)
    for _ in range(20):
        s1 = catmull_rom(o1, -r[0], -r[1])[0]
        gy, gx = np.gradient(0.5 * (o0[0] + s1))
        # o0(p) = o1(p - r) ~ o1(p) - r . grad  =>  r = -(J^T J)^-1 J^T (o0 - o1)
        jy, jx, e = gy[m], gx[m], (o0[0] - s1)[m]
        jtj = np.array([[jy @ jy, jy @ jx], [jx @ jy, jx @ jx]])
        if abs(np.linalg.det(jtj)) <= 1e-12 * max(1.0, float(np.trace(jtj)) ** 2):
            reg["why"] = "G has no gradient to register the halves on"
            return reg
        step = -np.linalg.solve(jtj, np.array([jy @ e, jx @ e]))
        r = r + step
        if np.abs(r).max() > 1:
            reg["why"] = "the halves' sub-pixel residual ran past 1 px: they do not register at offset (%d, %d)" % (dx, dy)
            return reg
        if np.abs(step).max() < 5e-3:
            break
    else:
        reg["why"] = "the halves' sub-pixel residual did not converge"
        return reg
    reg["ry"], reg["rx"] = float(r[0]), float(r[1])
    reg["match"] = float(np.corrcoef(o0[0][m], catmull_rom(o1, -r[0], -r[1])[0][m])[0, 1])
    if not reg["match"] >= MIN_MATCH:
        reg["why"] = "registered, the halves' G correlates %.2f over the disk (under %g): not one scene" % (
            reg["match"], MIN_MATCH)
    return reg


def noise_band(o0, o1, along, depth):
    """{"B/G": noise, "R/G": noise, "pixels": n}: std((ratio0 - ratio1) / sqrt 2) over the band's pixels that
    are disk in both halves, or None for a field that cannot be measured."""
    q0 = [p[along[0]:along[1], depth[0]:depth[1]] for p in o0]
    q1 = [p[along[0]:along[1], depth[0]:depth[1]] for p in o1]
    if not all(np.isfinite(q).all() for q in q0 + q1):
        return {"pixels": 0, "B/G": None, "R/G": None, "why": "non-finite sample in the band"}
    disk = (q0[1] >= DISK_G) & (q1[1] >= DISK_G)
    n = int(disk.sum())
    if n < MIN_PIXELS:
        return {"pixels": n, "B/G": None, "R/G": None,
                "why": "%d pixels are disk (G >= %g) in both halves, need %d" % (n, DISK_G, MIN_PIXELS)}
    g0, g1 = q0[1][disk], q1[1][disk]
    out = {"pixels": n}
    for k, c in (("B/G", 2), ("R/G", 0)):
        out[k] = float(np.std((q0[c][disk] / g0 - q1[c][disk] / g1) / np.sqrt(2)))
    return out


def measure_noise(p0, p1, side, rows, edge, ref):
    reg = register(p0[1], p1[1])
    out = {"mode": "noise", "side": side, "rows": "%d:%d" % rows, "edge": "%d:%d" % edge, "ref": "%d:%d" % ref,
           "bar": BAR, "offset": {"x": reg["dx"], "y": reg["dy"], "residual_x": reg["rx"], "residual_y": reg["ry"],
                                  "peak": reg["peak"], "match": reg["match"]}}
    why = [reg["why"]] if "why" in reg else []
    e = f = {"pixels": 0, "B/G": None, "R/G": None}
    if not why:
        # Half-way each (see the module docstring): p0'(q) = p0(q + r/2), p1'(q) = p1(q - r/2), so p0'(q) ~ p1'(q - d).
        h0 = catmull_rom(p0, reg["ry"] / 2, reg["rx"] / 2)
        h1 = catmull_rom(p1, -reg["ry"] / 2, -reg["rx"] / 2)
        o0, o1, (y0, x0) = overlap(h0, h1, reg["dy"], reg["dx"])
        out["overlap"] = {"x": x0, "y": y0, "width": o0.shape[2], "height": o0.shape[1]}
        v0, v1 = oriented(o0, side), oriented(o1, side)
        for name, (a, b), n in (("--rows", rows, v0.shape[1]), ("--edge", edge, v0.shape[2]), ("--ref", ref, v0.shape[2])):
            if b > n:
                raise ValueError("%s %d:%d is outside the halves' overlap (%d px along that axis for --side %s)"
                                 % (name, a, b, n, side))
        e, f = noise_band(v0, v1, rows, edge), noise_band(v0, v1, rows, ref)
        why += [w for w in (e.get("why"), f.get("why")) if w]
    out["edge_pixels"], out["ref_pixels"] = e["pixels"], f["pixels"]
    ok = not why
    for k in ("B/G", "R/G"):
        en, rn = e[k], f[k]
        ratio = en / rn if en is not None and rn is not None and rn > 0 else None
        out[k] = {"edge_noise": en, "ref_noise": rn, "ratio": ratio}
        ok = ok and ratio is not None and ratio <= BAR
    if why:
        out["unmeasurable"] = "; ".join(why)
    out["pass"] = ok
    return out


def fmt(v, spec):
    return "n/a" if v is None else format(v, spec)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("stacks", nargs="+", metavar="stack")
    p.add_argument("--noise", action="store_true", help="split-half chroma noise of two half-stacks")
    p.add_argument("--rows", type=span, default=(320, 760))
    p.add_argument("--edge", type=span, default=(0, 6))
    p.add_argument("--ref", type=span, default=(20, 40))
    p.add_argument("--side", choices=("left", "right", "top", "bottom"), default="left")
    p.add_argument("--json", action="store_true")
    a = p.parse_args(argv)
    if len(a.stacks) != (2 if a.noise else 1):
        p.error("--noise takes two half-stacks" if a.noise else "one stack (two half-stacks need --noise)")
    try:
        if a.noise:
            r = measure_noise(read_fits(a.stacks[0]), read_fits(a.stacks[1]), a.side, a.rows, a.edge, a.ref)
        else:
            r = measure(read_fits(a.stacks[0]), a.side, a.rows, a.edge, a.ref)
    except (OSError, ValueError, np.linalg.LinAlgError) as e:
        print("edge_chroma: %s" % e, file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps(r))
    elif a.noise:
        o = r["offset"]
        print("split-half chroma noise, %s edge, along %s of the overlap (half 1 at half 0's (%d, %d), residual "
              "%s / %s px, G match %s): edge band %s (%d px) vs reference band %s (%d px)"
              % (r["side"], r["rows"], o["x"], o["y"], fmt(o["residual_x"], ".2f"), fmt(o["residual_y"], ".2f"),
                 fmt(o["match"], ".3f"), r["edge"], r["edge_pixels"], r["ref"], r["ref_pixels"]))
        for k in ("B/G", "R/G"):
            q = r[k]
            print("  %s noise: edge %s  ref %s  ratio %s" % (k, fmt(q["edge_noise"], ".5f"), fmt(q["ref_noise"], ".5f"),
                                                            fmt(q["ratio"], ".2f")))
        if "unmeasurable" in r:
            print("  unmeasurable: %s" % r["unmeasurable"])
        print("%s (bar: both ratios <= %g)" % ("PASS" if r["pass"] else "FAIL", BAR))
    else:
        print("%s edge, along %s: edge band %s (%d disk px) vs reference band %s (%d disk px)"
              % (r["side"], r["rows"], r["edge"], r["edge_pixels"], r["ref"], r["ref_pixels"]))
        for k in ("B/G", "R/G"):
            q = r[k]
            print("  %s std: edge %s  ref %s  ratio %s" % (k, fmt(q["edge_std"], ".4f"), fmt(q["ref_std"], ".4f"),
                                                          fmt(q["ratio"], ".2f")))
        if "unmeasurable" in r:
            print("  unmeasurable: %s" % r["unmeasurable"])
        print("%s (bar: both ratios <= %g)" % ("PASS" if r["pass"] else "FAIL", BAR))
    return 0 if r["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
