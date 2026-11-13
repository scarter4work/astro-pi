#!/usr/bin/env python
"""Golden generator for the AutoContrast native PCL port.

Runs the Python reference (tools/autocontrast) and records what it computes, so every
C++ stage can be tested against it without Python at test time.

    tools/autocontrast/.venv/bin/python -I make_goldens.py synthetic <outdir>
    tools/autocontrast/.venv/bin/python -I make_goldens.py real      <outdir> [--data-dir DIR]
    tools/autocontrast/.venv/bin/python -I make_goldens.py manifest  <golden-root>
    tools/autocontrast/.venv/bin/python -I make_goldens.py all       <golden-root> [--data-dir DIR]

``synthetic`` needs nothing but the venv.  ``real`` reads (never copies) the renders under
the data dir (``--data-dir`` or env ``AC_DATA_DIR``; default tools/autocontrast/data) and
writes only derived values.  ``manifest`` writes <golden-root>/MANIFEST.json (tool versions,
reference commit, sha256 + size of every golden file).  ``all`` = synthetic + real + manifest
into <golden-root>/{synthetic,real}.

File formats
------------
* arrays: ``.npy`` (float64 unless the source is integer/bool; dtypes <f8 <f4 <i8 <u2 |u1 |b1),
* everything else: compact JSON (sorted keys).  JSON has no NaN/Infinity, so non-finite floats
  are written as the strings "NaN", "Infinity", "-Infinity" (MANIFEST ``nonfinite_encoding``).
* images that Python itself produces from 8-bit data (synthetic renders) are stored as uint8;
  the float image is ``u8.astype(float64) / 255.0`` (IEEE division, exactly numpy's).

Determinism: every random draw comes from ``np.random.default_rng(20261112)`` created afresh
inside each stage function; there are no timestamps, temp-path names or dict-order dependences
in any output file.  Running the generator twice must give ``diff -r``-identical trees.

Every stage failure is fatal: the first exception aborts the run with a traceback.
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import sys
import tempfile
import warnings
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]                      # .../astro-pi-ac
AC_ROOT = REPO / "tools" / "autocontrast"
# `python -I` drops cwd and PYTHON* from sys.path: be explicit, and put the reference first so
# an installed copy can never shadow the commit being recorded.
sys.path.insert(0, str(AC_ROOT / "src"))

import numpy as np                                            # noqa: E402
from scipy import ndimage                                     # noqa: E402
from scipy.stats import wasserstein_distance_nd               # noqa: E402

SEED = 20261112

# --------------------------------------------------------------------------------------
# Output plumbing
# --------------------------------------------------------------------------------------

_NPY_OK = {np.dtype("<f8"), np.dtype("<f4"), np.dtype("<i8"), np.dtype("<u2"),
           np.dtype("|u1"), np.dtype("|b1")}


def enc(o):
    """Recursively convert to JSON-safe Python (non-finite floats -> strings)."""
    if o is None or isinstance(o, (bool, str)):
        return o
    if isinstance(o, (np.bool_,)):
        return bool(o)
    if isinstance(o, (int, np.integer)):
        return int(o)
    if isinstance(o, (float, np.floating)):
        f = float(o)
        if math.isnan(f):
            return "NaN"
        if math.isinf(f):
            return "Infinity" if f > 0 else "-Infinity"
        return f
    if isinstance(o, np.ndarray):
        return [enc(x) for x in o.tolist()]
    if isinstance(o, dict):
        return {str(k): enc(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [enc(x) for x in o]
    if isinstance(o, (set, frozenset)):
        return sorted(enc(x) for x in o)
    if dataclasses.is_dataclass(o) and not isinstance(o, type):
        return {f.name: enc(getattr(o, f.name)) for f in dataclasses.fields(o)}
    raise TypeError(f"enc: cannot serialise {type(o).__name__}: {o!r}")


class Out:
    """Writer rooted at one output directory; remembers every file it wrote."""

    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.written: list[str] = []

    def _path(self, rel: str) -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        self.written.append(rel)
        return p

    def prune(self):
        """Delete files left over from an earlier run that this run did not write, so the tree is
        exactly what the current generator produces (and a stale golden can never linger)."""
        keep = set(self.written)
        for p in sorted(self.root.rglob("*"), reverse=True):
            if p.is_file() and p.relative_to(self.root).as_posix() not in keep:
                p.unlink()
            elif p.is_dir() and not any(p.iterdir()):
                p.rmdir()

    def npy(self, rel: str, arr, dtype=None):
        a = np.asarray(arr)
        if dtype is not None:
            a = a.astype(dtype)
        if a.dtype == np.int32 or a.dtype == np.int16 or a.dtype == np.uint32:
            a = a.astype(np.int64)
        a = np.ascontiguousarray(a)
        if a.dtype.newbyteorder("<") not in _NPY_OK and a.dtype not in _NPY_OK:
            raise TypeError(f"{rel}: dtype {a.dtype} is not in the supported .npy set")
        with open(self._path(rel), "wb") as f:
            np.save(f, a, allow_pickle=False)

    def json(self, rel: str, obj):
        text = json.dumps(enc(obj), sort_keys=True, separators=(",", ":"), allow_nan=False,
                          ensure_ascii=False)
        self._path(rel).write_text(text + "\n", encoding="utf-8")


def u8_to_rgb(u8: np.ndarray) -> np.ndarray:
    return u8.astype(np.float64) / 255.0


def to_u8(x: np.ndarray) -> np.ndarray:
    return np.rint(np.clip(x, 0.0, 1.0) * 255.0).astype(np.uint8)


def rng0() -> np.random.Generator:
    return np.random.default_rng(SEED)


def action_dict(a) -> dict:
    from autocontrast.optimize.recipe import action_to_dict
    d = action_to_dict(a)
    d["key"] = a.key
    return d


def fp_dict(fp) -> dict:
    return fp.to_dict()


# --------------------------------------------------------------------------------------
# Synthetic imagery
# --------------------------------------------------------------------------------------

def smooth_noise(rng, h, w, sigma):
    n = ndimage.gaussian_filter(rng.standard_normal((h, w)), sigma=sigma, mode="wrap")
    return n / (np.abs(n).max() + 1e-12)


def make_nebula(rng, h=120, w=160, n_stars=40) -> np.ndarray:
    """A 8-bit 'render': dust lane, emission lobes, noise and stars. Returns uint8 (h, w, 3)."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    img = np.zeros((h, w, 3))
    img += np.array([0.035, 0.030, 0.040])
    img += (xx / w * 0.02)[..., None] * np.array([1.0, 0.8, 0.6])           # sky gradient
    blobs = [  # (cy, cx, sy, sx, amp, colour)
        (0.50 * h, 0.45 * w, 0.18 * h, 0.22 * w, 0.75, (1.00, 0.45, 0.35)),
        (0.35 * h, 0.65 * w, 0.14 * h, 0.15 * w, 0.55, (0.30, 0.75, 0.90)),
        (0.70 * h, 0.30 * w, 0.12 * h, 0.12 * w, 0.40, (0.95, 0.30, 0.55)),
        (0.50 * h, 0.45 * w, 0.05 * h, 0.06 * w, 0.80, (1.00, 0.90, 0.80)),
    ]
    for cy, cx, sy, sx, amp, col in blobs:
        g = np.exp(-(((yy - cy) / sy) ** 2 + ((xx - cx) / sx) ** 2) / 2.0)
        img += amp * g[..., None] * np.array(col)
    dust = smooth_noise(rng, h, w, 3.0)
    img *= (1.0 - 0.45 * np.clip(dust * 3.0 - 0.6, 0.0, 1.0))[..., None]     # dark filaments
    fine = smooth_noise(rng, h, w, 1.0)
    img *= (1.0 + 0.25 * fine)[..., None]                                    # fine texture
    for _ in range(n_stars):
        sy_, sx_ = rng.uniform(1.0, h - 1.0), rng.uniform(1.0, w - 1.0)
        s = rng.uniform(0.8, 1.7)
        amp = rng.uniform(0.25, 0.9)
        tint = np.array([1.0, rng.uniform(0.85, 1.0), rng.uniform(0.7, 1.0)])
        g = np.exp(-((yy - sy_) ** 2 + (xx - sx_) ** 2) / (2 * s * s))
        img += amp * g[..., None] * tint
    img += rng.standard_normal((h, w, 3)) * 0.008
    # Headroom on both ends: a render with pixels exactly at 0 or 1 fails the clipping guardrails
    # for EVERY candidate, which would make the loop goldens a test of nothing but those two checks.
    return to_u8(np.clip(img, 0.012, 0.97))


def gauss_blob(h, w, cy, cx, sy, sx):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    return np.exp(-(((yy - cy) / sy) ** 2 + ((xx - cx) / sx) ** 2) / 2.0)


# --------------------------------------------------------------------------------------
# Synthetic stages
# --------------------------------------------------------------------------------------

def stage_images(out: Out):
    """Images shared by several stages (written once, referenced by path from the JSON)."""
    from autocontrast.eval.degrade import flatten
    neb = make_nebula(rng0(), 120, 160)
    out.npy("images/nebula_u8.npy", neb)
    out.npy("images/flat06_f8.npy", flatten(u8_to_rgb(neb), 0.6))


def stage_npy_selftest(out: Out):
    out.npy("npy_selftest_f8.npy", np.arange(6, dtype="<f8").reshape(3, 2))
    out.npy("npy_selftest_b1.npy", np.array([[1, 0, 1], [0, 0, 1]], dtype=bool))
    out.npy("npy_selftest_f4.npy", np.array([0.5, -1.25, 3.0], dtype="<f4"))
    out.npy("npy_selftest_i8.npy", np.array([-3, 0, 7, 1000000], dtype="<i8"))
    out.npy("npy_selftest_u2.npy", np.array([0, 1, 65535], dtype="<u2"))
    out.npy("npy_selftest_u1.npy", np.array([0, 128, 255], dtype="|u1"))


def stage_primitives(out: Out):
    from autocontrast.fingerprint.metrics import QUANTILE_LEVELS
    from autocontrast.fingerprint.starlet import _B3_KERNEL, _dilate
    rng = rng0()
    levels = np.array(list(QUANTILE_LEVELS) + [0.0, 100.0, 33.3, 66.6667], dtype=np.float64)
    out.npy("prim/percentile_levels.npy", levels)

    # percentile / median ------------------------------------------------------------
    cases = {
        "p1001": rng.standard_normal(1001) * 20 + 50,
        "p1000": rng.standard_normal(1000) * 20 + 50,
        "pties": np.rint(rng.uniform(0, 8, 999)) / 8.0,
        "pn2": np.array([0.25, 0.75]),
        "pn5": rng.uniform(0, 1, 5),
        "pn1": np.array([0.4]),
    }
    for name, x in cases.items():
        out.npy(f"prim/percentile_{name}_in.npy", x)
        out.npy(f"prim/percentile_{name}_out.npy", np.percentile(x, levels))
        out.npy(f"prim/median_{name}_out.npy", np.median(x))
    # 2-D input (the metrics call np.percentile on whole images)
    img = rng.uniform(0, 100, (37, 29))
    out.npy("prim/percentile_img_in.npy", img)
    out.npy("prim/percentile_img_out.npy", np.percentile(img, levels))
    out.npy("prim/percentile_img25_out.npy", np.percentile(img, 25.0))

    # interp ---------------------------------------------------------------------------
    xp = np.sort(rng.uniform(0.1, 60.0, 8))
    fp = rng.uniform(0, 10, 8)
    xq = np.concatenate([[xp[0] - 1.0, xp[0], xp[-1], xp[-1] + 5.0, 0.0], xp, rng.uniform(0, 70, 40),
                         0.5 * (xp[:-1] + xp[1:])])
    out.npy("prim/interp_xp.npy", xp)
    out.npy("prim/interp_fp.npy", fp)
    out.npy("prim/interp_x.npy", xq)
    out.npy("prim/interp_out.npy", np.interp(xq, xp, fp))
    centers = 2.0 ** np.arange(8) * 0.05
    energy = np.sort(rng.uniform(1, 1e4, 8))[::-1]
    grid = 2.0 ** np.arange(2, 7) * 1.01
    out.npy("prim/interp_log_xp.npy", np.log(centers))
    out.npy("prim/interp_log_fp.npy", energy)
    out.npy("prim/interp_log_x.npy", np.log(grid))
    out.npy("prim/interp_log_out.npy", np.interp(np.log(grid), np.log(centers), energy))

    # histogram (1-D) ------------------------------------------------------------------
    edges = np.linspace(0.0, 360.0, 361)
    vals = np.concatenate([rng.uniform(0, 360, 5000), edges, edges[:-1] + 1e-12,
                           np.nextafter(edges[1:-1], 0.0), np.nextafter(edges[1:-1], 1e9),
                           [-1e-9, 360.0, 360.0 + 1e-9, -5.0, 400.0]])
    w = rng.uniform(0, 5, vals.size)
    out.npy("prim/hist_vals.npy", vals)
    out.npy("prim/hist_weights.npy", w)
    out.npy("prim/hist_edges_arr_counts.npy", np.histogram(vals, bins=edges)[0])
    out.npy("prim/hist_edges_arr_weighted.npy", np.histogram(vals, bins=edges, weights=w)[0])
    out.npy("prim/hist_int_counts.npy", np.histogram(vals, bins=360, range=(0.0, 360.0))[0])
    out.npy("prim/hist_int_weighted.npy", np.histogram(vals, bins=360, range=(0.0, 360.0), weights=w)[0])
    # a second, non-trivial range: 16 bins on [-100, 100]
    e16 = np.linspace(-100.0, 100.0, 17)
    v16 = np.concatenate([rng.uniform(-110, 110, 2000), e16, np.nextafter(e16, -1e9), np.nextafter(e16, 1e9)])
    out.npy("prim/hist16_vals.npy", v16)
    out.npy("prim/hist16_edges_arr_counts.npy", np.histogram(v16, bins=e16)[0])
    out.npy("prim/hist16_int_counts.npy", np.histogram(v16, bins=16, range=(-100.0, 100.0))[0])

    # histogram2d (weighted, as chroma_histogram) -------------------------------------
    a = np.concatenate([rng.uniform(-120, 120, 4000), e16, [100.0, -100.0, 100.0, 0.0, -100.0],
                        np.nextafter(e16, 1e9), np.nextafter(e16, -1e9)])
    b = np.concatenate([rng.uniform(-120, 120, 4000), e16[::-1], [100.0, -100.0, -100.0, 0.0, 100.0],
                        np.nextafter(e16, -1e9), np.nextafter(e16, 1e9)])
    assert a.size == b.size
    wt = np.hypot(a, b)
    out.npy("prim/hist2d_a.npy", a)
    out.npy("prim/hist2d_b.npy", b)
    h2, _, _ = np.histogram2d(a, b, bins=[e16, e16], weights=wt)
    out.npy("prim/hist2d_weighted.npy", h2)
    h2u, _, _ = np.histogram2d(a, b, bins=[e16, e16])
    out.npy("prim/hist2d_unweighted.npy", h2u)

    # convolve1d mirror --------------------------------------------------------------
    k_by_w = {w_: _dilate(_B3_KERNEL, s) for w_, s in ((5, 0), (9, 1), (17, 2), (257, 6))}
    for w_, k in k_by_w.items():
        assert k.size == w_
        out.npy(f"prim/conv_kernel_{w_}.npy", k)
    kasym = rng.uniform(-1, 1, 9)
    out.npy("prim/conv_kernel_asym9.npy", kasym)
    k_by_w["asym9"] = kasym
    for (h, w_) in ((7, 5), (64, 48)):
        x = rng.uniform(-1, 1, (h, w_))
        out.npy(f"prim/conv_in_{h}x{w_}.npy", x)
        for name, k in k_by_w.items():
            for axis in (0, 1):
                out.npy(f"prim/conv_{h}x{w_}_k{name}_axis{axis}.npy",
                        ndimage.convolve1d(x, k, axis=axis, mode="mirror"))

    # gaussian_filter ----------------------------------------------------------------
    for (h, w_) in ((7, 5), (64, 48)):
        x = np.asarray(out_load_conv(out, h, w_))
        for sig in (0.5, 1.0, 2.355, 3.0, 8.0):
            out.npy(f"prim/gauss_{h}x{w_}_s{sig}.npy", ndimage.gaussian_filter(x, sigma=sig))

    # label + find_objects ----------------------------------------------------------
    for name, p, (h, w_) in (("dense", 0.45, (64, 48)), ("sparse", 0.12, (64, 48)), ("tiny", 0.5, (7, 5))):
        mask = rng.uniform(0, 1, (h, w_)) < p
        lab, n = ndimage.label(mask)
        objs = ndimage.find_objects(lab)
        boxes = np.array([[s[0].start, s[0].stop, s[1].start, s[1].stop] for s in objs],
                         dtype=np.int64).reshape(-1, 4)
        out.npy(f"prim/label_{name}_mask.npy", mask)
        out.npy(f"prim/label_{name}_labels.npy", lab.astype(np.int64))
        out.npy(f"prim/label_{name}_boxes.npy", boxes)
        out.json(f"prim/label_{name}.json", {"n": int(n)})


def out_load_conv(out, h, w_):
    return np.load(out.root / f"prim/conv_in_{h}x{w_}.npy")


def _test_image(rng, h, w):
    """A positive structured float image for starlet/energy tests."""
    base = smooth_noise(rng, h, w, 4.0) * 0.5 + 0.5
    fine = rng.standard_normal((h, w)) * 0.05
    pts = np.zeros((h, w))
    for _ in range(max(2, h * w // 400)):
        pts += rng.uniform(0.2, 1.0) * gauss_blob(h, w, rng.uniform(0, h), rng.uniform(0, w), 1.2, 1.2)
    return base + fine + pts


def stage_starlet_energy_bandlimit(out: Out):
    from autocontrast.fingerprint.starlet import starlet_transform
    from autocontrast.fingerprint.energy import energy_spectrum_from_planes, energy_spectrum, EnergySpectrum
    from autocontrast.fingerprint.bandlimit import band_limit, comparable_band
    rng = rng0()
    meta = []
    kept = {}
    for (h, w) in ((96, 80), (64, 48), (7, 5)):
        img = _test_image(rng, h, w)
        out.npy(f"starlet/img_{h}x{w}.npy", img)
        for n in (1, 5, 7):
            planes, resid = starlet_transform(img, n)
            out.npy(f"starlet/planes_{h}x{w}_n{n}.npy", planes)
            out.npy(f"starlet/residual_{h}x{w}_n{n}.npy", resid)
            recon = planes.sum(axis=0) + resid
            meta.append({"h": h, "w": w, "n": n, "max_recon_err": float(np.abs(recon - img).max())})
            kept[(h, w, n)] = planes
    out.json("starlet/index.json", {"cases": meta})

    # energy ------------------------------------------------------------------------------
    ecases = []
    for (h, w, n), px in (((96, 80, 5), 1.0), ((96, 80, 7), 0.05), ((64, 48, 7), 2.5), ((7, 5, 1), 1.01)):
        planes = kept[(h, w, n)]
        spec = energy_spectrum_from_planes(planes, px)
        ecases.append({"h": h, "w": w, "n": n, "pixel_scale": px,
                       "centers": spec.centers_arcsec, "energy": spec.energy})
        spec2 = energy_spectrum(np.load(out.root / f"starlet/img_{h}x{w}.npy"), px, n)
        assert np.array_equal(spec.energy, spec2.energy) and np.array_equal(spec.centers_arcsec, spec2.centers_arcsec)
    out.json("energy/cases.json", {"cases": ecases})

    # band_limit -------------------------------------------------------------------------
    def spec(n, px, e):
        return EnergySpectrum(centers_arcsec=2.0 ** np.arange(n) * px, energy=np.asarray(e, dtype=np.float64))

    def decaying(n, scale):
        return np.sort(rng.uniform(1, 100, n))[::-1] * scale

    bl_cases = [
        # (name, ref spectrum, target spectrum, ref_psf, target_psf, min_bins)
        ("usable", spec(8, 0.05, decaying(8, 1e3)), spec(7, 1.01, decaying(7, 5e2)), 0.15, 2.4, 3),
        ("usable_same_scale", spec(7, 1.0, decaying(7, 1e3)), spec(7, 1.0, decaying(7, 1e3)), 2.0, 2.0, 3),
        ("hi_le_lo", spec(6, 1.0, decaying(6, 1e3)), spec(6, 1.0, decaying(6, 1e3)), 1000.0, 2.0, 3),
        ("hi_eq_lo", spec(6, 1.0, decaying(6, 1e3)), spec(6, 1.0, decaying(6, 1e3)), 16.0, 2.0, 3),
        ("lt_min_bins", spec(7, 1.0, decaying(7, 1e3)), spec(7, 1.0, decaying(7, 1e3)), 16.0, 2.0, 3),
        ("lt_min_bins_custom", spec(7, 1.0, decaying(7, 1e3)), spec(7, 1.0, decaying(7, 1e3)), 4.0, 2.0, 5),
        ("min_bins_1", spec(7, 1.0, decaying(7, 1e3)), spec(7, 1.0, decaying(7, 1e3)), 16.0, 2.0, 1),
        ("short_spectrum", spec(3, 1.0, decaying(3, 1e3)), spec(3, 1.0, decaying(3, 1e3)), 0.5, 0.5, 3),
        ("zero_energy_target", spec(7, 1.0, decaying(7, 1e3)), spec(7, 1.0, np.zeros(7)), 2.0, 2.0, 3),
        ("coarse_ref_fine_target", spec(6, 4.0, decaying(6, 1e3)), spec(8, 0.25, decaying(8, 1e3)), 6.0, 0.6, 3),
    ]
    res = []
    for name, ref, tgt, rp, tp, mb in bl_cases:
        r = band_limit(ref, tgt, rp, tp, min_bins=mb)
        lo, hi = comparable_band(ref, tgt, rp, tp)
        res.append({
            "name": name, "min_bins": mb, "ref_psf": rp, "target_psf": tp,
            "ref": {"centers": ref.centers_arcsec, "energy": ref.energy},
            "target": {"centers": tgt.centers_arcsec, "energy": tgt.energy},
            "comparable_band": [lo, hi],
            "result": {"lo": r.lo_arcsec, "hi": r.hi_arcsec, "usable": r.usable, "reason": r.reason,
                       "grid": r.grid_arcsec, "ref_shape": r.ref_shape, "target_shape": r.target_shape},
        })
    out.json("bandlimit/cases.json", {"cases": res})


def stage_color_metrics(out: Out):
    from autocontrast.fingerprint.color import rgb_to_lab
    from autocontrast.fingerprint import metrics as M
    rng = rng0()
    # rgb_to_lab -------------------------------------------------------------------------------
    gray = np.arange(256, dtype=np.uint8)
    ramp = np.stack([gray, gray, gray], axis=-1).reshape(16, 16, 3)
    out.npy("color/ramp_u8.npy", ramp)
    out.npy("color/ramp_lab.npy", rgb_to_lab(u8_to_rgb(ramp)))
    knee = np.array([0.0, 0.04045, np.nextafter(0.04045, 1.0), np.nextafter(0.04045, 0.0),
                     0.04, 0.05, 1.0, 0.5, 0.0031308, 0.008, 0.0089, 0.2])
    rnd = np.concatenate([rng.uniform(0, 1, 90), (rng.uniform(0, 1, 90) ** 4), knee, knee[::-1]])
    rnd = rnd[: (rnd.size // 18) * 18].reshape(-1, 6, 3)
    out.npy("color/rand_rgb.npy", rnd)
    out.npy("color/rand_lab.npy", rgb_to_lab(rnd))
    sat = np.array([[[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 0], [0, 1, 1], [1, 0, 1], [1, 1, 1], [0, 0, 0]]],
                   dtype=np.float64)
    out.npy("color/primaries_rgb.npy", sat)
    out.npy("color/primaries_lab.npy", rgb_to_lab(sat))

    # metrics ----------------------------------------------------------------------------------------
    neb = make_nebula(rng, 120, 160)[30:90, 40:120]       # 60x80 crop keeps the metrics goldens small
    out.npy("metrics/nebula_crop_u8.npy", neb)
    rgb = u8_to_rgb(neb)
    lab = rgb_to_lab(rgb)
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    chroma = np.hypot(a, b)
    out.npy("metrics/nebula_crop_lab.npy", lab)
    out.npy("metrics/tonal_quantiles.npy", M.tonal_quantiles(L))
    out.npy("metrics/saturation_quantiles.npy", M.saturation_quantiles(chroma))
    out.npy("metrics/chroma_hist.npy", M.chroma_histogram(a, b))
    out.npy("metrics/chroma_hist_bins8_extent60.npy", M.chroma_histogram(a, b, bins=8, extent=60.0))
    out.npy("metrics/chroma_hist_neutral.npy", M.chroma_histogram(np.zeros((8, 8)), np.zeros((8, 8))))
    out.npy("metrics/balance.npy", M.background_channel_balance(rgb))
    out.npy("metrics/balance_p10.npy", M.background_channel_balance(rgb, percentile=10.0))
    scalars = {"floor_to_peak_ratio": M.floor_to_peak_ratio(L)}
    from autocontrast.fingerprint.starlet import starlet_transform
    for n in (3, 5, 7):
        planes, resid = starlet_transform(L, n)
        scalars[f"structure_to_gradient_ratio_n{n}"] = M.structure_to_gradient_ratio(L, n_scales=n)
        scalars[f"structure_to_gradient_from_transform_n{n}"] = M.structure_to_gradient_ratio_from_transform(planes, resid)
    # edge cases
    scalars["floor_to_peak_zero_peak"] = M.floor_to_peak_ratio(np.zeros((6, 6)))
    scalars["floor_to_peak_constant"] = M.floor_to_peak_ratio(np.full((6, 6), 37.5))
    const = np.full((16, 16), 50.0)
    scalars["structure_gradient_constant"] = M.structure_to_gradient_ratio(const, n_scales=3)
    out.npy("metrics/constant_L.npy", const)
    zeros = np.zeros((16, 16))
    scalars["structure_gradient_zero"] = M.structure_to_gradient_ratio(zeros, n_scales=3)
    out.npy("metrics/zeros_L.npy", zeros)
    tiny = rng.uniform(0, 100, (7, 5))
    out.npy("metrics/tiny_L.npy", tiny)
    scalars["structure_gradient_tiny_n3"] = M.structure_to_gradient_ratio(tiny, n_scales=3)
    black = np.zeros((10, 10, 3))
    out.npy("metrics/black_rgb.npy", black)
    out.npy("metrics/balance_black.npy", M.background_channel_balance(black))
    out.json("metrics/scalars.json", scalars)


def stage_extract(out: Out):
    from autocontrast.fingerprint.extract import extract
    from autocontrast.eval.degrade import flatten
    rng = rng0()
    neb = make_nebula(rng, 120, 160)
    nebf = u8_to_rgb(neb)
    flat = flatten(nebf, 0.6)
    gray = np.full((40, 40, 3), 117, np.uint8)
    cast = make_nebula(rng, 48, 64, n_stars=8)
    cast = to_u8(u8_to_rgb(cast) * np.array([1.0, 0.7, 0.4]))
    tiny = rng.integers(0, 256, (7, 5, 3), dtype=np.uint8)
    noise = rng.integers(0, 256, (64, 48, 3), dtype=np.uint8)
    out.npy("extract/gray_u8.npy", gray)
    out.npy("extract/cast_u8.npy", cast)
    out.npy("extract/tiny_u8.npy", tiny)
    out.npy("extract/noise_u8.npy", noise)
    cases = [
        ("nebula", nebf, dict(pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0, palette_class="RGB")),
        ("nebula_fine", nebf, dict(pixel_scale_arcsec=0.05, n_scales=7, psf_fwhm_arcsec=0.15, palette_class="HaRGB")),
        ("flat06", flat, dict(pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0, palette_class="RGB")),
        ("gray", u8_to_rgb(gray), dict(pixel_scale_arcsec=1.5, n_scales=3, psf_fwhm_arcsec=3.0, palette_class="L-only")),
        ("cast", u8_to_rgb(cast), dict(pixel_scale_arcsec=1.0, n_scales=5, psf_fwhm_arcsec=2.0, palette_class="HOO")),
        ("tiny", u8_to_rgb(tiny), dict(pixel_scale_arcsec=1.0, n_scales=3, psf_fwhm_arcsec=1.0, palette_class="unknown")),
        ("noise", u8_to_rgb(noise), dict(pixel_scale_arcsec=1.0, n_scales=4, psf_fwhm_arcsec=2.0, palette_class="SHO")),
    ]
    src = {"nebula": "images/nebula_u8.npy", "nebula_fine": "images/nebula_u8.npy",
           "flat06": "images/flat06_f8.npy", "gray": "extract/gray_u8.npy", "cast": "extract/cast_u8.npy",
           "tiny": "extract/tiny_u8.npy", "noise": "extract/noise_u8.npy"}
    for name, rgb, kw in cases:
        fp = extract(rgb, **kw)
        out.json(f"extract/fingerprint_{name}.json",
                 {"input": src[name], "params": kw, "fingerprint": fp_dict(fp)})


def _fp_set(rng):
    """Named FingerprintData instances shared by the distance / proposer stages."""
    from autocontrast.fingerprint.extract import extract
    from autocontrast.eval import degrade as D
    neb = u8_to_rgb(make_nebula(rng, 120, 160))
    kw = dict(pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0, palette_class="RGB")
    fps = {"ref": extract(neb, **kw)}
    variants = {
        "flat03": D.flatten(neb, 0.3), "flat06": D.flatten(neb, 0.6), "flat10": D.flatten(neb, 1.0),
        "sharp05": D.over_sharpen(neb, 0.5), "desat05": D.desaturate(neb, 0.5),
        "oversat05": D.oversaturate(neb, 0.5),
    }
    for k, v in variants.items():
        fps[k] = extract(v, **kw)
    fine = extract(neb, pixel_scale_arcsec=0.05, n_scales=7, psf_fwhm_arcsec=0.15, palette_class="RGB")
    fps["ref_fine"] = fine
    fps["flat06_fine"] = extract(D.flatten(neb, 0.6), pixel_scale_arcsec=0.05, n_scales=7,
                                 psf_fwhm_arcsec=0.15, palette_class="RGB")
    return fps


def stage_distance(out: Out):
    from autocontrast.fingerprint import distance as DI
    from autocontrast.fingerprint.extract import FingerprintData
    from autocontrast.fingerprint.palette import palette_chroma_compatible
    rng = rng0()

    # chroma EMD pairs -------------------------------------------------------------------------------
    pairs, results = [], []
    for i in range(20):
        mats = []
        for j in range(2):
            h = rng.uniform(0, 1, (16, 16)) ** rng.choice([1.0, 3.0])
            if i >= 10:                                    # sparse: 90 % of bins zero
                h = h * (rng.uniform(0, 1, (16, 16)) < 0.10)
            if i == 18 and j == 1:
                h = np.zeros((16, 16))                     # neutral target
            if i == 19 and j == 0:
                h = np.zeros((16, 16))                     # neutral reference
            s = h.sum()
            mats.append(h / s if s > 0 else h)
        pairs.append(np.stack(mats))
    pairs_arr = np.stack(pairs)
    out.npy("distance/emd_pairs.npy", pairs_arr)
    edges = np.linspace(-100.0, 100.0, 17)
    centers = 0.5 * (edges[:-1] + edges[1:])
    ga, gb = np.meshgrid(centers, centers, indexing="ij")
    coords = np.column_stack([ga.ravel(), gb.ravel()])
    raw, norm = [], []
    Stub = type("Stub", (), {})
    for p in pairs_arr:
        r, t = Stub(), Stub()
        r.chroma_hist, t.chroma_hist = p[0], p[1]
        norm.append(DI._chroma_distance(r, t))
        if p[0].sum() == 0 or p[1].sum() == 0:
            raw.append(0.0)
        else:
            raw.append(float(wasserstein_distance_nd(coords, coords, p[0].ravel(), p[1].ravel())))
    out.npy("distance/emd_raw.npy", np.array(raw))
    out.npy("distance/emd_normalized.npy", np.array(norm))

    # fingerprints + fingerprint_distance --------------------------------------------------------------
    fps = _fp_set(rng)
    for name, fp in fps.items():
        out.json(f"fingerprints/{name}.json", fp_dict(fp))
    import dataclasses as dc
    rep = dc.replace
    fp_extra = {
        "ref_hoo": rep(fps["ref"], palette_class="HOO"),
        "ref_sho": rep(fps["ref"], palette_class="SHO"),
        "ref_lonly": rep(fps["ref"], palette_class="L-only"),
        "ref_unknown": rep(fps["ref"], palette_class="unknown"),
        "ref_hargb": rep(fps["ref"], palette_class="HaRGB"),
        "ref_bigpsf": rep(fps["ref"], psf_fwhm_arcsec=500.0),
        "flat06_psf20": rep(fps["flat06"], psf_fwhm_arcsec=20.0),
    }
    for name, fp in fp_extra.items():
        out.json(f"fingerprints/{name}.json", fp_dict(fp))
    allfp = {**fps, **fp_extra}
    plan = [
        ("ref", "ref"), ("ref", "flat03"), ("ref", "flat06"), ("ref", "flat10"), ("ref", "sharp05"),
        ("ref", "desat05"), ("ref", "oversat05"), ("ref_fine", "flat06_fine"), ("ref_fine", "flat06"),
        ("ref", "flat06_fine"),
        ("ref_hoo", "flat06"), ("ref_sho", "ref_hoo"), ("ref_lonly", "flat06"), ("ref_unknown", "ref_unknown"),
        ("ref_hargb", "flat06"), ("ref_bigpsf", "flat06"), ("ref", "flat06_psf20"), ("ref_sho", "ref_sho"),
        ("flat06", "ref"), ("sharp05", "oversat05"),
    ]
    weights_variants = [None, dict(spectrum=0.1, tonal=0.2, chroma=0.3, background=0.4),
                        dict(spectrum=1.0, tonal=0.0, chroma=0.0, background=0.0)]
    cases = []
    for r, t in plan:
        ref, tgt = allfp[r], allfp[t]
        sd, usable = DI._spectrum_distance(ref, tgt)
        comp = dict(spectrum=sd, tonal=DI._tonal_distance(ref, tgt), chroma=DI._chroma_distance(ref, tgt),
                    background=DI._background_distance(ref, tgt))
        bg = DI._background_distance(ref, tgt)
        match = palette_chroma_compatible(ref.palette_class, tgt.palette_class)
        entry = {"ref": r, "target": t, "spectrum_usable": usable, "palette_match": match,
                 "components": comp, "d": DI.fingerprint_distance(ref, tgt), "background": bg}
        wres = []
        for wv in weights_variants[1:]:
            W = DI.DistanceWeights(**wv)
            try:
                wres.append({"weights": wv, "d": DI.fingerprint_distance(ref, tgt, W), "error": None})
            except ZeroDivisionError as e:      # all surviving weights are 0: Python divides by zero
                wres.append({"weights": wv, "d": None, "error": "ZeroDivisionError"})
        entry["weighted"] = wres
        cases.append(entry)
    out.json("distance/fingerprint_distance.json", {"cases": cases})

    # combine_distance direct -----------------------------------------------------------------------------
    comb = []
    for comps, match, wv in (((0.2, 0.3, 0.4, 0.1), True, None), ((0.2, 0.3, 0.4, 0.1), False, None),
                             ((0.5, 0.5, 0.5, 0.5), True, None), ((0.9, 0.1, 0.8, 0.2), False,
                                                                  dict(spectrum=0.0, tonal=0.5, chroma=0.5, background=0.0)),
                             ((0.0, 0.0, 0.0, 0.0), True, None), ((1.0, 1.0, 1.0, 1.0), False, None)):
        c = DI.DistanceComponents(*comps)
        W = DI.DistanceWeights(**wv) if wv else DI.DistanceWeights()
        comb.append({"components": comps, "palette_match": match, "weights": wv,
                     "d": DI.combine_distance(c, W, palette_match=match)})
    out.json("distance/combine_distance.json", {"cases": comb})

    # palette compatibility matrix + background sub-terms ------------------------------------------------------
    from autocontrast.fingerprint.palette import PALETTE_CLASSES
    classes = sorted(PALETTE_CLASSES)
    out.json("distance/palette_compat.json",
             {"classes": classes, "matrix": [[palette_chroma_compatible(a, b) for b in classes] for a in classes]})
    sq = [0.0, 0.5, 1.0, 3.0, 1e6, float("inf")]
    out.json("distance/squash.json", {"x": sq, "y": [DI._squash(x) for x in sq]})


def stage_actions_propose(out: Out):
    from autocontrast.optimize import actions as A
    from autocontrast.optimize import propose as P
    from autocontrast.optimize.recipe import Recipe, PROCESS_FOR_KIND
    from autocontrast.optimize.beam import BeamConfig
    from autocontrast.fingerprint.extract import FingerprintData
    import dataclasses as dc
    rng = rng0()

    # constants --------------------------------------------------------------------------------------------
    out.json("actions/constants.json", {
        "LEVELS": list(A.LEVELS), "ONCE_ONLY": A.ONCE_ONLY, "STRENGTH": A._STRENGTH,
        "PROCESS_FOR_KIND": PROCESS_FOR_KIND, "GAP_GENTLE_MAX": P.GAP_GENTLE_MAX,
        "GAP_MODERATE_MAX": P.GAP_MODERATE_MAX, "REMEDIES": {k: list(v) for k, v in P._REMEDIES.items()},
        "SCALE_KINDS": P._SCALE_KINDS,
        "BeamConfig": dc.asdict(BeamConfig()), "BeamConfig_top_k_5": dc.asdict(BeamConfig(top_k=5)),
        "BeamConfig_explicit_max_attempts": dc.asdict(BeamConfig(max_attempts=4)),
    })

    # Action.key / strength / layer maths ---------------------------------------------------------------------------
    scales = [None, 0.0, 0.0005, 0.0015, 0.0025, 1.0005, 1.0015, 2.0, 2.5, 1.0 / 3.0, 7.5e-4, 16.0, 0.12345678,
              1e-6, 123456.78915, 0.9995, 4.0005, 2.0 ** 0.5, 1.005, 1.015, 1.025]
    keyrows = []
    for s in scales:
        for lvl in ("gentle", "", "strong"):
            a = A.Action("local_contrast", lvl, s)
            keyrows.append({"scale": s, "level": lvl, "key": a.key})
    strengths = []
    for lvl in ("gentle", "moderate", "strong", ""):
        strengths.append({"level": lvl, "strength": A.Action("x", lvl, None).strength})
    try:
        A.Action("x", "bogus", None).strength
        raise AssertionError("strength must reject unknown level")
    except ValueError as e:
        bad_msg = str(e)
    lay = []
    for px in (1.0, 0.05, 1.01, 0.4):
        for ratio in (1.0, 1.4, 1.41, 2 ** 0.5, 1.5, 2.0, 2.9, 3.0, 5.65, 8.0, 11.3, 16.0, 45.0, 64.0):
            sc = ratio * px
            lay.append({"px": px, "scale": sc, "layer": A.layer_for_scale(sc, px)})
    below = None
    try:
        A.layer_for_scale(0.5, 1.0)
    except ValueError as e:
        below = str(e)
    out.json("actions/key_strength_layer.json", {
        "keys": keyrows, "strengths": strengths, "bad_level_message": bad_msg,
        "layer_for_scale": lay, "layer_below_pixel_message": below,
        "scale_for_layer": [{"layer": i, "px": px, "scale": A.scale_for_layer(i, px)}
                            for i in range(9) for px in (1.0, 0.05, 1.01)],
        "level_for_gap": [{"gap": g, "level": P._level_for_gap(g)}
                          for g in (-1.0, 0.0, 0.07, 0.0749999, 0.075, 0.0750001, 0.12, 0.1499999, 0.15, 0.2, 9.0)],
    })

    # available_actions ---------------------------------------------------------------------------------------------------
    avail = []
    for px, psf, n, comp, applied in (
            (1.0, 2.0, 6, True, []), (1.0, 2.0, 6, False, []), (1.0, 2.0, 6, True, ["star_split"]),
            (1.0, 2.0, 6, True, ["star_split", "background_neutralize"]), (0.05, 0.15, 7, True, []),
            (1.0, 1000.0, 6, True, []), (1.01, 2.4, 5, True, ["local_contrast"]), (2.0, 0.5, 1, True, []),
            (0.5, 1.0, 8, False, ["background_neutralize"])):
        acts = A.available_actions(pixel_scale_arcsec=px, psf_fwhm_arcsec=psf, n_scales=n,
                                   palette_compatible=comp, applied_kinds=frozenset(applied))
        avail.append({"pixel_scale": px, "psf": psf, "n_scales": n, "palette_compatible": comp,
                      "applied_kinds": applied, "actions": [action_dict(a) for a in acts]})
    out.json("actions/available_actions.json", {"cases": avail})

    # propose_actions: 12 (ref, target, applied_kinds) cases --------------------------------------------------------------------
    fps = _fp_set(rng)
    rep = dc.replace
    fps["ref_hoo"] = rep(fps["ref"], palette_class="HOO")
    fps["ref_lonly"] = rep(fps["ref"], palette_class="L-only")
    fps["flat06_psf20"] = rep(fps["flat06"], psf_fwhm_arcsec=20.0)
    fps["ref_bigpsf"] = rep(fps["ref"], psf_fwhm_arcsec=500.0)
    for name, fp in fps.items():
        out.json(f"fingerprints/{name}.json", fp_dict(fp))     # same bytes as the distance stage for shared names
    plan = [
        ("ref", "flat06", []), ("ref", "flat03", []), ("ref", "flat10", ["star_split"]),
        ("ref", "sharp05", []), ("ref", "desat05", ["background_neutralize"]),
        ("ref", "oversat05", ["star_split", "background_neutralize"]),
        ("ref", "ref", []), ("ref_hoo", "flat06", []), ("ref_lonly", "flat06", ["local_contrast"]),
        ("ref_fine", "flat06_fine", []), ("ref_bigpsf", "flat06", []), ("ref", "flat06_psf20", ["core_hdr"]),
    ]
    cases = []
    for r, t, applied in plan:
        ref, tgt = fps[r], fps[t]
        n_scales = len(tgt.energy.energy)
        gaps = P.component_gaps(ref, tgt)
        deficits = P._band_deficits(ref, tgt)
        entry = {"ref": r, "target": t, "applied_kinds": applied, "n_scales": n_scales,
                 "component_gaps": gaps, "band_deficits": deficits}
        for label, k in (("top3", 3), ("all", None), ("top1", 1), ("top7", 7)):
            ps = P.propose_actions(ref, tgt, applied_kinds=frozenset(applied), n_scales=n_scales, top_k=k)
            entry[label] = [action_dict(a) for a in ps]
        cases.append(entry)
    out.json("propose/cases.json", {"cases": cases})

    # recipe ---------------------------------------------------------------------------------------------------------------------------
    acts = [A.Action("local_contrast", "moderate", 4.0, {"layer": 2}),
            A.Action("chroma", "gentle", None, {}),
            A.Action("star_split", "", None, {}),
            A.Action("local_equalize", "strong", 16.0, {"radius_arcsec": 16.0}),
            A.Action("core_hdr", "moderate", None, {"layers": 3}),
            A.Action("black_point", "gentle", None, {"clip_limited": True}),
            A.Action("tonal_reshape", "strong", None, {"monotone": True}),
            A.Action("background_neutralize", "", None, {})]
    r = Recipe.empty()
    snaps = [{"key": r.key, "applied_kinds": r.applied_kinds, "dict": r.to_dict(), "steps": r.to_pixinsight_steps()}]
    for a in acts:
        r = r.extend(a)
        snaps.append({"key": r.key, "applied_kinds": r.applied_kinds, "dict": r.to_dict(),
                      "steps": r.to_pixinsight_steps()})
    try:
        Recipe((A.Action("nonsense", "gentle", None),)).to_pixinsight_steps()
        raise AssertionError("unmapped kind must raise")
    except ValueError as e:
        unmapped = str(e)
    out.json("recipe/recipes.json", {"snapshots": snaps, "unmapped_kind_message": unmapped})


def stage_degrade(out: Out):
    from autocontrast.eval import degrade as D
    rng = rng0()
    img = u8_to_rgb(make_nebula(rng, 40, 48, n_stars=10))
    out.npy("degrade/in_u8.npy", to_u8(img))
    img = u8_to_rgb(to_u8(img))
    for name, fn in (("flatten", D.flatten), ("over_sharpen", D.over_sharpen),
                     ("desaturate", D.desaturate), ("oversaturate", D.oversaturate)):
        for s in (0.3, 0.6, 1.0):
            r = fn(img, s)
            out.npy(f"degrade/{name}_s{s}.npy", r)
    out.npy("degrade/flatten_sigma5_s05.npy", D.flatten(img, 0.5, blur_sigma=5.0))
    out.npy("degrade/over_sharpen_sigma3_s05.npy", D.over_sharpen(img, 0.5, blur_sigma=3.0))


def stage_palette(out: Out):
    from autocontrast.fingerprint import palette as PA
    rows = [
        [], ["Ha"], ["ha"], [" Ha "], ["H-alpha"], ["OIII"], ["O3"], ["SII", "Ha", "OIII"], ["S2", "H-Alpha", "O3"],
        ["Ha", "OIII"], ["HaO3"], ["L-Extreme"], ["L-eNhance"], ["Duo-Band"], ["R", "G", "B"], ["Red", "Green", "Blue"],
        ["L", "R", "G", "B"], ["Lum", "r", "g", "b"], ["Ha", "R", "G", "B"], ["Ha", "OIII", "R", "G", "B"],
        ["L"], ["Luminance"], ["R", "G"], ["UV/IR Cut"], ["Ha", "UV/IR Cut"], ["SII", "Ha"], ["SII"], ["dual-band", "R", "G", "B"],
        ["L", "Ha"], ["ALP-T"], ["L-Ultimate", "L"], ["", "Ha"], [" "], ["Ha", "Ha"], ["R", "R", "G", "B", "B"],
        ["SII", "Ha", "OIII", "L"], ["SII", "Ha", "OIII", "R"], ["h_alpha", "o3", "s2"], ["ALPT", "G"], ["B"],
    ]
    assert len(rows) == 40
    out.json("palette/from_filters.json", {"rows": [{"filters": r, "class": PA.palette_class_from_filters(r)} for r in rows]})
    grows = [
        [], ["B", "V", "H-alpha", "I", "Z"], ["B", "V"], ["V"], ["R"], ["g", "r", "i"], ["Ha"], ["H-alpha", "[O III]", "[S II]"],
        ["Ha", "OIII"], ["[OIII]", "H_ALPHA"], ["S-II", "O-III", "Ha"], ["SII", "OIII"], ["U", "B", "V", "R", "I"],
        ["J", "H", "K"], ["Y", "J"], ["", "B"], [" ", " "], ["B", "B"], ["W"], ["Ha", "B"], ["Ha", "B", "V"],
        ["O3", "HA"], ["S2", "HA", "O3"], ["z", "i"], ["L"], ["M", "L"], ["Ks", "H"], ["R", "Ha", "OIII"],
        ["F814W", "F606W"], ["I", "Z", "H-alpha", "V", "-", "B"], ["optical", "B"], ["Ha", "Ha", "OIII"], ["OIII"],
        ["[S II]", "[O III]", "H-alpha", "V"], ["g", "Ha"], ["U"], ["u", "g", "r", "i", "z"], ["Hα"], ["SII", "Ha", "OIII", "R", "G"],
        ["V", "-"],
    ]
    assert len(grows) == 40
    out.json("palette/from_gallery_bands.json",
             {"rows": [{"bands": r, "class": PA.palette_class_from_gallery_bands(r)} for r in grows]})


def stage_gallery(out: Out):
    from autocontrast.db.discover import gallery as G
    fx = AC_ROOT / "tests" / "fixtures" / "gallery"
    spec = {  # fixture -> (kind, id, gallery)
        "hubble_detail_heic0211i.html": ("detail", "heic0211i", "esa_hubble"),
        "hubble_detail_heic0601a.html": ("detail", "heic0601a", "esa_hubble"),
        "hubble_detail_opo0205c.html": ("detail", "opo0205c", "esa_hubble"),
        "eso_detail_eso1103a.html": ("detail", "eso1103a", "eso"),
        "hubble_listing_orion.html": ("listing", None, "esa_hubble"),
        "eso_listing_orion.html": ("listing", None, "eso"),
    }
    present = sorted(p.name for p in fx.glob("*.html"))
    assert present == sorted(spec), f"gallery fixtures changed: {present}"
    for fname, (kind, eid, gal) in spec.items():
        data = (fx / fname).read_bytes()
        doc = data.decode("utf-8")
        rec = {"fixture": f"tools/autocontrast/tests/fixtures/gallery/{fname}", "sha256": hashlib.sha256(data).hexdigest(),
               "bytes": len(data), "kind": kind, "gallery": gal}
        lines = G.text_lines(doc)
        rec["text_lines_count"] = len(lines)
        rec["text_lines_head"] = lines[:25]
        if kind == "detail":
            cfg = G.GALLERIES[gal]
            entry = G.parse_detail(doc, entry_id=eid, gallery=gal, detail_url=G.detail_url(cfg, eid))
            rec["entry_id"] = eid
            rec["entry"] = entry
            rec["has_position"] = entry.has_position
            rec["credit"] = G.parse_credit(doc)
            rec["asserts_copyright"] = G.asserts_copyright(rec["credit"])
            rec["filter_bands"] = G.parse_filter_bands(doc)
            rec["size_px"] = G.parse_size_px(lines)
            rec["image_url"] = G._image_url(doc)
            for lab in ("Id", "Name", "Category", "Type", "Position (RA)", "Position (Dec)", "Field of view",
                        "Size", "Release date"):
                rec.setdefault("labels", {})[lab] = G.labelled_value(lines, lab)
            # with the listing's dimensions supplied
            e2 = G.parse_detail(doc, entry_id=eid, gallery=gal, detail_url="x", width_px=1000, height_px=500)
            rec["entry_with_dims"] = e2
        else:
            items = G.parse_listing(doc)
            rec["items"] = items
            rec["result_total"] = G.parse_result_total(doc)
        out.json(f"gallery/{fname[:-5]}.json", rec)

    # table-driven unit goldens (the synthetic edge cases of spec 6.11)
    ras = ["5 35 9.73", "0 0 0", "23 59 59.999", "5h 35m 9.73s", "05:35:09.73", "5 35", "", "12 30 0", "x"]
    decs = ["-5° 24' 50.32\"", "+41° 16' 9\"", "-0° 30' 0\"", "0° 0' 0\"", "−0° 30' 15.5\"", "-90° 0' 0\"",
            "12° 5'", "", "-5 24 50.32", " -1° 2' 3\"  ", "−10° 20' 30\""]
    fovs = ["30.03 x 30.03 arcminutes", "30 × 20 arcminutes", "5.5 x 4.2 arcseconds", "1.5 x 1.5 degrees", "12 x 8 arcsec",
            "10 x 10 furlongs", "", "abc", "30.03x30.03 arcmin", "  7 x 7  deg"]
    dates = ["11 January 2006, 16:00", "5 March 2010", "31 December 1999, 23:59", "1 Foo 2000", "", "9 May 2017, 7:05",
             "11 january 2006, 16:00"]
    sizes = [["Size", "18000 x 18000 px"], ["Size", "1600×1200"], ["Size"], ["Foo", "Bar"], ["size:", "640 x 480 px"]]
    labs = [(["A", "Id:", "x1", "Name", "y"], "Id"), (["id", "z"], "Id"), (["Id"], "Id"), (["Name:", "n"], "Name"), ([], "Id")]
    out.json("gallery/units.json", {
        "ra": [{"in": s, "out": G.parse_ra_sexagesimal(s)} for s in ras],
        "dec": [{"in": s, "out": G.parse_dec_sexagesimal(s)} for s in decs],
        "fov": [{"in": s, "out": G.parse_fov_arcmin(s)} for s in fovs],
        "date": [{"in": s, "out": G.parse_release_date(s)} for s in dates],
        "size": [{"in": s, "out": G.parse_size_px(s)} for s in sizes],
        "labelled": [{"lines": l, "label": lab, "out": G.labelled_value(l, lab)} for l, lab in labs],
        "fov_radius": [{"w": w, "h": h, "out": G.fov_radius_arcmin(w, h)} for w, h in ((30.0, 30.0), (3.0, 4.0), (0.0, 0.0))],
        "copyright": [{"in": s, "out": G.asserts_copyright(s)} for s in
                      ["NASA, ESA", "© AAO", "Copyright David Malin", "(c) someone", "all rights reserved", None, "", "Credit: ESO"]],
        "credit_html": [{"html": h, "out": G.parse_credit(h)} for h in [
            '<div class="credit">NASA , ESA , M. Robberto ( STScI / ESA )</div>',
            '<div class="credit"><a href="x">NASA</a>, <a>ESA</a> &amp; Hubble</div>',
            '<div class="credit">   </div>', 'no credit here',
            '<div class="credit" id="c">A<br/>B  ; C . D</div>']],
        "filter_html": [{"html": h, "out": G.parse_filter_bands(h)} for h in [
            'Colours &amp; filters<table><tr><th>Band</th><th>Telescope</th></tr>'
            '<tr><td><span class="band_instrument">Optical<br/>B</span></td><td>HST</td></tr>'
            '<tr><td><span class="band_instrument">Optical<br/>H-alpha</span></td><td>HST</td></tr>'
            '<tr><td></td><td>HST</td></tr></table>',
            'Colours & Filters<table><tr><td>Infrared J</td><td>x</td></tr></table>', 'nothing', 'Colour &amp; filter</table>']],
        "search_url": [{"gallery": g, "criteria": c, "page": p, "out": G.search_url(G.GALLERIES[g], c, p)}
                       for g, c, p in (("esa_hubble", {"q": "orion"}, 1), ("esa_hubble", {"q": "m 42", "a": "b&c"}, 3),
                                       ("eso", {"q": "orion"}, 2))],
        "detail_url": [{"gallery": g, "id": i, "out": G.detail_url(G.GALLERIES[g], i)}
                       for g, i in (("esa_hubble", "heic0601a"), ("eso", "eso1103a"))],
        "galleries": G.GALLERIES,
        "result_total": [{"html": h, "out": G.parse_result_total(h)} for h in [
            "<p>Showing 1 to 35 of 35</p>", "Showing 1,001 to 1,050 of 12,345 results", "Showing nothing", ""]],
    })


def stage_skymath_wcs(out: Out):
    from autocontrast.db import skymath as S
    from autocontrast.db.wcs import _geometry
    from astropy.wcs import WCS
    rng = rng0()
    pairs = [(10.0, 20.0, 10.0, 20.0), (359.9, 0.0, 0.1, 0.0), (0.0, 89.9, 180.0, 89.9), (0.0, 90.0, 123.0, 90.0),
             (0.0, 90.0, 0.0, -90.0), (0.0, 0.0, 180.0, 0.0), (83.8, -5.4, 83.9, -5.4), (83.8, -5.4, 83.8, -5.3),
             (10.0, 10.0, 10.0000001, 10.0), (350.0, -60.0, 10.0, -60.0), (0.0, 0.0, 1e-9, 1e-9), (123.4, 56.7, 303.4, -56.7)]
    for _ in range(40):
        pairs.append((rng.uniform(0, 360), rng.uniform(-90, 90), rng.uniform(0, 360), rng.uniform(-90, 90)))
    sep = [{"p": list(p), "sep_arcmin": S.separation_arcmin(*p)} for p in pairs]
    cones = [(5.0, 2.0, 3.0), (5.0, 2.0, 2.9), (0.0, 0.0, 0.0), (10.0, 4.0, 6.0), (10.000000001, 4.0, 6.0)]
    out.json("skymath/separation.json", {"pairs": sep, "cones": [{"sep": a, "ra": b, "rb": c, "overlap": S.cones_overlap(a, b, c)}
                                                                  for a, b, c in cones]})

    # _geometry on synthetic TAN WCS ---------------------------------------------------------------------------
    def tan(crpix, crval, ctype=("RA---TAN", "DEC--TAN")):
        w = WCS(naxis=2)
        w.wcs.ctype = list(ctype)
        w.wcs.crpix = list(crpix)
        w.wcs.crval = list(crval)
        w.wcs.cunit = ["deg", "deg"]
        return w

    cases = []

    def rec(name, w, nx, ny, form, spec):
        ra, dec, rad, sc = _geometry(w, nx, ny)
        cases.append({"name": name, "form": form, "nx": nx, "ny": ny, "wcs": spec,
                      "ra": ra, "dec": dec, "radius_arcmin": rad, "pixel_scale_arcsec": sc})

    th = math.radians(30.0)
    s = 1.0 / 3600.0                                              # 1 arcsec/px
    w1 = tan((512.5, 384.5), (83.82, -5.38))                       # CD, rotated 30 deg, standard parity
    cd1 = [[-s * math.cos(th), s * math.sin(th)], [s * math.sin(th), s * math.cos(th)]]
    w1.wcs.cd = cd1
    rec("cd_rotated", w1, 1024, 768, "CD", {"crpix": [512.5, 384.5], "crval": [83.82, -5.38], "cd": cd1})
    w2 = tan((300.0, 200.0), (359.9, 45.0))                         # CD, flipped parity, near RA wrap, anisotropic
    cd2 = [[0.9 * s, 0.0], [0.0, 1.3 * s]]
    w2.wcs.cd = cd2
    rec("cd_flipped_wrap", w2, 600, 400, "CD", {"crpix": [300.0, 200.0], "crval": [359.9, 45.0], "cd": cd2})
    w3 = tan((1000.0, 1000.0), (200.0, -70.0))                      # CDELT + PC rotation + skew-free flip
    pc3 = [[math.cos(th), -math.sin(th)], [math.sin(th), math.cos(th)]]
    cdelt3 = [-0.4 * s, 0.4 * s]
    w3.wcs.cdelt = cdelt3
    w3.wcs.pc = pc3
    rec("cdelt_pc_rotated", w3, 2000, 1500, "CDELT+PC", {"crpix": [1000.0, 1000.0], "crval": [200.0, -70.0],
                                                           "cdelt": cdelt3, "pc": pc3})
    w4 = tan((9000.0, 9000.0), (83.7905320053, -5.41397796614))     # CDELT only (AVM form, heic0601a values)
    cdelt4 = [-2.78028143237e-05, 2.78028143237e-05]
    w4.wcs.cdelt = cdelt4
    rec("cdelt_only_avm", w4, 18000, 18000, "CDELT", {"crpix": [9000.0, 9000.0], "crval": [83.7905320053, -5.41397796614],
                                                       "cdelt": cdelt4})
    w5 = tan((50.0, 60.0), (10.0, 89.0))                             # near the pole, CD skewed
    cd5 = [[-s, 0.2 * s], [0.1 * s, s]]
    w5.wcs.cd = cd5
    rec("cd_skew_pole", w5, 100, 120, "CD", {"crpix": [50.0, 60.0], "crval": [10.0, 89.0], "cd": cd5})
    out.json("wcs/geometry.json", {"cases": cases})


def _tri_image(rng, h, w, base_rgb=(0.12, 0.11, 0.10)):
    img = np.empty((h, w, 3))
    for c in range(3):
        img[..., c] = base_rgb[c]
    return img


def stage_guardrails(out: Out):
    from autocontrast.optimize import guardrails as GR
    from autocontrast.fingerprint.color import rgb_to_lab
    rng = rng0()
    H = W = 48
    star_pos = [(10, 10), (12, 34), (24, 20), (35, 8), (36, 38), (20, 42)]

    N0 = rng.standard_normal((H, W, 1))                 # one achromatic noise field shared by every variant

    def sky(noise=0.012, tint=(1.0, 0.95, 0.9), level=0.2):
        img = np.empty((H, W, 3))
        for c in range(3):
            img[..., c] = level * tint[c]
        img += N0 * noise
        return img

    def add_stars(img, sy=1.4, sx=1.4, positions=star_pos, amp=0.5):
        out_ = img.copy()
        for (cy, cx) in positions:
            g = gauss_blob(H, W, cy, cx, sy, sx)
            out_ += amp * g[..., None]
        return out_

    def nebula_tint(img):
        g = gauss_blob(H, W, 24, 24, 12, 14)
        return img + g[..., None] * np.array([0.03, 0.01, 0.0])        # faint orange hue, below the star threshold

    base = nebula_tint(add_stars(sky()))
    imgs = {"base": to_u8(base)}
    b = u8_to_rgb(imgs["base"])
    imgs["ok"] = to_u8(np.clip(b + 0.01, 0, 1))
    imgs["noisy"] = to_u8(b + rng.standard_normal((H, W, 1)) * 0.03)
    imgs["starloss"] = to_u8(nebula_tint(add_stars(sky(), positions=star_pos[:3])))
    imgs["starbloat"] = to_u8(nebula_tint(add_stars(sky(), sy=2.6, sx=2.6)))
    imgs["starecc"] = to_u8(nebula_tint(add_stars(sky(), sy=0.9, sx=2.3)))
    sh = b.copy()
    sh[3:8, 3:8, :] = 0.0
    imgs["shadow"] = to_u8(sh)
    hi = b.copy()
    hi[40:45, 3:8, :] = 1.0
    imgs["highlight"] = to_u8(hi)
    hue = b.copy()
    hue[2:14, 2:14, :] = np.array([0.15, 0.55, 0.18])                    # green patch: hue the source lacks
    imgs["hue"] = to_u8(hue)
    gray_src = add_stars(sky(tint=(1.0, 1.0, 1.0)))
    imgs["gray"] = to_u8(gray_src)
    gtint = u8_to_rgb(imgs["gray"]).copy()
    gtint[..., 0] += 0.03
    imgs["gray_tint"] = to_u8(gtint)
    ratio = b.copy()
    ratio = np.clip(ratio * np.array([1.3, 0.9, 0.7]), 0, 1)
    imgs["ratio"] = to_u8(ratio)
    imgs["const"] = np.full((H, W, 3), 60, np.uint8)
    imgs["starless"] = to_u8(nebula_tint(sky()))
    imgs["mild_noise"] = to_u8(b + rng.standard_normal((H, W, 1)) * 0.0012)
    for k, v in imgs.items():
        out.npy(f"guardrails/img_{k}_u8.npy", v)
    R = {k: u8_to_rgb(v) for k, v in imgs.items()}

    cases = [  # name, candidate, baseline, source, expected tripped (None = not asserted)
        ("ok", "ok", "base", "base", []),
        ("noise", "noisy", "base", "base", ["noise_floor"]),
        ("star_loss", "starloss", "base", "base", ["star_integrity"]),
        ("star_bloat", "starbloat", "base", "base", ["star_integrity"]),
        ("star_ecc", "starecc", "base", "base", ["star_integrity"]),
        ("shadow", "shadow", "base", "base", ["shadow_clipping"]),
        ("highlight", "highlight", "base", "base", ["highlight_clipping"]),
        ("hue", "hue", "base", "base", ["hue_invention"]),
        ("hue_neutral_source", "gray_tint", "gray", "gray", ["hue_invention"]),
        ("ratio", "ratio", "base", "base", ["channel_ratio_drift"]),
        ("baseline_sigma_zero", "base", "const", "base", ["noise_floor"]),
        ("starless_baseline", "starless", "starless", "starless", []),
        ("mild_noise", "mild_noise", "base", "base", None),
    ]
    limit_variants = {"default": {}, "tight": dict(noise_growth=0.0, star_count_drop=0.0, hue_tolerance_deg=3.0,
                                                     channel_ratio_drift=0.001, star_fwhm_growth=0.0, star_ecc_growth=0.0)}
    out_cases = []
    for name, cand, bas, src, expect in cases:
        for lname, lkw in limit_variants.items():
            L = GR.GuardrailLimits(**lkw)
            vs = GR.evaluate_guardrails(R[cand], R[bas], R[src], L)
            tripped = [v.name for v in vs if not v.ok]
            if lname == "default" and expect is not None:
                assert tripped == expect, f"guardrail case {name}: expected {expect}, tripped {tripped}: {vs}"
            out_cases.append({"name": name, "limits": lname, "limits_kw": lkw, "candidate": cand, "baseline": bas,
                              "source": src, "tripped": tripped,
                              "verdicts": [dataclasses.asdict(v) for v in vs]})
    out.json("guardrails/cases.json", {"cases": out_cases, "limit_defaults": dataclasses.asdict(GR.GuardrailLimits()),
                                         "consts": {"MAD_TO_SIGMA": GR._MAD_TO_SIGMA, "NUM_HUE_BINS": GR.NUM_HUE_BINS,
                                                    "HUE_CHROMA_FLOOR": GR.HUE_CHROMA_FLOOR}})

    # component quantities per image ------------------------------------------------------------------------------
    comp = {}
    for k, v in R.items():
        gray = GR._gray(v)
        st = GR.detect_stars(gray)
        sigma = GR.mrs_noise_sigma(gray)
        hue_mass = GR._hue_mass_by_bin(v)
        out.npy(f"guardrails/huemass_{k}.npy", hue_mass)
        comp[k] = {"mrs_noise_sigma": sigma, "stars": dataclasses.asdict(st), "clipped_at_0": GR.clipped_fraction(v, 0.0),
                   "clipped_at_1": GR.clipped_fraction(v, 1.0), "channel_ratios": GR._channel_ratios(v),
                   "median_gray": float(np.median(gray))}
        mask = gray > float(np.median(gray)) + 5.0 * max(sigma, 1e-9)
        lab, n = ndimage.label(mask)
        objs = ndimage.find_objects(lab)
        out.npy(f"guardrails/starmask_{k}.npy", mask)
        out.npy(f"guardrails/starlabels_{k}.npy", lab.astype(np.int64))
        out.npy(f"guardrails/starboxes_{k}.npy",
                np.array([[s[0].start, s[0].stop, s[1].start, s[1].stop] for s in objs], dtype=np.int64).reshape(-1, 4))
    for k_sig in (3.0, 5.0, 8.0):
        comp.setdefault("base_ksigma", {})[str(k_sig)] = dataclasses.asdict(GR.detect_stars(GR._gray(R["base"]), k_sigma=k_sig))
    # dilation helper
    occ = np.zeros(360, dtype=bool)
    occ[[0, 100, 359]] = True
    for rad in (0, 1, 20, 40):
        out.npy(f"guardrails/dilate_r{rad}.npy", GR._dilate_circular(occ, rad))
    out.npy("guardrails/dilate_in.npy", occ)
    out.json("guardrails/components.json", comp)


def _run_loop_case(out: Out, name, *, inputs, proxy_rgb, ref_rgb, kw, config=None, limits=None, executor=None):
    """Run begin + advance*N with NumpyExecutor on an in-memory store; record everything."""
    from autocontrast.optimize import loop as LP
    from autocontrast.optimize.beam import BeamConfig
    from autocontrast.optimize.guardrails import GuardrailLimits
    from autocontrast.optimize.executors.numpy_exec import NumpyExecutor
    from autocontrast.fingerprint.extract import extract
    from autocontrast.optimize.session import Session

    store: dict[str, np.ndarray] = {}
    saved_paths: list[str] = []

    def load(path):
        return store[path]

    def save(path, arr):
        store[path] = np.array(arr, copy=True)
        saved_paths.append(path)

    proxy_path = f"/ac/{name}/proxy.png"
    source_path = f"/ac/{name}/source.png"
    work_dir = f"/ac/{name}/work"
    store[proxy_path] = np.array(proxy_rgb, copy=True)
    store[source_path] = np.array(proxy_rgb, copy=True)
    ref_fp = extract(ref_rgb, **kw)
    config = config or BeamConfig()
    limits = limits or GuardrailLimits()
    executor = executor or NumpyExecutor()

    attempts: list[dict] = []
    menus: list[dict] = []
    real_ingest = LP.ingest_candidate
    real_menu = LP._ranked_menu
    state = {"it": 0}

    def rec_ingest(session, **kwargs):
        before = len(session.guardrail_log)
        res = real_ingest(session, **kwargs)
        attempts.append({
            "iteration": kwargs["iteration"], "parent": kwargs["parent_recipe"].key, "action": kwargs["action"].key,
            "candidate_path": kwargs["candidate_path"], "kept": res is not None,
            "distance": None if res is None else res.distance,
            "recipe": None if res is None else res.recipe.key,
            "log_entries": session.guardrail_log[before:],
        })
        return res

    def rec_menu(reference_fp, parent_fp, **kwargs):
        res = real_menu(reference_fp, parent_fp, **kwargs)
        menus.append({"iteration": state["it"] + 1, "applied_kinds": kwargs["applied_kinds"],
                      "keys": [a.key for a in res]})
        return res

    LP.ingest_candidate = rec_ingest
    LP._ranked_menu = rec_menu
    try:
        sess = LP.begin(source_path=source_path, proxy_path=proxy_path, reference_id=f"golden-{name}",
                        reference_fp=ref_fp, work_dir=work_dir, pixel_scale_arcsec=kw["pixel_scale_arcsec"],
                        psf_fwhm_arcsec=kw["psf_fwhm_arcsec"], palette_class=kw["palette_class"],
                        n_scales=kw["n_scales"], session_id=f"golden-{name}", load=load, config=config)
        iterations = []
        with tempfile.TemporaryDirectory() as td:
            while not sess.converged:
                state["it"] = sess.iteration
                n_log_before = len(sess.guardrail_log)
                sess = LP.advance(sess, executor, load=load, save=save, limits=limits)
                iterations.append({
                    "iteration": sess.iteration,
                    "branches": [{"recipe": b.recipe.key, "actions": [action_dict(a) for a in b.recipe.actions],
                                  "image_path": b.image_path, "distance": b.distance, "alive": b.alive}
                                 for b in sess.branches],
                    "best": {"recipe": sess.best.recipe.key, "distance": sess.best.distance,
                             "image_path": sess.best.image_path},
                    "distance_history": list(sess.distance_history),
                    "guardrail_log_new": sess.guardrail_log[n_log_before:],
                    "converged": sess.converged, "convergence_reason": sess.convergence_reason,
                })
            sess_path = Path(td) / "session.json"
            sess.save(sess_path)
            session_text = sess_path.read_text()
    finally:
        LP.ingest_candidate = real_ingest
        LP._ranked_menu = real_menu
    result = LP.outcome(sess)
    out.json(f"loop/{name}.json", {
        "name": name, "inputs": inputs, "params": kw, "config": dataclasses.asdict(config), "limits": dataclasses.asdict(limits),
        "paths": {"proxy": proxy_path, "source": source_path, "work_dir": work_dir},
        "reference_fp": fp_dict(ref_fp), "baseline_distance": sess.baseline_distance,
        "iterations": iterations, "attempts": attempts, "menus": menus, "guardrail_log": sess.guardrail_log,
        "distance_history": sess.distance_history, "outcome": result, "saved_paths": saved_paths,
        "n_attempts": len(attempts),
    })
    if "Infinity" in session_text or "NaN" in session_text:
        raise RuntimeError(f"loop case {name}: Session.save wrote a non-finite number; schema golden would be unreadable")
    (out.root / f"loop/{name}_session.json").parent.mkdir(parents=True, exist_ok=True)
    out._path(f"loop/{name}_session.json").write_text(session_text + "\n")
    return result


def stage_loop(out: Out):
    from autocontrast.eval.degrade import flatten
    from autocontrast.optimize.beam import BeamConfig
    rng = rng0()
    kw = dict(pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0, palette_class="RGB")
    neb_u8 = make_nebula(rng, 120, 160)
    neb = u8_to_rgb(neb_u8)
    flat = flatten(neb, 0.6)

    # (a) the nebula against its own fingerprint: must decline
    ra = _run_loop_case(out, "a_decline", inputs={"proxy": "images/nebula_u8.npy", "reference": "images/nebula_u8.npy"}, proxy_rgb=neb, ref_rgb=neb, kw=kw)
    assert ra["improved"] is False, "case (a) was meant to decline"
    # (b) flatten(neb, 0.6) against the original
    rb = _run_loop_case(out, "b_flattened", inputs={"proxy": "images/flat06_f8.npy", "reference": "images/nebula_u8.npy"}, proxy_rgb=flat, ref_rgb=neb, kw=kw)

    # (c) guardrail-hostile input: clipped blacks/whites + a strong colour cast, so candidates trip guardrails
    hostile = neb.copy()
    h, w = hostile.shape[:2]
    hostile[: h // 3, : w // 5, :] = 0.0
    hostile[h - 8:, w - 40:, :] = 1.0
    hostile = np.clip(hostile * np.array([1.0, 0.8, 0.5]), 0.0, 1.0)
    hostile_u8 = to_u8(hostile)
    out.npy("loop/hostile_u8.npy", hostile_u8)
    hostile = u8_to_rgb(hostile_u8)
    _run_loop_case(out, "c_hostile", inputs={"proxy": "loop/hostile_u8.npy", "reference": "images/nebula_u8.npy"}, proxy_rgb=hostile, ref_rgb=neb, kw=kw)

    # (d) tiny image: local_contrast layers beyond what 32x24 supports make the executor raise
    small = u8_to_rgb(make_nebula(rng, 24, 32, n_stars=6))
    small_f = flatten(small, 0.7)
    out.npy("loop/small_u8.npy", to_u8(small))
    out.npy("loop/small_flat07_f8.npy", small_f)
    small = u8_to_rgb(to_u8(small))
    kw_small = dict(pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0, palette_class="RGB")
    _run_loop_case(out, "d_small_executor_failure", inputs={"proxy": "loop/small_flat07_f8.npy", "reference": "loop/small_u8.npy"}, proxy_rgb=small_f, ref_rgb=small, kw=kw_small,
                   config=BeamConfig(top_k=7, iteration_cap=3))

    # (e) narrower beam / smaller iteration cap config, same flattened image, to pin config plumbing
    _run_loop_case(out, "e_config", inputs={"proxy": "images/flat06_f8.npy", "reference": "images/nebula_u8.npy"}, proxy_rgb=flat, ref_rgb=neb, kw=kw,
                   config=BeamConfig(width=2, top_k=2, iteration_cap=4, convergence_window=2, epsilon=1e-3,
                                     epsilon_improve=5e-3))


SYNTHETIC_STAGES = [
    ("images", stage_images),
    ("npy_selftest", stage_npy_selftest),
    ("primitives", stage_primitives),
    ("starlet_energy_bandlimit", stage_starlet_energy_bandlimit),
    ("color_metrics", stage_color_metrics),
    ("extract", stage_extract),
    ("distance", stage_distance),
    ("actions_propose", stage_actions_propose),
    ("degrade", stage_degrade),
    ("palette", stage_palette),
    ("gallery", stage_gallery),
    ("skymath_wcs", stage_skymath_wcs),
    ("guardrails", stage_guardrails),
    ("loop", stage_loop),
]


def run_synthetic(outdir: Path):
    out = Out(outdir)
    for name, fn in SYNTHETIC_STAGES:
        print(f"[synthetic] {name} ...", flush=True)
        fn(out)
    out.prune()
    return out


# --------------------------------------------------------------------------------------
# Real-data stages
# --------------------------------------------------------------------------------------

REAL_MAX_DIM = 1600
REAL_EXTRACT = dict(pixel_scale_arcsec=1.0, n_scales=7, psf_fwhm_arcsec=2.0, palette_class="RGB")
REAL_STRENGTHS = [0.0, 0.25, 0.5, 0.75, 1.0]
ROWS_KEEP_COLS = 256


def safe_key(group: str, name: str) -> str:
    stem = Path(name).stem
    return f"{group}__" + re.sub(r"[^A-Za-z0-9._-]", "_", stem)


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def resolve_data_dir(arg: str | None) -> Path:
    cand = arg or os.environ.get("AC_DATA_DIR") or str(AC_ROOT / "data")
    p = Path(cand).resolve()
    for sub in ("references", "amateur", "discovery_cache"):
        if not (p / sub).is_dir():
            raise SystemExit(f"data dir {p} has no '{sub}/' (set --data-dir or AC_DATA_DIR)")
    return p


def avm_probe(path: Path) -> dict:
    from pyavm import AVM, NoAVMPresent
    rec = {"present": True, "ok": False, "error_type": None, "error_message": None, "wcs": None, "spatial": None}
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        try:
            avm = AVM.from_image(str(path))
        except NoAVMPresent as e:
            rec["present"] = False
            rec["error_type"] = "NoAVMPresent"
            rec["error_message"] = str(e)
            return rec
        except Exception as e:
            rec["error_type"] = type(e).__name__
            rec["error_message"] = str(e)
            return rec
        rec["ok"] = True
        rec["spatial"] = getattr(avm.Spatial, "_items", None)
        try:
            w = avm.to_wcs()
        except Exception as e:
            rec["wcs_error_type"] = type(e).__name__
            rec["wcs_error_message"] = str(e)
            return rec
    if w is None:
        rec["wcs"] = None
        return rec
    rec["wcs"] = {
        "ctype": list(w.wcs.ctype), "crpix": w.wcs.crpix, "crval": w.wcs.crval,
        "has_cd": bool(w.wcs.has_cd()), "cd": w.wcs.cd if w.wcs.has_cd() else None,
        "cdelt": w.wcs.cdelt, "pixel_scale_matrix": w.pixel_scale_matrix, "has_celestial": bool(w.has_celestial),
    }
    return rec


def run_real(outdir: Path, data_dir: Path):
    from PIL import Image, features
    from autocontrast.io.loaders import load_raster, image_dimensions
    from autocontrast.db.wcs import _from_avm
    from autocontrast.fingerprint.extract import extract
    from autocontrast.fingerprint.distance import fingerprint_distance
    from autocontrast.eval import ranking as RK
    from autocontrast.eval import degrade as DG

    out = Out(outdir)
    groups = [("reference", sorted((data_dir / "references").glob("*.jpg"))),
              ("amateur", sorted((data_dir / "amateur").glob("*.jpg"))),
              ("cache", sorted((data_dir / "discovery_cache").glob("*.jpg")))]
    counts = {g: len(p) for g, p in groups}
    if counts != {"reference": 5, "amateur": 5, "cache": 35}:
        raise SystemExit(f"unexpected corpus {counts}; expected 5 references, 5 amateurs, 35 cached renders")

    Image.MAX_IMAGE_PIXELS = None
    index = []
    fps: dict[str, object] = {}
    rasters: dict[str, np.ndarray] = {}
    for group, paths in groups:
        for p in paths:
            key = safe_key(group, p.name)
            print(f"[real] {key} ...", flush=True)
            rec = {"key": key, "group": group, "file": p.name, "file_bytes": p.stat().st_size,
                   "file_sha256": sha256_file(p)}
            # native decode: dimensions, full-image digest, first 16 rows
            with Image.open(p) as im:
                rec["pillow"] = {"format": im.format, "mode": im.mode, "size": list(im.size)}
                full = np.asarray(im)
            rec["image_dimensions"] = list(image_dimensions(p))
            assert full.dtype == np.uint8 and full.ndim == 3 and full.shape[2] == 3
            h = hashlib.sha256()
            for r0 in range(0, full.shape[0], 256):
                h.update(np.ascontiguousarray(full[r0:r0 + 256]).tobytes())
            rec["decode_full"] = {"shape": list(full.shape), "sha256_uint8": h.hexdigest()}
            rows = np.ascontiguousarray(full[:16])
            rec["decode_rows16"] = {"shape": list(rows.shape), "sha256_uint8": hashlib.sha256(rows.tobytes()).hexdigest(),
                                    "kept_cols": min(ROWS_KEEP_COLS, rows.shape[1])}
            out.npy(f"{key}.rows16_cols{ROWS_KEEP_COLS}.npy", rows[:, :ROWS_KEEP_COLS, :])
            del full, rows
            # load_raster(max_dim=1600)
            arr = load_raster(p, max_dim=REAL_MAX_DIM)
            assert arr.dtype == np.float64 and arr.ndim == 3
            rec["raster"] = {"max_dim": REAL_MAX_DIM, "shape": list(arr.shape),
                             "sha256_float64": hashlib.sha256(np.ascontiguousarray(arr).tobytes()).hexdigest()}
            ch, cw = arr.shape[0], arr.shape[1]
            y0, x0 = (ch - 64) // 2, (cw - 64) // 2
            crop = arr[y0:y0 + 64, x0:x0 + 64, :]
            as_u8 = np.rint(crop * 255.0).astype(np.uint8)
            if np.array_equal(as_u8.astype(np.float64) / 255.0, crop):
                out.npy(f"{key}.crop64.npy", as_u8)          # exact: stored as the integer source
                rec["crop64"] = {"dtype": "uint8", "origin_yx": [y0, x0], "note": "float image = u8/255.0 exactly"}
            else:
                out.npy(f"{key}.crop64.npy", crop)
                rec["crop64"] = {"dtype": "float64", "origin_yx": [y0, x0]}
            # AVM
            rec["avm"] = avm_probe(p)
            notes: list[str] = []
            res = _from_avm(p, notes)
            rec["from_avm"] = {"result": res, "notes": notes}
            # fingerprint
            fp = extract(arr, **REAL_EXTRACT)
            fps[key] = fp
            rasters[key] = arr
            rec["fingerprint"] = {"params": REAL_EXTRACT, "value": fp_dict(fp)}
            out.json(f"{key}.json", rec)
            index.append({"key": key, "group": group, "file": p.name, "file_sha256": rec["file_sha256"],
                          "file_bytes": rec["file_bytes"], "raster_shape": rec["raster"]["shape"]})
    out.json("index.json", {"max_dim": REAL_MAX_DIM, "extract_params": REAL_EXTRACT, "strengths": REAL_STRENGTHS,
                            "entries": index, "pillow_libjpeg_turbo": features.version("libjpeg_turbo"),
                            "pillow_libjpeg_api": features.version("jpg")})

    # reports ------------------------------------------------------------------------------------------------------
    ref_keys = [k for k in fps if k.startswith("reference__")]
    am_keys = [k for k in fps if k.startswith("amateur__")]
    pros = {k: fps[k] for k in ref_keys}
    amateurs = {k: fps[k] for k in am_keys}
    loo = RK.leave_one_out_report(pros, amateurs)
    allkeys = list(fps)
    d_to_refs = {k: {r: fingerprint_distance(fps[r], fps[k]) for r in ref_keys} for k in allkeys}
    ten = ref_keys + am_keys
    matrix = [[fingerprint_distance(fps[a], fps[b]) for b in ten] for a in ten]
    ranks = {}
    for r in ref_keys:
        others = {k: v for k, v in fps.items() if k != r}
        ranks[r] = [{"name": e.name, "distance": e.distance} for e in RK.rank_against_reference(fps[r], others)]
    out.json("reports/leave_one_out.json", {"report": loo, "ten_order": ten, "ten_distance_matrix": matrix,
                                            "rankings": ranks})
    out.json("reports/distance_to_references.json", d_to_refs)

    lm = {}
    for r in ref_keys:
        rep = DG.reference_is_local_minimum(rasters[r], strengths=REAL_STRENGTHS, extract_kwargs=REAL_EXTRACT)
        lm[r] = rep
    out.json("reports/local_minimum.json", {"strengths": REAL_STRENGTHS, "extract_params": REAL_EXTRACT,
                                            "axes": list(DG.DEFAULT_DEGRADATIONS), "per_reference": lm})
    out.prune()
    return out


# --------------------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------------------

def git(*args, cwd=AC_ROOT) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def build_manifest(root: Path) -> dict:
    import PIL
    import scipy
    import astropy
    import tifffile
    import pyavm
    import png
    from PIL import features
    try:
        from importlib.metadata import version as dist_version
    except ImportError:                                      # pragma: no cover
        dist_version = None

    def dv(name):
        return dist_version(name)

    status = [l for l in git("status", "--porcelain", "--", ".").splitlines() if "data/" not in l]
    files = {}
    for sub in ("synthetic", "real"):
        d = root / sub
        if not d.is_dir():
            continue
        for p in sorted(d.rglob("*")):
            if p.is_file():
                files[f"{sub}/{p.relative_to(d).as_posix()}"] = {"bytes": p.stat().st_size, "sha256": sha256_file(p)}
    man = {
        "schema": 1,
        "description": "Goldens produced by tools/autocontrast (Python reference) for the AutoContrast PCL port. "
                       "Regenerate with make_goldens.py; never edit by hand.",
        "seed": SEED,
        "nonfinite_encoding": {"nan": "NaN", "inf": "Infinity", "-inf": "-Infinity"},
        "image_convention": "arrays stored as uint8 are 8-bit renders: float image = u8.astype(float64) / 255.0",
        "reference": {
            "repo_head": git("rev-parse", "HEAD"),
            "tools_autocontrast_tree": git("rev-parse", "HEAD:tools/autocontrast"),
            "last_commit_touching_tools_autocontrast": git("log", "-1", "--format=%H", "--", "."),
            "uncommitted_changes_under_tools_autocontrast": status,
        },
        "versions": {
            "python": platform.python_version(),
            "numpy": np.__version__, "scipy": scipy.__version__, "astropy": astropy.__version__,
            "Pillow": PIL.__version__, "libjpeg_turbo": features.version("libjpeg_turbo"),
            "libjpeg_api": features.version("jpg"), "zlib": features.version("zlib"),
            "libtiff": features.version("libtiff"),
            "libjpeg_turbo_flag": bool(features.check_feature("libjpeg_turbo")),
            "tifffile": tifffile.__version__, "pypng": dv("pypng"), "pypng_module": getattr(png, "__version__", None),
            "pyavm": dv("pyavm"), "pyerfa": dv("pyerfa"),
            "pytest": dv("pytest"),
        },
        "files": files,
        "total_bytes": sum(v["bytes"] for v in files.values()),
    }
    return man


def write_manifest(root: Path, extra: dict | None = None):
    man = build_manifest(root)
    idx = root / "real" / "index.json"
    if idx.exists():
        real = json.loads(idx.read_text())
        man["real_corpus"] = {"entries": real["entries"], "max_dim": real["max_dim"],
                              "extract_params": real["extract_params"]}
    (root / "MANIFEST.json").write_text(
        json.dumps(enc(man), sort_keys=True, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["synthetic", "real", "manifest", "all"])
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--data-dir", default=None)
    args = ap.parse_args(argv)

    if args.mode == "synthetic":
        run_synthetic(args.outdir)
    elif args.mode == "real":
        run_real(args.outdir, resolve_data_dir(args.data_dir))
    elif args.mode == "manifest":
        write_manifest(args.outdir)
    else:
        run_synthetic(args.outdir / "synthetic")
        run_real(args.outdir / "real", resolve_data_dir(args.data_dir))
        write_manifest(args.outdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
