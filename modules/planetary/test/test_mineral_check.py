#!/usr/bin/env python3
"""ctest case (NAS-free): mineral_check.py grades R31 and R12 by their rulings, and fails closed.

In-process, on mineral_check's own functions with their inputs stubbed (the measurements themselves are
edge_chroma's and the bench's, tested on their own):

  R31 gain   Ruling R38 (mineral ledger, 2026-10-27-planetary-mineral-moon/progress.md): R31 is informational --
             noise gain over detail gain is reported, never a FAIL; a NaN measure still fails.
  R12 edge   Ruling R39: 0528a's bottom row (R/G 2.1 at the ruling; B/G under its 1.5 bar) is accepted; the same on
             another capture, another side of 0528a, or past the excused level fails.

mineral_check needs numpy, scipy and Pillow; without them this exits 77 (ctest: Skipped). Under -I the user site is
not on sys.path, so it is added explicitly (scipy lives there on this box); never the cwd or this directory.

    python3 -I test_mineral_check.py
"""
import os
import site
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
try:
    import numpy as np
    import scipy  # noqa: F401
    import PIL  # noqa: F401
except ImportError:
    sys.path.append(site.getusersitepackages())
    try:
        import numpy as np
        import scipy  # noqa: F401
        import PIL  # noqa: F401
    except ImportError:  # e.g. the Rocky 9 build container
        print("SKIP: numpy, scipy or Pillow is not importable by this python3; the mineral check was not tested")
        sys.exit(77)

sys.path.insert(0, HERE)
import mineral_check as mc  # noqa: E402


def half(h=120, w=100):
    return {"mineral": np.zeros((3, h, w)), "display": np.zeros((3, h, w)), "disk": np.ones((h, w), bool)}


class R12Edge(unittest.TestCase):
    """split_half_edges with disc_span and edge_chroma.measure_noise stubbed: the bottom side judged, the rest not."""

    def setUp(self):
        self.saved = (mc.disc_span, mc.edge_chroma.measure_noise)
        self.bg, self.rg, self.failing = 1.33, 2.05, {"bottom"}

        def span(inner, side):
            return (10, 90) if side in self.sides else None

        def measure(p0, p1, side, rows, edge, ref):
            bad = side in self.failing
            v = {"B/G": {"ratio": self.bg if bad else 1.0, "edge_noise": 1.0, "ref_noise": 1.0},
                 "R/G": {"ratio": self.rg if bad else 1.0, "edge_noise": 1.0, "ref_noise": 1.0}}
            v["verdict"] = "fail" if bad else "pass"
            return v
        self.sides = {"bottom"}
        mc.disc_span, mc.edge_chroma.measure_noise = span, measure

    def tearDown(self):
        mc.disc_span, mc.edge_chroma.measure_noise = self.saved

    def verdict(self, cid):
        rep = mc.Report(cid)
        mc.split_half_edges(rep, half(), half(), {"dy": 0, "dx": 0, "ry": 0.0, "rx": 0.0})
        return rep.rows[-1]["verdict"]

    def test_0528a_bottom_accepted(self):
        # The task-7 run's numbers (B/G 1.33, R/G 2.05): accepted per R39, not a FAIL, never a PASS.
        self.assertEqual(self.verdict("0528a"), "ACCEPTED")

    def test_other_capture_fails(self):
        self.assertEqual(self.verdict("0528b"), "FAIL")

    def test_other_side_fails(self):
        self.sides, self.failing = {"bottom", "top"}, {"top"}
        self.assertEqual(self.verdict("0528a"), "FAIL")
        self.failing = {"bottom", "top"}
        self.assertEqual(self.verdict("0528a"), "FAIL")

    def test_past_the_excused_level_fails(self):
        for bg, rg in ((1.33, 2.6), (1.6, 2.05), (1.33, float("nan"))):
            with self.subTest(bg=bg, rg=rg):
                self.bg, self.rg = bg, rg
                self.assertEqual(self.verdict("0528a"), "FAIL")

    def test_clean_passes(self):
        self.failing = set()
        self.assertEqual(self.verdict("0528a"), "PASS")


class R31Gain(unittest.TestCase):
    """noise_gain with registered() stubbed: textured halves whose sharpened noise grows 3x and detail 1.2x."""

    def setUp(self):
        self.saved = mc.registered
        rng = np.random.default_rng(7)
        h = w = 160
        yy, xx = np.mgrid[0:h, 0:w]
        tex = 0.4 + 0.05 * np.sin(xx / 3.0) * np.cos(yy / 4.0)
        disk = np.ones((h, w))
        n0, n1 = rng.normal(0, 0.002, (h, w)), rng.normal(0, 0.002, (h, w))
        self.halves = ([tex + n0, tex * 1.2 + 3 * n0, disk], [tex + n1, tex * 1.2 + 3 * n1, disk])
        mc.registered = lambda even, odd, reg, planes: tuple(np.stack(p) for p in self.halves)

    def tearDown(self):
        mc.registered = self.saved

    def run_gain(self):
        rep = mc.Report("646F")
        mc.noise_gain(rep, {}, {}, {"dy": 0, "dx": 0, "ry": 0.0, "rx": 0.0, "match": 1.0})
        return rep.rows[-1]

    def test_noise_over_detail_is_informational(self):
        # R38: noise riding texture takes the full gain; R37's bar is reported, not graded.
        row = self.run_gain()
        self.assertEqual(row["verdict"], "INFO", row["value"])
        self.assertIn("R38", row["value"])

    def test_nan_fails(self):
        self.halves[0][1] = self.halves[0][1].copy()
        self.halves[0][1][50:60, 50:60] = np.nan
        self.assertEqual(self.run_gain()["verdict"], "FAIL")


if __name__ == "__main__":
    unittest.main(verbosity=1)
