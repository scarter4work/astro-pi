#!/usr/bin/env python3
"""EZHazeKill parity and quality check against the offline reference (hk_lib.py).

Used by two harnesses that write the same probe-out.json schema:
  - modules/ez-tools/test/run-probe-parity.sh   the EZTools module's EZHazeKill process (probe-parity.js)
  - scripts/ez-stretch/test/run-hazekill-parity.sh   the EZHazeKill.js 1.1.0 script (hazekill-probe.js)

  prep  <run_dir> [--extra] [case ...]
        prepare each case's input exactly as the design study did (hk_run.py: gradient extraction for
        the linear masters, then an unlinked AutoSTF for N3), write it as float32 XISF and write
        <run_dir>/cases.json. --extra adds three module-only cases built from L2:
          uint16_rgb      L2 x 65535, rounded, as a 16-bit integer image (Neutralize 0.55: on this image the
                          offsets end 0.95, 0.75 and 0.3 of a 16-bit step, so rounding on write is tested)
          preview_target  L2 with a 1024x768 preview at (1200,600): the probe executes on the preview
          nan_refused     L2 with one NaN sample: must be refused, image unchanged
  check <run_dir> [--script-run <dir>]
        compare PixInsight's output with hk_lib.hk_v11() on the same input, recompute the design's
        metrics on PixInsight's output, print both, exit 1 on any failure. With --script-run, also
        compare every common case's output sample by sample with the script harness's output there.

The model is read back from the probe's JSON when the probe has it (the script), otherwise from the
console log the probe captured (the module prints the script's report: 5 decimals).

Sources are read only (NAS masters and two local stretched images; override with HK_MASTER_DIR,
HK_N1, HK_N2). A case whose source is missing is skipped with a note, unless HK_REQUIRE_ALL=1.
"""
import json
import os
import re
import sys

import numpy as np
import xisf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hk_lib import (apply_stf, extract_gradient, hk_v11, masks, metrics, ref_sky,  # noqa: E402
                    stf_params)

MASTER_DIR = os.environ.get("HK_MASTER_DIR", "/mnt/qnap/astro_data/processing-archive-2026-10-03/master")
M = MASTER_DIR + "/masterLight_BIN-1_3840x2160_EXPOSURE-120.00s_FILTER-"

# name, source, prep, design faint luminance retained (v1.1 defaults, design table 3)
CASES = [
    ("L1_LDN1235_Full_linear", M + "Full_RGB.xisf", "linear", 87.9),
    ("L2_M33_Lqef_linear", M + "Lqef_RGB.xisf", "linear", 84.3),
    ("L3_NGC7000_HaO3_linear", M + "HaO3_RGB_(10).xisf", "linear", 66.6),
    ("N1_M33_Scott_GHS_stretched",
     os.environ.get("HK_N1", os.path.expanduser("~/Desktop/M33_RGB_9_8_2026.xisf")), "stretched", 98.6),
    ("N2_IC1396_HOO_stretched",
     os.environ.get("HK_N2", os.path.expanduser("~/astro_work/ic1396/masters/hoo_final.xisf")), "stretched", 96.2),
    ("N3_NGC7000_HaO3_STFstretched", M + "HaO3_RGB_(10).xisf", "stf", 93.0),
    # Not in the design study: L2's green channel alone, for the single-channel (grey-scale) path.
    ("M1_M33_Lqef_G_mono_linear", M + "Lqef_RGB.xisf", "mono", None),
]
LINEAR_KINDS = ("linear", "mono", "uint16", "preview", "nan")
EXTRA_BASE = "L2_M33_Lqef_linear"
PREVIEW_RECT = (1200, 600, 2224, 1368)       # x0, y0, x1, y1 (1024 x 768)
NAN_AT = (100, 100, 0)                       # y, x, channel
DEFAULTS = dict(amount=0.5, neutralize=1.0, target=0.05)

# Tolerances (PixInsight output vs hk_lib on identical input).
TOL_PIXEL_MAX = 2.5e-7     # max |PI - ref| over every sample: 4 float32 ULPs at 1.0
TOL_PIXEL_RMS = 5e-8       # RMS of the same
TOL_MODEL = 1e-7           # sky, sigma, offset, eps, T_eff, pedestal read from the probe (hk_lib in float32)
TOL_MODEL_LOG = 5.01e-6    # the same values read from the console report, printed with 5 decimals
TOL_UINT16 = 0.5 / 65535 + 1e-9   # 16-bit output vs the exact (float64) result: rounding to nearest only
TOL_SCRIPT = 2.5e-7        # module output vs script output (both float32, both within TOL_PIXEL_MAX of ref)
TOL_DESIGN_RETAINED = 0.2  # faint luminance retained vs the design table, percentage points
MIN_RETAINED_STRETCHED = 96.0


def read(path, as_float64=False):
    """Image as (H, W, C); integer images scaled to [0,1] (PixInsight's sample conversion)."""
    a = xisf.XISF(path).read_image(0)
    if a.ndim == 2:
        a = a[..., None]
    if np.issubdtype(a.dtype, np.integer):
        return a.astype(np.float64) / float(np.iinfo(a.dtype).max)
    return a.astype(np.float64 if as_float64 else np.float32)


def write(path, img):
    xisf.XISF.write(path, np.ascontiguousarray(img), creator_app="EZHazeKill parity harness")


def prep(run_dir, which, extra):
    os.makedirs(run_dir, exist_ok=True)
    cases = []
    base_input = None
    for name, src, kind, _ in CASES:
        if which and name not in which and not (extra and name == EXTRA_BASE):
            continue
        if not os.path.exists(src):
            if os.environ.get("HK_REQUIRE_ALL") == "1":
                sys.exit("missing source for %s: %s" % (name, src))
            print("  skip  %s: source not found (%s)" % (name, src))
            continue
        img = np.clip(read(src), 0, 1)                       # as hk_run.py
        if kind in ("linear", "stf", "mono"):
            img, _ = extract_gradient(img)
        if kind == "mono":
            img = img[..., 1:2]
        if kind == "stf":
            c0, m = stf_params(img, linked=False)
            img = apply_stf(img, c0, m).astype(np.float32)
        img = np.ascontiguousarray(img, dtype=np.float32)
        inp = os.path.join(run_dir, name + ".in.xisf")
        write(inp, img)
        cases.append(dict(name=name, kind=kind, source=src, input=inp,
                          output=os.path.join(run_dir, name + ".pi.xisf"),
                          log=os.path.join(run_dir, name + ".log"), **DEFAULTS))
        print("  prep  %s (%s, %dx%dx%d)" % (name, kind, img.shape[1], img.shape[0], img.shape[2]))
        if name == EXTRA_BASE:
            base_input = (inp, img)
    if extra:
        if base_input is None:
            sys.exit("--extra needs %s's source" % EXTRA_BASE)
        inp, img = base_input
        u16 = os.path.join(run_dir, "uint16_rgb.in.xisf")
        write(u16, np.round(np.clip(img.astype(np.float64), 0, 1) * 65535).astype(np.uint16))
        # PixInsight's XISF reader turns a stored NaN into 0 (measured 2026-11-10, PI 1.9.6), so the
        # probe injects the NaN after opening, and checks in PixInsight that the image is unchanged.
        y, x, ch = NAN_AT
        for name, kind, path, more in (("uint16_rgb", "uint16", u16, {"neutralize": 0.55}),
                                       ("preview_target", "preview", inp, {"preview": list(PREVIEW_RECT)}),
                                       ("nan_refused", "nan", inp, {"expect_error": "NaN or infinite",
                                                                     "inject_nan": [x, y, ch]})):
            cases.append(dict(name=name, kind=kind, source=EXTRA_BASE, input=path,
                              output=os.path.join(run_dir, name + ".pi.xisf"),
                              log=os.path.join(run_dir, name + ".log"), **{**DEFAULTS, **more}))
            print("  prep  %s (%s, from %s)" % (name, kind, EXTRA_BASE))
    if not cases:
        sys.exit("no case could be prepared")
    json.dump({"cases": cases}, open(os.path.join(run_dir, "cases.json"), "w"), indent=1)


REPORT_LINE = re.compile(r"^\s+([RGBK]): sky ([-+0-9.]+)\s+sigma ([-+0-9.]+)\s+cast ([-+0-9.]+)\s+subtract ([-+0-9.]+)\s*$")


def model_from_log(text):
    """sky, sigma, offset per channel from the console report (the first report in the log)."""
    sky, sigma, offset = [], [], []
    for line in text.splitlines():
        line = re.sub(r"^\[[^\]]*\] ", "", line)      # console log lines carry a "[timestamp] " prefix
        m = REPORT_LINE.match(line)
        if m:
            sky.append(float(m.group(2)))
            sigma.append(float(m.group(3)))
            offset.append(float(m.group(5)))
        elif sky:
            break
    return dict(sky=sky, sigma=sigma, offset=offset)


def check(run_dir, script_run=None):
    cases = json.load(open(os.path.join(run_dir, "cases.json")))["cases"]
    probe = json.load(open(os.path.join(run_dir, "probe-out.json")))
    design = {n: d for n, _, _, d in CASES}
    fails = []
    pres = {r["name"]: r for r in probe["cases"]}
    if not probe.get("ok") and not all(c.get("expect_error") for c in cases if not pres.get(c["name"], {}).get("ok")):
        fails.append("probe: " + probe.get("error", "a case failed"))
    report = {"version": probe.get("version"), "cases": {}}
    print("%-30s %10s %10s %10s %8s %9s %9s %9s %10s" % ("case", "max|d|", "rms", "model|d|", "clip0%",
                                                          "retained", "design", "hue_deg", "vs script"))
    for c in cases:
        name, kind = c["name"], c["kind"]
        r = pres.get(name)
        if r is None:
            fails.append("%s: no probe result" % name)
            continue
        log = open(c["log"]).read() if c.get("log") and os.path.exists(c["log"]) else ""

        if c.get("expect_error"):
            err = (r.get("error") or "") + "\n" + log
            if r.get("ok"):
                fails.append("%s: executed, expected a refusal (%s)" % (name, c["expect_error"]))
            elif c["expect_error"] not in err:
                fails.append("%s: refused, but without '%s': %s" % (name, c["expect_error"], r.get("error")))
            # The probe compares every sample before and after in PixInsight (NaN-aware), because the
            # NaN it injected does not survive an XISF round trip.
            unchanged = r.get("unchanged") is True
            if not unchanged:
                fails.append("%s: the image changed (or was not saved) after the refusal" % name)
            print("%-30s refused: %s; image unchanged: %s" % (name, (r.get("error") or "").strip()[:60], unchanged))
            report["cases"][name] = dict(kind=kind, refused=not r.get("ok"), error=r.get("error"), unchanged=unchanged)
            continue

        if not r.get("ok"):
            fails.append("%s: PixInsight case failed: %s" % (name, r.get("error", "missing")))
            continue
        exact = kind == "uint16"                       # reference in float64 for the integer path
        img = read(c["input"], as_float64=exact)
        if c.get("preview"):
            x0, y0, x1, y1 = c["preview"]
            img = img[y0:y1, x0:x1]
        pi = read(c["output"], as_float64=True)
        ref, info = hk_v11(img, amount=c["amount"], neutralize=c["neutralize"], target=c["target"])
        if not exact:
            ref = ref.astype(np.float32)
        if pi.shape != ref.shape:
            fails.append("%s: output shape %s, expected %s" % (name, pi.shape, ref.shape))
            continue
        d = np.abs(pi.astype(np.float64) - ref.astype(np.float64))
        dmax, drms = float(d.max()), float(np.sqrt((d ** 2).mean()))

        if "sky" in r:                                 # the script probe returns its model
            got = {k: r[k] for k in ("sky", "sigma", "offset")}
            tol_model = TOL_MODEL
        else:                                          # the module: its console report
            got = model_from_log(log)
            tol_model = TOL_MODEL_LOG
            if "Sky from the darkest 10% of" not in log:
                fails.append("%s: the console log has no 'Sky from the darkest 10%% of' report" % name)
        if len(got["sky"]) != len(info["sky"]):
            fails.append("%s: model has %d channels, expected %d" % (name, len(got["sky"]), len(info["sky"])))
            continue
        model_d = max(float(np.max(np.abs(np.asarray(got[k]) - np.asarray(info[k])))) for k in ("sky", "sigma", "offset"))
        if "eps" in r:
            eps_ref = max(1e-6, 2.0 * float(info["sigma"].max()))
            model_d = max(model_d, abs(r["eps"] - eps_ref), abs(r["tEff"] - float(info["target"])),
                          abs(r["ped"] - float(info["ped"])))
            stretched = r["stretched"]
        else:
            stretched = "Note: the sky noise is" in log

        vs_script = None
        if script_run:
            sp = os.path.join(script_run, name + ".pi.xisf")
            if os.path.exists(sp):
                vs_script = float(np.max(np.abs(read(sp, as_float64=True) - pi)))
                if vs_script > TOL_SCRIPT:
                    fails.append("%s: max |module - script| %.3g > %.1e" % (name, vs_script, TOL_SCRIPT))

        entry = dict(kind=kind, pixel_max_abs=dmax, pixel_rms=drms, model_max_abs=model_d, stretched=stretched,
                     pi_seconds=r.get("seconds"), max_abs_vs_script=vs_script)
        ret, hue, clip = None, None, 100 * float((pi <= 0).any(2).mean())
        if kind not in ("uint16", "preview"):
            # Design metrics on PixInsight's output (hk_run.py's definitions; the tool's background is the
            # measured sky minus what was subtracted).
            sky_r, sig_r, _, _ = ref_sky(img)
            mk = masks(img, sky_r, sig_r)
            bg = np.asarray(r["sky"]) - np.asarray(r["offset"]) if "sky" in r else info["sky"] - info["offset"]
            met = metrics(img, pi.astype(np.float32), mk, sky_r, bg)
            ret, hue, clip = met["faint_lum_retained_%"], met["faint_hue_shift_deg"], met["clip0_%"]
            entry.update(metrics=met, design_retained=design.get(name))
        report["cases"][name] = entry
        print("%-30s %10.2e %10.2e %10.2e %8.4f %9s %9s %9s %10s" % (
            name, dmax, drms, model_d, clip, "-" if ret is None else "%.1f" % ret,
            "-" if design.get(name) is None else "%.1f" % design[name], "-" if hue is None else "%.1f" % hue,
            "-" if vs_script is None else "%.2e" % vs_script))

        if exact:
            if dmax > TOL_UINT16:
                fails.append("%s: max |PI - round(ref)| %.3g > 0.5/65535" % (name, dmax))
            # Exactly the nearest 16-bit value, except where the exact result sits on a half step.
            q = ref * 65535.0
            frac = q - np.floor(q)
            tie = np.abs(frac - 0.5) < 1e-6
            wrong = int(((np.round(pi * 65535.0) != np.round(q)) & ~tie).sum())
            upper = int((frac > 0.5 + 1e-6).sum())   # samples where truncation would differ from rounding
            entry["uint16_not_nearest"] = wrong
            entry["uint16_samples_rounding_up"] = upper
            print("%-30s uint16: %d sample(s) not the nearest 16-bit value; %d sample(s) round up" % ("", wrong, upper))
            if wrong:
                fails.append("%s: %d samples are not the nearest 16-bit value of the exact result" % (name, wrong))
            if upper < 1000:
                fails.append("%s: only %d samples round up: the case cannot tell rounding from truncation" % (name, upper))
        else:
            if dmax > TOL_PIXEL_MAX:
                fails.append("%s: max |PI - ref| %.3g > %.1e" % (name, dmax, TOL_PIXEL_MAX))
            if drms > TOL_PIXEL_RMS:
                fails.append("%s: RMS |PI - ref| %.3g > %.0e" % (name, drms, TOL_PIXEL_RMS))
        if model_d > tol_model:
            fails.append("%s: model |PI - ref| %.3g > %.2e" % (name, model_d, tol_model))
        if clip != 0:
            fails.append("%s: %.4f%% of pixels clipped to 0" % (name, clip))
        if ret is not None and design.get(name) is not None and abs(ret - design[name]) > TOL_DESIGN_RETAINED:
            fails.append("%s: faint retained %.1f%% vs design %.1f%%" % (name, ret, design[name]))
        if kind == "stretched" and ret is not None and ret < MIN_RETAINED_STRETCHED:
            fails.append("%s: faint retained %.1f%% < %.0f%% on a stretched image" % (name, ret, MIN_RETAINED_STRETCHED))
        if stretched != (kind not in LINEAR_KINDS):
            fails.append("%s: stretched note %s for a %s image" % (name, stretched, kind))
    json.dump(report, open(os.path.join(run_dir, "report.json"), "w"), indent=1, default=float)
    print("tolerances: pixel max %.1e, pixel RMS %.0e, model %.0e (probe) / %.2e (5-decimal report), uint16 "
          "0.5/65535, module vs script %.1e, design retained +-%.1f pp; gates: 0%% clipped, stretched retained >= %.0f%%"
          % (TOL_PIXEL_MAX, TOL_PIXEL_RMS, TOL_MODEL, TOL_MODEL_LOG, TOL_SCRIPT, TOL_DESIGN_RETAINED,
             MIN_RETAINED_STRETCHED))
    for f in fails:
        print("  FAIL  " + f)
    print(("PASS: %d case(s)" % len(report["cases"])) if not fails else "FAIL: %d problem(s)" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("prep", "check"):
        sys.exit(__doc__)
    args = sys.argv[3:]
    if sys.argv[1] == "prep":
        prep(sys.argv[2], set(a for a in args if a != "--extra"), "--extra" in args)
    else:
        sr = None
        if "--script-run" in args:
            i = args.index("--script-run")
            sr = args[i + 1]
        sys.exit(check(sys.argv[2], sr))
