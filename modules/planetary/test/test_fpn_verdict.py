#!/usr/bin/env python3
"""ctest case (NAS-free): avi_real_check.py's fixed-pattern readout check (4b) fails closed.

A placement is resolved only when identity at (0,0) was scored, the three flips at (0,0) were
scored, and identity leads every scored alternative by FPN_RATIO. Anything short of that scoring
is "unscored", and an unscored capture fails even when it is pinned in FPN_UNRESOLVED. Synthetic
data only; numpy is the one dependency.

    python3 -I test_fpn_verdict.py
"""
import os
import sys
import unittest

try:
    import numpy as np
except ImportError:  # e.g. the Rocky 9 build container: ctest reports the case as Skipped (77)
    print("SKIP: numpy is not importable by this python3; the fail-closed check was not run")
    sys.exit(77)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import avi_real_check as c  # noqa: E402

FLIPS = {("vflip", 0, 0), ("hflip", 0, 0), ("rot180", 0, 0)}


class FpnVerdict(unittest.TestCase):
    def test_nothing_scored_is_unscored_not_resolved(self):
        # The fail-open case the guard exists for: identity alone, no competitor.
        self.assertEqual(c.fpn_verdict(0.1, (0.0, "", 0, 0), set(), "resolved"), "unscored")

    def test_each_missing_flip_is_unscored(self):
        for missing in FLIPS:
            with self.subTest(missing=missing):
                self.assertEqual(c.fpn_verdict(0.1, (0.001, "vflip", 0, 0), FLIPS - {missing}, "resolved"),
                                 "unscored")

    def test_identity_not_scored_is_unscored(self):
        self.assertEqual(c.fpn_verdict(float("nan"), (0.001, "vflip", 0, 0), set(FLIPS), "resolved"), "unscored")

    def test_scored_lead_resolves_and_weak_lead_does_not(self):
        self.assertEqual(c.fpn_verdict(0.1, (0.02, "vflip", 0, 0), set(FLIPS), "resolved"), "resolved")
        self.assertEqual(c.fpn_verdict(0.1, (-0.04, "hflip", 1, 0), set(FLIPS), "resolved"), "unresolved")
        self.assertEqual(c.fpn_verdict(0.0, (0.0, "vflip", 0, 0), set(FLIPS), "resolved"), "unresolved")


class FpnScore(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(7)
        self.ref = rng.normal(size=(200, 300)).astype(np.float32)
        self.ref_m = np.ones(self.ref.shape, bool)
        self.crop = self.ref[50:150, 60:180].copy()
        self.full = np.ones(self.crop.shape, bool)

    def verdict(self, r, m):
        ident, other, _, scored = c.fpn_score(r, m, self.ref, self.ref_m, 50, 60)
        return c.fpn_verdict(ident, other, scored, "resolved")

    def test_exact_crop_resolves(self):
        self.assertEqual(self.verdict(self.crop, self.full), "resolved")

    def test_flipped_crop_does_not_resolve(self):
        self.assertEqual(self.verdict(self.crop[::-1].copy(), self.full), "unresolved")
        self.assertEqual(self.verdict(self.crop[::-1, ::-1].copy(), self.full), "unresolved")

    def test_mask_too_small_to_score_is_unscored(self):
        m = np.zeros(self.crop.shape, bool)
        m[:20, :20] = True  # 400 px: under the 1000 px a placement needs
        self.assertEqual(self.verdict(self.crop, m), "unscored")


class FpnCheckFailsClosed(unittest.TestCase):
    """fpn_check must fail an unscored capture, pinned or not (no NAS: I/O is stubbed)."""

    def setUp(self):
        self.saved = {k: getattr(c, k) for k in ("FPN_REFERENCES", "FPN_UNRESOLVED", "read_fits", "ff_mean",
                                                 "fpn_score")}
        c.failures.clear()
        c.FPN_REFERENCES = {"ZWO Test": ["ref.fit"]}
        c.read_fits = lambda p: ({"INSTRUME": "ZWO Test", "BAYERPAT": "RGGB"},
                                 np.random.default_rng(1).normal(size=(64, 64)).astype(np.float32))
        c.ff_mean = lambda path, n, w, h: (np.random.default_rng(2).normal(size=(h, w)).astype(np.float32), n)
        # Every placement below the overlap floor: nothing but identity is scored.
        c.fpn_score = lambda r, m, ref, ref_m, y0, x0: (0.2, (0.0, "", 0, 0), 5000, set())

    def tearDown(self):
        for k, v in self.saved.items():
            setattr(c, k, v)
        c.failures.clear()

    def run_check(self, pinned):
        c.FPN_UNRESOLVED = {"2099-01-01-000000-Test": "pinned for the test"} if pinned else {}
        sc = {"StartX": "8", "StartY": "8", "Flip": "Vert(unimplemented)"}
        j = {"width": 32, "height": 32, "sidecarCamera": "ZWO Test"}
        c.fpn_check([("/nowhere/2099-01-01-000000-Test-Bin1.avi", j, sc)])
        return [f for f in c.failures if "unscored" in f]

    def test_unscored_capture_fails(self):
        self.assertEqual(len(self.run_check(pinned=False)), 1)

    def test_unscored_capture_fails_even_when_pinned(self):
        self.assertEqual(len(self.run_check(pinned=True)), 1)


if __name__ == "__main__":
    unittest.main(verbosity=1)
