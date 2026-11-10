#!/usr/bin/env python3
"""EZHazeKill 1.1 parity and quality check against the offline reference (hk_lib.py).

  prep  <run_dir> [case ...]   prepare each case's input exactly as the design study did
                               (hk_run.py: gradient extraction for the linear masters, then an
                               unlinked AutoSTF for N3), write it as float32 XISF and write
                               <run_dir>/cases.json for hazekill-probe.js
  check <run_dir>              compare PixInsight's output (hazekill-probe.js) with
                               hk_lib.hk_v11() on the same float32 input, recompute the design's
                               metrics on PixInsight's output, print both, exit 1 on any failure

Sources are read only (NAS masters and two local stretched images; override with HK_MASTER_DIR,
HK_N1, HK_N2). A case whose source is missing is skipped with a note, unless HK_REQUIRE_ALL=1.
"""
import json
import os
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
LINEAR_KINDS = ("linear", "mono")
DEFAULTS = dict(amount=0.5, neutralize=1.0, target=0.05)

# Tolerances (PixInsight output vs hk_lib on identical float32 input).
TOL_PIXEL_MAX = 2.5e-7     # max |PI - ref| over every sample: 4 float32 ULPs at 1.0
TOL_PIXEL_RMS = 5e-8       # RMS of the same
TOL_MODEL = 1e-7           # sky, sigma, offset, eps, T_eff, pedestal (hk_lib computes in float32)
TOL_DESIGN_RETAINED = 0.2  # faint luminance retained vs the design table, percentage points
MIN_RETAINED_STRETCHED = 96.0


def read(path):
    a = xisf.XISF(path).read_image(0)
    if a.ndim == 2:
        a = a[..., None]
    return a.astype(np.float32)


def prep(run_dir, which):
    os.makedirs(run_dir, exist_ok=True)
    cases = []
    for name, src, kind, _ in CASES:
        if which and name not in which:
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
        xisf.XISF.write(inp, img, creator_app="EZHazeKill parity harness")
        cases.append(dict(name=name, kind=kind, source=src, input=inp,
                          output=os.path.join(run_dir, name + ".pi.xisf"), **DEFAULTS))
        print("  prep  %s (%s, %dx%dx%d)" % (name, kind, img.shape[1], img.shape[0], img.shape[2]))
    if not cases:
        sys.exit("no case could be prepared")
    json.dump({"cases": cases}, open(os.path.join(run_dir, "cases.json"), "w"), indent=1)


def check(run_dir):
    cases = json.load(open(os.path.join(run_dir, "cases.json")))["cases"]
    probe = json.load(open(os.path.join(run_dir, "probe-out.json")))
    design = {n: d for n, _, _, d in CASES}
    fails = []
    if not probe.get("ok"):
        fails.append("probe: " + probe.get("error", "a case failed"))
    pres = {r["name"]: r for r in probe["cases"]}
    report = {"version": probe.get("version"), "cases": {}}
    print("%-30s %10s %10s %10s %8s %9s %9s %9s" % ("case", "max|d|", "rms", "model|d|", "clip0%",
                                                     "retained", "design", "hue_deg"))
    for c in cases:
        name = c["name"]
        r = pres.get(name)
        if not r or not r.get("ok"):
            fails.append("%s: PixInsight case failed: %s" % (name, (r or {}).get("error", "missing")))
            continue
        img = read(c["input"])
        pi = read(c["output"])
        ref, info = hk_v11(img, amount=c["amount"], neutralize=c["neutralize"], target=c["target"])
        ref = ref.astype(np.float32)
        d = np.abs(pi.astype(np.float64) - ref.astype(np.float64))
        dmax, drms = float(d.max()), float(np.sqrt((d ** 2).mean()))
        model_d = max(float(np.max(np.abs(np.asarray(r[k]) - np.asarray(info[k])))) for k in ("sky", "sigma", "offset"))
        eps_ref = max(1e-6, 2.0 * float(info["sigma"].max()))
        model_d = max(model_d, abs(r["eps"] - eps_ref), abs(r["tEff"] - float(info["target"])),
                      abs(r["ped"] - float(info["ped"])))
        # Design metrics on PixInsight's output (hk_run.py's definitions, its own numbers for the tool's
        # background: what PixInsight measured minus what it subtracted).
        sky_r, sig_r, _, _ = ref_sky(img)
        mk = masks(img, sky_r, sig_r)
        met = metrics(img, pi, mk, sky_r, np.asarray(r["sky"]) - np.asarray(r["offset"]))
        ret = met["faint_lum_retained_%"]
        print("%-30s %10.2e %10.2e %10.2e %8.4f %9.1f %9s %9.1f" % (
            name, dmax, drms, model_d, met["clip0_%"], ret,
            "-" if design[name] is None else "%.1f" % design[name], met["faint_hue_shift_deg"]))
        report["cases"][name] = dict(kind=c["kind"], pixel_max_abs=dmax, pixel_rms=drms, model_max_abs=model_d,
                                     pi_seconds=r["seconds"], pi_model={k: r[k] for k in ("sky", "sigma", "offset",
                                     "eps", "tEff", "ped", "stretched")}, metrics=met, design_retained=design[name])
        if dmax > TOL_PIXEL_MAX:
            fails.append("%s: max |PI - ref| %.3g > %.0e" % (name, dmax, TOL_PIXEL_MAX))
        if drms > TOL_PIXEL_RMS:
            fails.append("%s: RMS |PI - ref| %.3g > %.0e" % (name, drms, TOL_PIXEL_RMS))
        if model_d > TOL_MODEL:
            fails.append("%s: model |PI - ref| %.3g > %.0e" % (name, model_d, TOL_MODEL))
        if met["clip0_%"] != 0:
            fails.append("%s: %.4f%% of pixels clipped to 0" % (name, met["clip0_%"]))
        if design[name] is not None and abs(ret - design[name]) > TOL_DESIGN_RETAINED:
            fails.append("%s: faint retained %.1f%% vs design %.1f%%" % (name, ret, design[name]))
        if c["kind"] == "stretched" and ret < MIN_RETAINED_STRETCHED:
            fails.append("%s: faint retained %.1f%% < %.0f%% on a stretched image" % (name, ret, MIN_RETAINED_STRETCHED))
        if r["stretched"] != (c["kind"] not in LINEAR_KINDS):
            fails.append("%s: stretched note %s for a %s image" % (name, r["stretched"], c["kind"]))
    json.dump(report, open(os.path.join(run_dir, "report.json"), "w"), indent=1, default=float)
    print("tolerances: pixel max %.1e, pixel RMS %.0e, model %.0e, design retained +-%.1f pp; "
          "gates: 0%% clipped, stretched retained >= %.0f%%" % (TOL_PIXEL_MAX, TOL_PIXEL_RMS, TOL_MODEL,
                                                               TOL_DESIGN_RETAINED, MIN_RETAINED_STRETCHED))
    for f in fails:
        print("  FAIL  " + f)
    print(("PASS: %d case(s)" % len(report["cases"])) if not fails else "FAIL: %d problem(s)" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("prep", "check"):
        sys.exit(__doc__)
    if sys.argv[1] == "prep":
        prep(sys.argv[2], set(sys.argv[3:]))
    else:
        sys.exit(check(sys.argv[2]))
