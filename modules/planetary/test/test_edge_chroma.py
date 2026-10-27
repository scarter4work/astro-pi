#!/usr/bin/env python3
"""ctest case (NAS-free): edge_chroma.py measures edge chroma noise and fails closed.

Synthetic 3-plane FITS stacks in stack-probe's layout (32-bit float, NAXIS3 = 3, first row = top):
a grey disk that touches the left frame edge, over dark sky, with per-pixel chroma noise (R/G and
B/G) of a chosen std in the edge columns and in the rest. The tool must exit 1 on a noisy edge
(and report the ratio), 0 on a clean one, and 1 whenever it cannot measure. numpy is the one
dependency.

    python3 -I test_edge_chroma.py
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

try:
    import numpy as np
except ImportError:  # e.g. the Rocky 9 build container: ctest reports the case as Skipped (77)
    print("SKIP: numpy is not importable by this python3; the edge chroma check was not run")
    sys.exit(77)

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "edge_chroma.py")
H, W = 240, 160
SPAN = "40:200"  # rows where the disk covers the left edge (disk rows 28-212 at x = 0)


def write_fits(path, planes):
    """planes: (3, H, W) float32, row 0 = top -- the layout of test/ProbeFits.h."""
    c, h, w = planes.shape
    cards = [("SIMPLE", "T"), ("BITPIX", "-32"), ("NAXIS", "3"), ("NAXIS1", str(w)), ("NAXIS2", str(h)),
             ("NAXIS3", str(c))]
    header = "".join((k.ljust(8) + "= " + v.rjust(20)).ljust(80) for k, v in cards) + "END".ljust(80)
    header = header.ljust((len(header) + 2879) // 2880 * 2880)
    data = planes.astype(">f4").tobytes()
    data += b"\0" * ((-len(data)) % 2880)
    with open(path, "wb") as f:
        f.write(header.encode("ascii"))
        f.write(data)


def stack(edge_std, rest_std, edge_cols=6, channels=(0, 2), seed=3, sky_ratio_noise=0.0):
    """A grey disk (G 0.3-0.6) touching the left edge; chroma noise per channel/G of edge_std in
    x < edge_cols and rest_std elsewhere, in the given channels (0 = R, 2 = B)."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:H, 0:W].astype(np.float64)
    disk = (x - 60) ** 2 + (y - 120) ** 2 <= 110 ** 2
    g = np.where(disk, 0.45 + 0.15 * np.sin(x / 9.0) * np.cos(y / 13.0), 0.01)
    planes = np.empty((3, H, W))
    planes[1] = g
    for ch in (0, 2):
        std = np.where(x < edge_cols, edge_std, rest_std) if ch in channels else rest_std
        ratio = 1.0 + std * rng.standard_normal((H, W))
        # Sky (G < 0.05) is outside the measurement: give it wild colour to prove it is masked out.
        ratio = np.where(disk, ratio, 1.0 + sky_ratio_noise * rng.standard_normal((H, W)))
        planes[ch] = g * ratio
    return planes.astype(np.float32)


class EdgeChroma(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, planes, *args):
        path = os.path.join(self.tmp.name, "s.fits")
        write_fits(path, planes)
        p = subprocess.run([sys.executable, "-I", TOOL, path, "--rows", SPAN, *args],
                           capture_output=True, text=True)
        return p.returncode, p.stdout, p.stderr

    def run_json(self, planes, *args):
        code, out, err = self.run_tool(planes, "--json", *args)
        self.assertEqual(err, "", err)
        return code, json.loads(out)

    def test_noisy_left_edge_fails_with_its_ratio(self):
        code, r = self.run_json(stack(0.03, 0.008))
        self.assertEqual(code, 1)
        for k in ("B/G", "R/G"):
            self.assertGreater(r[k]["ratio"], 3.0, r)
            self.assertAlmostEqual(r[k]["edge_std"], 0.03, delta=0.004)
            self.assertAlmostEqual(r[k]["ref_std"], 0.008, delta=0.001)
        self.assertFalse(r["pass"])

    def test_clean_left_edge_passes(self):
        code, r = self.run_json(stack(0.008, 0.008))
        self.assertEqual(code, 0, r)
        self.assertTrue(r["pass"])
        for k in ("B/G", "R/G"):
            self.assertLess(r[k]["ratio"], 1.5)

    def test_plain_output_names_both_ratios(self):
        code, out, err = self.run_tool(stack(0.03, 0.008))
        self.assertEqual(code, 1)
        self.assertEqual(err, "")
        for k in ("B/G", "R/G"):
            self.assertIn(k, out)
        self.assertIn("FAIL", out)

    def test_one_noisy_ratio_is_enough_to_fail(self):
        for ch, name in ((2, "B/G"), (0, "R/G")):
            with self.subTest(noisy=name):
                code, r = self.run_json(stack(0.03, 0.008, channels=(ch,)))
                self.assertEqual(code, 1)
                self.assertGreater(r[name]["ratio"], 3.0)

    def test_sky_is_masked_out(self):
        # Wild colour on the sky (G = 0.01) in every column, disk clean: still a pass.
        code, r = self.run_json(stack(0.008, 0.008, sky_ratio_noise=5.0))
        self.assertEqual(code, 0, r)

    def test_bands_follow_the_side(self):
        noisy = stack(0.03, 0.008)
        # Right: the same stack mirrored left-right.
        self.assertEqual(self.run_json(np.ascontiguousarray(noisy[:, :, ::-1]), "--side", "right")[0], 1)
        # The mirrored disk still reaches the (now clean) left edge, rows 72-168.
        self.assertEqual(self.run_json(np.ascontiguousarray(noisy[:, :, ::-1]), "--side", "left")[0], 0)
        # Top / bottom: transposed so the noisy columns become the first (top) rows, then flipped.
        top = np.ascontiguousarray(noisy.transpose(0, 2, 1))
        self.assertEqual(self.run_json(top, "--side", "top")[0], 1)
        self.assertEqual(self.run_json(np.ascontiguousarray(top[:, ::-1, :]), "--side", "bottom")[0], 1)
        clean_top = np.ascontiguousarray(stack(0.008, 0.008).transpose(0, 2, 1))
        self.assertEqual(self.run_json(clean_top, "--side", "top")[0], 0)

    def test_custom_bands(self):
        # Noise in x 0-11 only: a 0:6 edge vs a 6:12 reference sees none of it, 0:6 vs 20:40 does.
        planes = stack(0.03, 0.008, edge_cols=12)
        self.assertEqual(self.run_json(planes, "--edge", "0:6", "--ref", "6:12")[0], 0)
        self.assertEqual(self.run_json(planes, "--edge", "0:6", "--ref", "20:40")[0], 1)

    def test_fails_closed_when_it_cannot_measure(self):
        planes = stack(0.008, 0.008)
        dark = planes.copy()
        dark[:, :, :6] = 0.01  # edge band is all sky: no disk pixels to measure
        code, r = self.run_json(dark)
        self.assertEqual(code, 1)
        self.assertFalse(r["pass"])
        nan = planes.copy()
        nan[2, 100, 2] = np.nan
        self.assertEqual(self.run_json(nan)[0], 1)
        # Not a 3-plane image: refused, never a pass.
        path = os.path.join(self.tmp.name, "mono.fits")
        write_fits(path, planes[1:2])
        p = subprocess.run([sys.executable, "-I", TOOL, path], capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        p = subprocess.run([sys.executable, "-I", TOOL, os.path.join(self.tmp.name, "missing.fits")],
                           capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        # A band outside the image is refused.
        self.assertNotEqual(self.run_tool(planes, "--ref", "150:170")[0], 0)


if __name__ == "__main__":
    unittest.main()
