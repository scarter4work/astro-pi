#!/usr/bin/env python3
"""ctest case (NAS-free): planet_check.py fails closed.

A synthetic run dir in run-planet-real.sh's layout: one capture, a smooth 12 px disc in Saturn's catalogue colour.
Clean, every requirement passes or is not judged and the checker exits 0; with R and B moved 0.8 px apart in the
output (a limb fringe) it exits 1 on "4 fringe"; a missing stage file, a missing gating run or an empty run dir exit 1;
a capture with no disc (ruling Q3a) and one the stacker refused are not failures; a declined channel (ruling Q4) is
not judged only under 0.5 px and once per capture, and a NaN limb residual or Saturation 0 sample fails. numpy only
(Pillow, when present, for the PNG run).

    python3 -I test_planet_check.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

try:
    import numpy as np
except ImportError:  # e.g. the Rocky 9 build container: ctest reports the case as Skipped (77)
    print("SKIP: numpy is not importable by this python3; the fail-closed check was not run")
    sys.exit(77)

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "planet_check.py")
SHARPENS = (0, 1, 1.5, 2, 3, 4)
BLURS = (0, 0.5, 1, 1.5, 2, 2.5, 3, 4, 6)
W, H, R = 96, 80, 12.0
SATURN = (0.679, 1.238)  # B/G, R/G


def dump(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f)


def write_fits(path, planes):
    """stack-probe's FITS (ProbeFits.h): 32-bit float, NAXIS3 planes, first row = top."""
    a = np.asarray(planes, np.float32)
    if a.ndim == 2:
        a = a[None]
    n, h, w = a.shape
    cards = ["SIMPLE  =                    T", "BITPIX  =                  -32", "NAXIS   =                    %d" % (3 if n > 1 else 2),
             "NAXIS1  = %20d" % w, "NAXIS2  = %20d" % h] + (["NAXIS3  = %20d" % n] if n > 1 else []) + ["END"]
    head = "".join(c.ljust(80) for c in cards).encode("ascii")
    head += b" " * (-len(head) % 2880)
    data = a.astype(">f4").tobytes()
    with open(path, "wb") as f:
        f.write(head + data + b"\0" * (-len(data) % 2880))


def gaussian(x, s):
    r = int(4 * s) + 1
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / s) ** 2)
    k /= k.sum()
    x = np.apply_along_axis(lambda v: np.convolve(np.pad(v, r, mode="symmetric"), k, "valid"), 1, x)
    return np.apply_along_axis(lambda v: np.convolve(np.pad(v, r, mode="symmetric"), k, "valid"), 0, x)


def shift(img, dy, dx):
    fy, fx = np.fft.fftfreq(img.shape[0])[:, None], np.fft.fftfreq(img.shape[1])[None, :]
    return np.fft.ifft2(np.fft.fft2(img) * np.exp(-2j * np.pi * (fy * dy + fx * dx))).real


def scene(seed=1, level=0.6):
    """(calibrated linear RGB, disk mask, sky mask): the disc in Saturn's colour, sky at 0 with noise 0.0005."""
    yy, xx = np.mgrid[0:H, 0:W]
    disc = gaussian(((yy - 40.3) ** 2 + (xx - 47.6) ** 2 <= R * R).astype(float), 1.0) * level
    rgb = np.stack([disc * SATURN[1], disc, disc * SATURN[0]])
    rgb += np.random.default_rng(seed).normal(0, 0.0005, rgb.shape)
    L = disc
    disk = L >= 0.04 * level
    near = np.zeros_like(disk)
    for dy in range(-4, 5):
        for dx in range(-4, 5):
            if dy * dy + dx * dx <= 16:
                near |= np.roll(np.roll(disk, dy, 0), dx, 1)
    return rgb, disk, ~near


ALIGN_NONE = ("Channel alignment (small disc): R +0.000, +0.000 px (+-0.010), B +0.000, +0.000 px (+-0.010) against G, "
              "from 400 px of the limb band in 16 sectors; within 0.1 px or within 3 sigma of its uncertainty, not applied")
# R resampled (no reason given for anything left): a residual over 0.1 px is a FAIL, not ruling Q4's NOT JUDGED.
ALIGN_APPLIED = ("Channel alignment (small disc): R +0.500, +0.000 px (+-0.010), B +0.000, +0.000 px (+-0.010) against G, "
                 "from 400 px of the limb band in 16 sectors; R and B resampled onto G (Catmull-Rom)")


def history(calib=True, align=ALIGN_NONE):
    return ["Planet disc: 452 px (equivalent radius 12.0 px), 5.89% of the frame, sky 80.00%",
            ("Colour calibration: Saturn's reflectance B/G 0.679 R/G 1.238 (synthetic)" if calib else
             "Colour calibration skipped: no planet disc distinguished from the sky; nothing done"),
            "Background: sky plane removed per channel (synthetic)",
            "Display transfer: linear (no midtones stretch), white point 1.00000",
            align,
            "Sharpen (luminance): amount 1.50 (synthetic)",
            "Saturation: 1.00x the natural colour (synthetic)"]


def shift_json(dx=0.0, applied=False):
    return {"dx": dx, "dy": 0.0, "ux": 0.01, "uy": 0.01, "measured": True, "applied": applied, "alongAxis": False}


def write_part(d, rgb, disk, sky, out=None, notice="", stack_refused=False, rshift=0.0, applied=False, nan_cal=False,
               bshift=0.0, nan_k0_b=False, caution=""):
    """caution: the engine's notice on a processed image (ruling Q13: a disc filling the frame), the disc kept;
    rshift: R left that many px right of G in the display, the unaligned display and the output (a residual
    dispersion); bshift: the same for B (declined too); applied: the engine says it resampled R (HISTORY gives no reason
    for the residual) instead of declining it; nan_cal: one disc sample of the calibrated RGB is NaN (a metric that must
    fail, not pass); nan_k0_b: one disc sample of the Saturation 0 output's B is NaN (the "7 grey" metric)."""
    os.makedirs(os.path.join(d, "stages"), exist_ok=True)
    if stack_refused:
        dump(os.path.join(d, "probe.json"), {"capture": "synthetic", "diagnosis": {"kind": "Clipped", "message":
             "The target is saturated."}, "target": "Saturn", "filter": "Broadband"})
        return
    if rshift:
        rgb = np.stack([shift(rgb[0], 0, rshift), rgb[1], rgb[2]])
    if bshift:
        rgb = np.stack([rgb[0], rgb[1], shift(rgb[2], 0, bshift)])
    display = np.clip(rgb, 0, 1)
    L = display.mean(axis=0)
    out = display if out is None else out
    stack = rgb + 0.01
    write_fits(os.path.join(d, "stack.fits"), stack)
    if notice:
        out = np.clip(stack, 0, 1).astype(np.float32).astype(np.float64)
    write_fits(os.path.join(d, "planet.fits"), out)
    for a in SHARPENS:
        write_fits(os.path.join(d, "planet_s%g.fits" % a), out)
    for s in BLURS:
        write_fits(os.path.join(d, "planet_c%g.fits" % s), out)
    k0 = np.stack([out.mean(axis=0)] * 3)
    if nan_k0_b:
        k0[2][tuple(np.argwhere(disk)[0])] = np.nan
    write_fits(os.path.join(d, "planet_k0.fits"), k0)
    st = os.path.join(d, "stages")
    write_fits(os.path.join(st, "display.fits"), display)
    write_fits(os.path.join(st, "display_unaligned.fits"), display)
    write_fits(os.path.join(st, "lum_sharp.fits"), L)
    write_fits(os.path.join(st, "lin_unsharp.fits"), L)
    write_fits(os.path.join(st, "lin_sharp.fits"), L)
    cal = rgb.copy()
    if nan_cal:
        cal[1][tuple(np.argwhere(disk)[0])] = np.nan
    write_fits(os.path.join(st, "calibrated.fits"), cal)
    write_fits(os.path.join(st, "masks.fits"), np.stack([disk, sky]).astype(np.float32))
    ys, xs = np.nonzero(disk)
    planet = {
        "refusal": None, "notice": notice or caution, "saturation": 1.0, "chromaBlur": 1.0, "sharpen": 1.5, "sliderSeconds": 0.001,
        "curve": {"white": 1.0, "balance": 0.5},
        "disc": {"pixels": 0 if notice else int(disk.sum()), "sky": int(sky.sum()), "equivRadius": R,
                 "box": None if notice else [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())], "reason": ""},
        "dispersion": {"before": None if notice else {"r": shift_json(0.5 if applied else rshift, applied), "b": shift_json(bshift),
                                                       "pixels": 400, "sectors": 16,
                                                       "frameCut": False, "reason": ""},
                       "after": None if notice else {"r": shift_json(), "b": shift_json(), "pixels": 400, "sectors": 16,
                                                      "frameCut": False, "reason": ""}},
        "history": history(not notice, ALIGN_APPLIED if applied else ALIGN_NONE), "stepSeconds": {}, "seconds": 0.0, "output": os.path.join(d, "planet.fits"),
        "sharpenAlso": [{"sharpen": a, "output": os.path.join(d, "planet_s%g.fits" % a)} for a in SHARPENS],
        "chromaBlurAlso": [{"chromaBlur": s, "output": os.path.join(d, "planet_c%g.fits" % s)} for s in BLURS],
        "saturationAlso": [{"saturation": 0, "output": os.path.join(d, "planet_k0.fits")}],
        "stages": st, "min": 0.0, "max": float(out.max())}
    j = {"capture": "synthetic", "frames": 100, "used": 90, "measured": 100, "threads": 1,
         "image": {"width": W, "height": H, "channels": 3}, "target": "Saturn", "filter": "Broadband", "planet": planet}
    dump(os.path.join(d, "probe.json"), j)


def write_gate(run):
    g = os.path.join(run, "gate")
    os.makedirs(g, exist_ok=True)
    for b in ("auto", "RGGB"):
        with open(os.path.join(g, b + ".exit"), "w") as f:
            f.write("4\n")
        dump(os.path.join(g, b + ".json"), {"planet": {"refusal": "Planet processing needs a broadband filter; HaO3 is "
                                                           "narrowband, so its colours come from the filter."}})


class PlanetCheckFailsClosed(unittest.TestCase):
    def setUp(self):
        self.run_dir = tempfile.mkdtemp(prefix="planet_check_test_")
        self.rgb, self.disk, self.sky = scene()

    def tearDown(self):
        shutil.rmtree(self.run_dir, ignore_errors=True)

    def make(self, cid="synth", out=None, **kw):
        write_part(os.path.join(self.run_dir, cid, "full"), self.rgb, self.disk, self.sky, out=out, **kw)

    def check(self, *ids):
        p = subprocess.run([sys.executable, "-I", CHECK, "--no-png", self.run_dir, *ids], capture_output=True,
                           text=True, timeout=600)
        return p.returncode, p.stdout + p.stderr

    def rows(self, cid="synth"):
        with open(os.path.join(self.run_dir, cid, "check.json")) as f:
            return {r["req"]: r["verdict"] for r in json.load(f)["rows"]}

    def test_clean_passes(self):
        self.make()
        write_gate(self.run_dir)
        code, text = self.check()
        self.assertEqual(code, 0, text)
        rows = self.rows()
        for req in ("1 calib", "2 sky", "3 disp R", "3 disp B", "4 fringe", "5 skynoise", "6 clip", "7 natural", "7 grey"):
            self.assertEqual(rows.get(req), "PASS", "%s: %s\n%s" % (req, rows.get(req), text))

    def test_fringe_fails(self):
        # R and B 0.8 px apart along x in the output: the limb's opposite sides take opposite colours.
        fringe = np.stack([shift(self.rgb[0], 0, 0.4), self.rgb[1], shift(self.rgb[2], 0, -0.4)])
        self.make(out=np.clip(fringe, 0, 1))
        write_gate(self.run_dir)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertEqual(self.rows().get("4 fringe"), "FAIL", text)

    def test_missing_stage_fails(self):
        self.make()
        write_gate(self.run_dir)
        os.remove(os.path.join(self.run_dir, "synth", "full", "stages", "calibrated.fits"))
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertIn("harness", text)

    def test_missing_gate_fails(self):
        self.make()
        code, text = self.check()
        self.assertEqual(code, 1, text)

    def test_empty_run_fails(self):
        code, text = self.check()
        self.assertEqual(code, 1, text)

    def test_no_disc_and_expected_refusal_are_not_failures(self):
        self.make("nodisc", notice="no planet disc distinguished from the sky")
        self.make("refused", stack_refused=True)
        write_gate(self.run_dir)
        code, text = self.check("nodisc", "refused:acceptance:Clipped")
        self.assertEqual(code, 0, text)
        self.assertEqual(self.rows("nodisc").get("no disc"), "PASS", text)
        self.assertEqual(self.rows("refused").get("stack"), "PASS", text)

    def test_frame_filling_caution_fails_and_is_not_no_disc(self):
        # Ruling Q13: a real capture's planet is in a sky a quadratic models, so the caution (a disc filling the frame) is a
        # FAIL of its own, and the capture is still judged as processed, not as the no-disc outcome.
        self.make(caution="the planet disc is 75% or more of the frame: the disc fills the frame, or the sky is curved "
                          "more than a quadratic models; colour calibration, the background and the sharpening then treat "
                          "the whole frame as the planet")
        write_gate(self.run_dir)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        rows = self.rows()
        self.assertEqual(rows.get("Q13 frame disc"), "FAIL", text)
        self.assertNotIn("no disc", rows, text)
        self.assertIn("6 clip", rows, text)
        self.assertIn("2 sky", rows, text)

    def test_unexpected_refusal_fails(self):
        # A capture the table expects an image of (a Saturn, a Jupiter) refused by the stacker: a regression, not an
        # outcome; and a refusal of another kind than the one expected.
        self.make("refused", stack_refused=True)
        write_gate(self.run_dir)
        for ids in (("refused",), ("refused:acceptance:NoTarget",)):
            with self.subTest(ids=ids):
                code, text = self.check(*ids)
                self.assertEqual(code, 1, text)
                self.assertEqual(self.rows("refused").get("stack"), "FAIL", text)

    def test_image_where_refusal_expected_fails(self):
        self.make()
        write_gate(self.run_dir)
        code, text = self.check("synth:acceptance:Clipped")
        self.assertEqual(code, 1, text)
        self.assertEqual(self.rows().get("stack"), "FAIL", text)

    def test_nan_metric_fails(self):
        self.make(nan_cal=True)
        write_gate(self.run_dir)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertEqual(self.rows().get("1 calib"), "FAIL", text)

    def test_declined_channel_not_judged(self):
        # Ruling Q4: R 0.5 px off G left by the engine with HISTORY's reason -> acceptance 3 R and the fringe row NOT
        # JUDGED (exit 0); the same residual where the engine claims to have resampled R -> FAIL.
        self.make(rshift=0.5)
        write_gate(self.run_dir)
        code, text = self.check()
        self.assertEqual(code, 0, text)
        rows = self.rows()
        self.assertEqual(rows.get("3 disp R"), "NOT JUDGED", text)
        self.assertEqual(rows.get("4 fringe"), "NOT JUDGED", text)
        self.make(rshift=0.5, applied=True)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertEqual(self.rows().get("3 disp R"), "FAIL", text)

    def test_declined_channel_capped(self):
        # Ruling Q4 is capped (final review): a declined channel is NOT JUDGED only while its measured residual is
        # under 0.5 px, and only one per capture -- an over-wide sigma regression that declines everything must fail.
        self.make(rshift=0.7)
        write_gate(self.run_dir)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertEqual(self.rows().get("3 disp R"), "FAIL", text)
        self.make(rshift=0.4, bshift=-0.4)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        rows = self.rows()
        self.assertEqual(rows.get("3 disp R"), "NOT JUDGED", text)
        self.assertEqual(rows.get("3 disp B"), "FAIL", text)

    def test_nan_limb_residual_fails(self):
        # A NaN limb residual on a declined channel must fail, not fall through "<= 0.1" into ruling Q4's NOT JUDGED.
        # In-process, with the checker's limb estimator returning NaN for R after the alignment (a finite input cannot
        # make it).
        sys.dont_write_bytecode = True
        sys.path.insert(0, HERE)
        import planet_check
        self.make()
        write_gate(self.run_dir)
        real = planet_check.limb_shift
        calls = []

        def nan_after(*a):
            out = real(*a)
            calls.append(1)
            if len(calls) == 2 and out is not None:  # the second call measures the aligned display
                out["R"] = dict(out["R"], mag=float("nan"))
            return out
        planet_check.limb_shift = nan_after
        try:
            code = planet_check.main(["--no-png", self.run_dir])
        finally:
            planet_check.limb_shift = real
        self.assertEqual(len(calls), 2)
        self.assertEqual(code, 1)
        self.assertEqual(self.rows().get("3 disp R"), "FAIL")

    def test_nan_grey_fails(self):
        # "7 grey": a NaN in Saturation 0's B must fail (builtin max(a, nan) returns a).
        self.make(nan_k0_b=True)
        write_gate(self.run_dir)
        code, text = self.check()
        self.assertEqual(code, 1, text)
        self.assertEqual(self.rows().get("7 grey"), "FAIL", text)

    def test_pngs(self):
        try:
            import PIL  # noqa: F401
        except ImportError:
            self.skipTest("Pillow not importable")
        self.make()
        write_gate(self.run_dir)
        p = subprocess.run([sys.executable, "-I", CHECK, self.run_dir], capture_output=True, text=True, timeout=600)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        for name in ("plain_vs_processed", "sharpen_series", "chroma_series"):
            self.assertTrue(os.path.isfile(os.path.join(self.run_dir, "png", "synth_%s.png" % name)), name)


if __name__ == "__main__":
    unittest.main(verbosity=1)
