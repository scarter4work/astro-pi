#!/usr/bin/env python3
"""Edge chroma check on a stack-probe colour stack (Ruling R8): is the chroma noisier at a frame edge?

    edge_chroma.py <stack.fits> [--rows 320:760] [--edge 0:6] [--ref 20:40] [--side left|right|top|bottom] [--json]

Input: `stack-probe --out x.fits` (32-bit float, NAXIS3 = 3 planes R, G, B, first row = top). Per pixel
it forms B/G and R/G over the disk (G >= 0.05; sky is left out), then takes each ratio's std over the
edge band and over the reference band, and reports edge std / reference std.

  --side   the frame edge measured (default left).
  --rows   the span ALONG that edge, in image pixels: rows for left/right, columns for top/bottom
           (default 320:760, where the 2026-10-23 64.6F Moon touches the left edge; its disk also covers
           columns 320-760 of the top and bottom rows).
  --edge   the edge band, as distances from that edge in pixels (default 0:6, i.e. the 6 outermost).
  --ref    the reference band, the same way (default 20:40).

Ranges are half-open A:B. Exit 0 when both ratios are <= 1.5, 1 otherwise -- including whenever a band
cannot be measured (fewer than 100 disk pixels, or a non-finite sample in it): the check fails closed.
Exit 2 for a bad argument, an unreadable file or a stack that is not 3 float planes.

Limit (Ruling R8): the std of B/G and R/G is colour structure, the scene's included, not noise alone. On
the 64.6F Moon the edge band's 3.8x (B/G) is reproducible colour (two independent half-stacks agree, corr
0.97-1.00; the split-half noise ratio is 1.54x), and the clean synthetic scene's own truth reads 0.27x.
A ratio here is evidence about the stack only where the scene's colour is known to be uniform.
"""
import argparse
import json
import sys

import numpy as np

BAR = 1.5        # edge std / reference std above this is a defect
DISK_G = 0.05    # pixels with G below this are sky, not measured
MIN_PIXELS = 100  # a band with fewer disk pixels is unmeasurable


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
        w, h = int(cards["NAXIS1"]), int(cards["NAXIS2"])
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


def fmt(v, spec):
    return "n/a" if v is None else format(v, spec)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("stack")
    p.add_argument("--rows", type=span, default=(320, 760))
    p.add_argument("--edge", type=span, default=(0, 6))
    p.add_argument("--ref", type=span, default=(20, 40))
    p.add_argument("--side", choices=("left", "right", "top", "bottom"), default="left")
    p.add_argument("--json", action="store_true")
    a = p.parse_args(argv)
    try:
        r = measure(read_fits(a.stack), a.side, a.rows, a.edge, a.ref)
    except (OSError, ValueError) as e:
        print("edge_chroma: %s" % e, file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps(r))
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
