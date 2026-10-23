#!/usr/bin/env python3
"""H-alpha site check on a raw ASIAIR CFA FITS of an emission nebula (test-only, host-only, one-off
evidence for the spec amendment 2026-10-23; not part of any gate). Needs numpy and scipy (scipy is
not importable under `python3 -I` on this host, so run it plainly).

    python3 halpha_site_check.py <raw.fit> [top_fraction]

H-alpha (656 nm) is one line: the red CFA filter passes it and the blue one barely does, so in a
nebula's excess over the sky the red site is several times the blue one, and in-camera white
balance gains (which favour blue on these cameras) cannot reverse that. The file is read in FILE
row order (no flip). Each 2x2 site is median-filtered (25 px, stars out); the sky is a coarse
median of that (every 8th pixel, 40-wide); the nebula is the top `top_fraction` (default 0.01) of
the summed excess. Prints each site's mean excess there, (0,0)/(1,1) and (0,1)/(1,0), and the
same ratios for the brightest unsaturated stars.

Measured 2026-10-23 (M16, a compact nebula with dark sky around it):
  10_9/Autorun/Light/M16/lights/Light_M16_300.0s_Bin1_0001.fit (ASI071MC Pro): (0,0)/(1,1) 2.58,
      greens 0.995; stars 1.17 (only 5 star pixels pass the cut on this frame)
  4_10_2023/M16/Light_M16_300.0s_Bin1_20230410-050720_0001.fit (ASI2400MC Pro): 2.59, greens
      1.019; stars 1.24
A frame-filling nebula (IC1805 at 861 mm) has no dark sky for the sky model and is not a valid
input: there the "excess" is vignetting and gradient, not the nebula.
"""
import sys

import numpy as np
from scipy.ndimage import median_filter


def read_fits(path):
    cards = {}
    with open(path, "rb") as f:
        done = False
        while not done:
            block = f.read(2880)
            if len(block) < 2880:
                raise SystemExit("%s: short FITS header" % path)
            for i in range(0, 2880, 80):
                c = block[i:i + 80].decode("ascii", "replace")
                if c[:8].strip() == "END":
                    done = True
                    break
                if c[8:10] == "= ":
                    v = c[10:]
                    cards[c[:8].strip()] = v.split("'")[1].strip() if v.strip().startswith("'") else v.split("/")[0].strip()
        if int(cards["BITPIX"]) != 16:
            raise SystemExit("%s: BITPIX %s, expected 16" % (path, cards["BITPIX"]))
        w, h = int(cards["NAXIS1"]), int(cards["NAXIS2"])
        a = np.frombuffer(f.read(w * h * 2), ">i2").reshape(h, w).astype(np.float64)
    return cards, a * float(cards.get("BSCALE", 1)) + float(cards.get("BZERO", 0))


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    cards, a = read_fits(sys.argv[1])
    top = float(sys.argv[2]) if len(sys.argv) == 3 else 0.01
    sites = ((0, 0), (0, 1), (1, 0), (1, 1))
    s = {k: a[k[0]::2, k[1]::2] for k in sites}
    sm = {k: median_filter(v, size=25) for k, v in s.items()}
    sky = {k: median_filter(v[::8, ::8], size=40).repeat(8, 0).repeat(8, 1)[:v.shape[0], :v.shape[1]]
           for k, v in sm.items()}
    ex = {k: sm[k] - sky[k] for k in sites}
    tot = sum(ex.values())
    sel = tot > np.quantile(tot, 1 - top)
    e = {k: float(ex[k][sel].mean()) for k in sites}
    print("%s  INSTRUME %s  BAYERPAT %s  nebula pixels %d" % (sys.argv[1], cards.get("INSTRUME"),
                                                             cards.get("BAYERPAT"), int(sel.sum())))
    print("  nebula excess per site: " + "  ".join("%s %.1f" % (k, e[k]) for k in sites))
    print("  (0,0)/(1,1) = %.2f   (0,1)/(1,0) = %.3f" % (e[(0, 0)] / e[(1, 1)], e[(0, 1)] / e[(1, 0)]))
    q = sum(s[k] - sm[k] for k in sites)
    peak = max(v.max() for v in s.values())
    stars = (q > np.quantile(q, 0.9995)) & (np.maximum.reduce([s[k] for k in sites]) < 0.8 * peak)
    st = {k: float((s[k] - sm[k])[stars].sum()) for k in sites}
    print("  stars (%d px): (0,0)/(1,1) = %.2f   (0,1)/(1,0) = %.3f"
          % (int(stars.sum()), st[(0, 0)] / st[(1, 1)], st[(0, 1)] / st[(1, 0)]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
