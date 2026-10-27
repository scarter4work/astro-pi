#!/usr/bin/env python3
"""ctest case (NAS-free): edge_chroma.py measures edge chroma noise and fails closed.

Synthetic 3-plane FITS stacks in stack-probe's layout (32-bit float, NAXIS3 = 3, first row = top):
a grey disk that touches the left frame edge, over dark sky, with per-pixel chroma noise (R/G and
B/G) of a chosen std in the edge columns and in the rest. The tool must exit 1 on a noisy edge
(and report the ratio), 0 on a clean one, and 1 whenever it cannot measure. numpy is the one
dependency.

The split-half mode (--noise HALF0 HALF1, Ruling R12) gets two half-stacks of one analytic scene --
real colour structure common to both, independent chroma noise in each, the second half on a grid
offset by whole pixels (each half-stack is cropped on its own) -- and must judge the noise alone:
pass on a colourful but quiet edge, fail on a noisy one, fail closed when the halves cannot be
registered to a whole pixel.

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


# Split-half pairs. Half 1's pixel (y, x) shows the scene at half 0's (y + OFF_Y, x + OFF_X) (+ sub): the two
# half-stacks of a capture are each cropped on their own and carry their own reference grid.
OFF_Y, OFF_X = 2, 3


def scene(wy, wx):
    """The analytic scene at world coordinates (floats): (disk mask, G, texture phase)."""
    disk = (wx - 60) ** 2 + (wy - 120) ** 2 <= 110 ** 2
    tex = sum(a * np.sin(fx * wx + fy * wy + ph) for a, fx, fy, ph in
              ((0.05, 0.31, 0.17, 0.4), (0.04, -0.23, 0.41, 1.3), (0.03, 0.57, -0.29, 2.2), (0.03, 0.11, 0.07, 0.9)))
    g = np.where(disk, 0.45 + 0.15 * np.sin(wx / 9.0) * np.cos(wy / 13.0) + tex, 0.01)
    return disk, g


def half(which, edge_std, rest_std, colour=0.0, channels=(0, 2), sub=(0.0, 0.0), seed=11):
    """One half-stack (which 0 or 1) of the split. Colour: real colour structure of this amplitude in the
    scene's edge region (common to both halves, so it is not noise). Chroma noise of edge_std in the 6 px
    of the overlap's left edge, rest_std elsewhere, independent per half."""
    h, w = (H, W) if which == 0 else (H - 3, W - 5)
    oy, ox = (0.0, 0.0) if which == 0 else (OFF_Y + sub[0], OFF_X + sub[1])
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    wy, wx = y + oy, x + ox
    disk, g = scene(wy, wx)
    near = wx < OFF_X + 6  # the overlap's left edge is at world x OFF_X; the band is its 6 outermost px
    rng = np.random.default_rng(seed + which)
    planes = np.empty((3, h, w))
    planes[1] = g
    for k, ch in enumerate((0, 2)):
        std = np.where(near, edge_std, rest_std) if ch in channels else rest_std
        col = colour * np.where(wx < OFF_X + 12, np.sin(wx / 1.7 + wy / 2.3 + k), 0.0)
        ratio = np.where(disk, 1.0 + col + std * rng.standard_normal((h, w)), 1.0)
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

    def test_near_the_bar(self):
        # Edge std 1.4x the reference's passes, 1.6x fails (review M2).
        for edge, want, code in ((0.0112, 1.4, 0), (0.0128, 1.6, 1)):
            with self.subTest(ratio=want):
                c, r = self.run_json(stack(edge, 0.008))
                self.assertEqual(c, code, r)
                for k in ("B/G", "R/G"):
                    self.assertAlmostEqual(r[k]["ratio"], want, delta=0.07)

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
        # Ruling R16: too little disk is NOT JUDGED (3, with the reason) -- not a pass, and not a fail either.
        code, r = self.run_json(dark)
        self.assertEqual(code, 3, r)
        self.assertFalse(r["pass"])
        self.assertEqual(r["verdict"], "not judged")
        self.assertIn("0 disk pixels", r["unmeasurable"])
        code, out, _ = self.run_tool(dark)
        self.assertEqual(code, 3)
        self.assertIn("NOT JUDGED", out)
        # A non-finite sample is a defect: a fail (1), even beside a band that is only sparse.
        nan = planes.copy()
        nan[2, 100, 2] = np.nan
        self.assertEqual(self.run_json(nan)[0], 1)
        nan[:, :, 20:40] = 0.01
        self.assertEqual(self.run_json(nan)[0], 1)
        # Bad input is refused with 2 (never 1, the "defect found" verdict, and never a pass).
        def refused(path, *args):
            p = subprocess.run([sys.executable, "-I", TOOL, path, *args], capture_output=True, text=True)
            self.assertEqual(p.returncode, 2, (path, args, p.stdout, p.stderr))
            self.assertTrue(p.stderr.startswith("edge_chroma: "), p.stderr)
        mono = os.path.join(self.tmp.name, "mono.fits")
        write_fits(mono, planes[1:2])
        refused(mono)
        refused(os.path.join(self.tmp.name, "missing.fits"))
        # A header without NAXIS1 / NAXIS2.
        full = os.path.join(self.tmp.name, "full.fits")
        write_fits(full, planes)
        with open(full, "rb") as f:
            good = f.read()
        for key in (b"NAXIS1  ", b"NAXIS2  "):
            i = good.index(key)
            bad = os.path.join(self.tmp.name, "no_%s.fits" % key.strip().decode())
            with open(bad, "wb") as f:
                f.write(good[:i] + b"COMMENT".ljust(80) + good[i + 80:])
            refused(bad)
        # A band outside the image.
        refused(full, "--ref", "150:170")


class SplitHalfNoise(unittest.TestCase):
    """--noise HALF0 HALF1: std((ratio0 - ratio1) / sqrt 2) per band, edge vs reference (Ruling R12)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, h0, h1, *args):
        p0, p1 = os.path.join(self.tmp.name, "h0.fits"), os.path.join(self.tmp.name, "h1.fits")
        write_fits(p0, h0)
        write_fits(p1, h1)
        p = subprocess.run([sys.executable, "-I", TOOL, "--noise", p0, p1, "--rows", SPAN, *args],
                           capture_output=True, text=True)
        return p.returncode, p.stdout, p.stderr

    def run_json(self, h0, h1, *args):
        code, out, err = self.run_tool(h0, h1, "--json", *args)
        self.assertEqual(err, "", err)
        return code, json.loads(out)

    def pair(self, *a, **k):
        return half(0, *a, **k), half(1, *a, **k)

    def test_colourful_quiet_edge_passes_where_the_raw_ratio_fails(self):
        h0, h1 = self.pair(0.008, 0.008, colour=0.05)
        code, r = self.run_json(h0, h1)
        self.assertEqual(code, 0, r)
        self.assertTrue(r["pass"])
        self.assertEqual(r["mode"], "noise")
        for k in ("B/G", "R/G"):
            self.assertLess(r[k]["ratio"], 1.5, r)
            self.assertAlmostEqual(r[k]["edge_noise"], 0.008, delta=0.001)
            self.assertAlmostEqual(r[k]["ref_noise"], 0.008, delta=0.001)
        # The same half-stack's colour fails the raw ratio: it is colour, which --noise must not count.
        path = os.path.join(self.tmp.name, "raw.fits")
        write_fits(path, h0[:, OFF_Y:, OFF_X:])
        p = subprocess.run([sys.executable, "-I", TOOL, path, "--rows", SPAN], capture_output=True, text=True)
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)

    def test_noisy_edge_fails_with_its_ratio_and_the_registration(self):
        code, r = self.run_json(*self.pair(0.03, 0.008))
        self.assertEqual(code, 1, r)
        self.assertFalse(r["pass"])
        for k in ("B/G", "R/G"):
            self.assertGreater(r[k]["ratio"], 3.0, r)
            self.assertAlmostEqual(r[k]["edge_noise"], 0.03, delta=0.004)
            self.assertAlmostEqual(r[k]["ref_noise"], 0.008, delta=0.001)
        # Half 1's pixel (0, 0) is half 0's (OFF_X, OFF_Y); the bands sit on the overlap.
        self.assertEqual((r["offset"]["x"], r["offset"]["y"]), (OFF_X, OFF_Y), r)
        self.assertLess(abs(r["offset"]["residual_x"]), 0.05, r)
        self.assertLess(abs(r["offset"]["residual_y"]), 0.05, r)
        self.assertEqual((r["overlap"]["width"], r["overlap"]["height"]), (W - 5, H - 3), r)

    def test_halves_in_either_order(self):
        h0, h1 = self.pair(0.03, 0.008)
        code, r = self.run_json(h1, h0)
        self.assertEqual(code, 1, r)
        self.assertEqual((r["offset"]["x"], r["offset"]["y"]), (-OFF_X, -OFF_Y), r)
        self.assertGreater(r["B/G"]["ratio"], 3.0, r)

    def test_one_noisy_ratio_is_enough_to_fail(self):
        for ch, name, other in ((2, "B/G", "R/G"), (0, "R/G", "B/G")):
            with self.subTest(noisy=name):
                code, r = self.run_json(*self.pair(0.03, 0.008, channels=(ch,)))
                self.assertEqual(code, 1, r)
                self.assertGreater(r[name]["ratio"], 3.0, r)
                self.assertLess(r[other]["ratio"], 1.5, r)

    def test_near_the_bar(self):
        # Edge noise 1.4x the interior's passes, 1.6x fails (review M2): the bar is the bar, not a 3x detector.
        for edge, want, code in ((0.0112, 1.4, 0), (0.0128, 1.6, 1)):
            with self.subTest(ratio=want):
                c, r = self.run_json(*self.pair(edge, 0.008))
                self.assertEqual(c, code, r)
                for k in ("B/G", "R/G"):
                    self.assertAlmostEqual(r[k]["ratio"], want, delta=0.07)

    def test_what_lies_outside_the_overlap_is_not_judged(self):
        # The mode's blind spot, pinned so it is not mistaken for coverage (review M2; module docstring): a full stack
        # reaches as far as the farther-reaching half, the overlap only as far as the nearer one. Chroma noise 5x the
        # interior's in half 0's own outer columns that half 1 does not reach (world x 0-2) is not seen: a pass. The
        # engine's border error there is A2's still-capture test's (test_engine, synthetic, truth known).
        h0, h1 = self.pair(0.008, 0.008)
        rng = np.random.default_rng(9)
        for ch in (0, 2):
            h0[ch, :, :OFF_X] *= (1 + 0.04 * rng.standard_normal((H, OFF_X))).astype(np.float32)
        code, r = self.run_json(h0, h1)
        self.assertEqual(code, 0, r)
        self.assertEqual(r["overlap"]["x"], OFF_X)

    def test_bands_follow_the_side(self):
        # Transposed, the noisy left edge is the top one; the disk's own left (the old top) is all sky.
        t = lambda p: np.ascontiguousarray(p.transpose(0, 2, 1))
        h0, h1 = self.pair(0.03, 0.008)
        code, r = self.run_json(t(h0), t(h1), "--side", "top")
        self.assertEqual(code, 1, r)
        self.assertGreater(r["B/G"]["ratio"], 3.0, r)
        self.assertEqual((r["offset"]["x"], r["offset"]["y"]), (OFF_Y, OFF_X), r)
        c0, c1 = self.pair(0.008, 0.008)
        self.assertEqual(self.run_json(t(c0), t(c1), "--side", "top")[0], 0)
        # Mirrored left-right: the noisy edge is the right one.
        m = lambda p: np.ascontiguousarray(p[:, :, ::-1])
        self.assertEqual(self.run_json(m(h0), m(h1), "--side", "right")[0], 1)

    def test_plain_output(self):
        code, out, err = self.run_tool(*self.pair(0.03, 0.008))
        self.assertEqual(code, 1)
        self.assertEqual(err, "")
        self.assertIn("split-half", out)
        for k in ("B/G", "R/G"):
            self.assertIn(k, out)
        self.assertIn("FAIL", out)

    def test_subpixel_residual_is_registered_out(self):
        # Each half's reference sits at its own sharpest frames' mean position: the halves differ by a fraction of
        # a pixel (0.60 px on the 70.0F). Left in, colour structure leaks into the difference; registered out, the
        # colourful but quiet edge passes and a noisy one still fails.
        for sub in ((0.1, -0.1), (0.4, 0.0), (-0.3, 0.45)):
            with self.subTest(sub=sub):
                code, r = self.run_json(*self.pair(0.008, 0.008, colour=0.05, sub=sub))
                self.assertEqual(code, 0, r)
                o = r["offset"]
                true_y, true_x = OFF_Y + sub[0], OFF_X + sub[1]
                self.assertAlmostEqual(o["y"] + o["residual_y"], true_y, delta=0.02)
                self.assertAlmostEqual(o["x"] + o["residual_x"], true_x, delta=0.02)
                self.assertGreater(o["match"], 0.99)
                for k in ("B/G", "R/G"):
                    self.assertLess(r[k]["ratio"], 1.5, r)
                self.assertEqual(self.run_json(*self.pair(0.03, 0.008, colour=0.05, sub=sub))[0], 1)

    def test_halves_of_different_scenes_fail_closed(self):
        h0, _ = self.pair(0.008, 0.008)
        rng = np.random.default_rng(5)
        other = h0.copy()
        other[:, 30:220, 0:140] = rng.uniform(0.1, 0.6, (1, 190, 140)) * np.ones((3, 1, 1))
        code, r = self.run_json(h0, np.ascontiguousarray(other))
        self.assertEqual(code, 1, r)
        self.assertFalse(r["pass"])
        self.assertIsNone(r["B/G"]["ratio"], r)
        self.assertIn("unmeasurable", r)
        # The same scene under G noise far above the scene's own structure registers, but does not match.
        g = h0[1] + rng.normal(0.0, 0.3, h0[1].shape).astype(np.float32) * (h0[1] >= 0.05)
        code, r = self.run_json(h0, np.ascontiguousarray(np.stack([h0[0], g, h0[2]])))
        self.assertEqual(code, 1, r)
        self.assertIn("not one scene", r["unmeasurable"])

    def test_fails_closed_and_refuses_bad_input(self):
        h0, h1 = self.pair(0.008, 0.008)
        dark0, dark1 = h0.copy(), h1.copy()
        dark0[:, :, :OFF_X + 6] = 0.01  # the overlap's edge band is all sky: not judged (R16), as the 70.0F's left edge
        code, r = self.run_json(dark0, dark1)
        self.assertEqual(code, 3, r)
        self.assertEqual(r["verdict"], "not judged")
        self.assertIn("disk (G >= 0.05) in both halves", r["unmeasurable"])
        # Halves that share too little disk to register at all: not judged either.
        sky0, sky1 = h0.copy(), h1.copy()
        sky0[:, :, :] = 0.01
        sky0[:, 100:105, 60:65] = 0.5
        sky1[:, :, :] = 0.01
        sky1[:, 98:103, 57:62] = 0.5
        code, r = self.run_json(sky0, sky1)
        self.assertEqual(code, 3, r)
        self.assertIn("share", r["unmeasurable"])
        nan1 = h1.copy()
        nan1[0, 100, 1] = np.nan
        self.assertEqual(self.run_json(h0, nan1)[0], 1)
        p0, p1 = os.path.join(self.tmp.name, "a.fits"), os.path.join(self.tmp.name, "mono.fits")
        write_fits(p0, h0)
        write_fits(p1, h1[1:2])

        def refused(*args):
            p = subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True)
            self.assertEqual(p.returncode, 2, (args, p.stdout, p.stderr))
        refused("--noise", p0, p1)        # a mono half
        refused("--noise", p0)            # one half
        refused(p0, p0)                   # two stacks without --noise
        refused("--noise", p0, os.path.join(self.tmp.name, "missing.fits"))


if __name__ == "__main__":
    unittest.main()
