#!/usr/bin/env python
"""Golden generator for the Gaia Depth Grade PCL port.

Runs the PYTHON reference core (gaia-depth-grade/src/gaia_depth_grade) stage by stage and
dumps every stage's output as a little-endian C-order .npy, plus stage.json (scalars and
parameters) and MANIFEST.json (what was generated, from which commit, with which library
versions). The C++ tests (test/unit/test_*.cpp) read these through test/util/Npy.h and must
reproduce them to the tolerances in the plan.

    make_goldens.py synthetic <outdir>
    make_goldens.py real <stars.fits> <starless.fits> <catalog.tsv|online.ecsv> <outdir>

Run it under the gaia-depth-grade project's own venv, isolated:

    gaia-depth-grade/.venv/bin/python -I modules/gaia-depth-grade/test/golden/make_goldens.py \
        synthetic modules/gaia-depth-grade/test/golden/synthetic

The Python core and its tests/ fixtures are put on sys.path explicitly (-I ignores
PYTHONPATH and the script directory), from the repo this file lives in.

Frame contract: arrays are exactly what the Python core sees (shape (H, W[, C]), row 0 =
FITS bottom row). Images are stored as float32 (the generator rounds its own inputs to
float32 first, so the Python core consumes exactly the stored input); image OUTPUTS are
computed in float64 and stored as float32 (<= 6e-8 absolute on [0, 1] data). Tables,
vectors and scalars are stored as float64 (bool as |b1, 8-bit display output as |u1).

No timestamps or random state outside the seeded generators: the output is byte-for-byte
reproducible given the same library versions, which MANIFEST.json pins.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import logging
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
PY_SRC = REPO / "gaia-depth-grade" / "src"
PY_TESTS = REPO / "gaia-depth-grade" / "tests"
sys.path.insert(0, str(PY_SRC))

import astropy  # noqa: E402
import numpy as np  # noqa: E402
import photutils  # noqa: E402
import scipy  # noqa: E402
from astropy.io import fits  # noqa: E402
from astropy.stats import sigma_clipped_stats  # noqa: E402
from astropy.table import Table  # noqa: E402
from astropy.wcs import WCS  # noqa: E402
from photutils.detection import DAOStarFinder  # noqa: E402
from scipy.ndimage import gaussian_filter, grey_closing  # noqa: E402

from gaia_depth_grade.config import Gains, GradeConfig  # noqa: E402
from gaia_depth_grade.detect import detect_stars  # noqa: E402
from gaia_depth_grade.display import autostretch, screen_blend  # noqa: E402
from gaia_depth_grade.distances import _OPTIONAL, _REQUIRED  # noqa: E402
from gaia_depth_grade.local_gaia import (  # noqa: E402
    LocalGaiaSource, parallax_to_distance_table, sigma_parallax_from_g)
from gaia_depth_grade.match import cross_match  # noqa: E402
from gaia_depth_grade.modulate import compute_modulation  # noqa: E402
from gaia_depth_grade.nebula import (  # noqa: E402
    gaia_depth_budget, render_nebula, structural_depth)
from gaia_depth_grade.pipeline import render_from_prep  # noqa: E402
from gaia_depth_grade.psf import build_halo_profile, moffat_profile  # noqa: E402
from gaia_depth_grade.render import render_stars  # noqa: E402
from gaia_depth_grade.transform import (  # noqa: E402
    confidence, depth_strength, effective_strength)
from gaia_depth_grade.wcs import FieldFootprint, field_footprint, load_wcs  # noqa: E402

from gaia_depth_grade.recon import cavity as rcavity  # noqa: E402
from gaia_depth_grade.recon.channels import (  # noqa: E402
    extract_channels, from_duoband, from_mono_sho, from_osc_rgb, looks_linear)
from gaia_depth_grade.recon.extinction import dust_from_balmer, dust_from_structure  # noqa: E402
from gaia_depth_grade.recon.ionization import ionization_ratio  # noqa: E402
from gaia_depth_grade.recon.ionizer import ionizer_from_xy, pick_ionizer  # noqa: E402
from gaia_depth_grade.recon.model import ReconConfig  # noqa: E402

SEED = 20261110
# Colour star field size. The plan names 512x384; at that size the committed goldens would be
# ~25 MB against the 5 MB budget (one colour image is 2.4 MB), so the field is 192x144.
FIELD_W, FIELD_H = 192, 144
N_STARS = 80
ODD_W, ODD_H = 64, 48

# The two gain sets: the dialog defaults (the process defaults, D9) and the engine defaults.
DIALOG_GAINS = Gains(brightness=0.65, size=0.85, contrast=0.12, saturation=0.4, halo_gain=1.0)
ENGINE_GAINS = Gains()
NEBULA_SETTINGS = {"s1_a1_t1": (1.0, 1.0, 1.0), "s2_a0p5_t1p5": (2.0, 0.5, 1.5)}


class Out:
    """Collects .npy files, stage.json and the manifest entries for one output directory."""

    def __init__(self, outdir: Path):
        self.dir = outdir
        outdir.mkdir(parents=True, exist_ok=True)
        self.files: dict[str, dict] = {}
        self.stage_json: dict[str, dict] = {}

    def save(self, name: str, arr, dtype: str = "f8") -> None:
        a = np.ascontiguousarray(np.asarray(arr).astype(dtype))
        path = self.dir / f"{name}.npy"
        np.save(path, a)
        self.files[name] = {"dtype": a.dtype.str, "shape": list(a.shape),
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def params(self, stage: str, **kw) -> None:
        self.stage_json.setdefault(stage, {}).update(kw)

    def save_table(self, prefix: str, table, cols, bool_cols=()) -> None:
        for c in cols:
            self.save(f"{prefix}_{c}", np.asarray(table[c]), "?" if c in bool_cols else "f8")


def _jsonable(o):
    if isinstance(o, (np.floating,)):
        return float(o)
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.bool_,)):
        return bool(o)
    if isinstance(o, np.ndarray):
        return o.tolist()
    raise TypeError(type(o))


# --------------------------------------------------------------------------- synthetic inputs

def field_wcs(w: int, h: int):
    """Same recipe as the simple_wcs_header fixture (tangent plane, RA=10, Dec=20, 1 arcsec/px),
    sized to the image."""
    wc = WCS(naxis=2)
    wc.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    wc.wcs.crpix = [w / 2.0, h / 2.0]
    wc.wcs.crval = [10.0, 20.0]
    wc.wcs.cdelt = [-1.0 / 3600.0, 1.0 / 3600.0]
    hdr = wc.to_header()
    hdr["NAXIS"] = 2
    hdr["NAXIS1"] = w
    hdr["NAXIS2"] = h
    return hdr


def _add_star(img, x, y, amp, color, sigma=2.0, wing_amp=0.08, wing_sigma=5.0):
    """Gaussian core plus a faint broad wing, added to img (H, W, 3) in place."""
    h, w = img.shape[:2]
    r = int(np.ceil(5 * wing_sigma))
    x0, x1 = max(0, int(x) - r), min(w, int(x) + r + 1)
    y0, y1 = max(0, int(y) - r), min(h, int(y) + r + 1)
    yy, xx = np.mgrid[y0:y1, x0:x1]
    d2 = (xx - x) ** 2 + (yy - y) ** 2
    prof = np.exp(-d2 / (2 * sigma ** 2)) + wing_amp * np.exp(-d2 / (2 * wing_sigma ** 2))
    img[y0:y1, x0:x1, :] += amp * prof[..., None] * np.asarray(color)[None, None, :]


def make_star_field(rng, w=FIELD_W, h=FIELD_H, n_stars=N_STARS, n_bright=10, n_sat=3):
    """Colour star layer (H, W, 3) float64 in [0, 1]. Returns (image, truth list)."""
    img = np.full((h, w, 3), 0.01)
    truth = []
    # Bright, unsaturated, isolated stars (the empirical halo's references): well clear of the
    # edges (>= 17 px, the halo stamp radius) and of each other (>= 30 px).
    anchors = []
    while len(anchors) < n_bright:
        x, y = rng.uniform(20, w - 20), rng.uniform(20, h - 20)
        if all(np.hypot(x - ax, y - ay) > 30 for ax, ay in anchors):
            anchors.append((x, y))
    sat = []
    while len(sat) < n_sat:
        x, y = rng.uniform(10, w - 10), rng.uniform(10, h - 10)
        if all(np.hypot(x - ax, y - ay) > 30 for ax, ay in anchors + sat):
            sat.append((x, y))

    def colour():
        c = np.array([rng.uniform(0.55, 1.0), rng.uniform(0.7, 1.0), rng.uniform(0.6, 1.0)])
        return c / c.max()

    for x, y in anchors:
        amp = rng.uniform(0.45, 0.8)
        c = colour()
        _add_star(img, x, y, amp, c)
        truth.append((x, y, amp, "bright"))
    for x, y in sat:
        _add_star(img, x, y, 1.8, colour())
        truth.append((x, y, 1.8, "saturated"))
    placed = anchors + sat
    for _ in range(n_stars - n_bright - n_sat):
        # Field stars stay out of the reference stars' 33x33 halo stamps (corner distance 23 px)
        # so the isolation test passes, and 6 px clear of every other star so DAOFIND resolves them.
        for _try in range(1000):
            x, y = rng.uniform(0, w - 1), rng.uniform(0, h - 1)
            if (all(np.hypot(x - ax, y - ay) > 23 for ax, ay in anchors)
                    and all(np.hypot(x - ax, y - ay) > 6 for ax, ay in placed)):
                break
        else:
            raise RuntimeError("could not place a field star; reduce N_STARS")
        amp = float(np.exp(rng.uniform(np.log(0.02), np.log(0.35))))
        _add_star(img, x, y, amp, colour(), sigma=rng.uniform(1.7, 2.4))
        placed.append((x, y))
        truth.append((x, y, amp, "field"))
    img += rng.normal(0.0, 0.002, img.shape)
    return np.clip(img, 0.0, 1.0), truth


def make_starless(rng, w=FIELD_W, h=FIELD_H):
    """Smooth colour nebula with dark lanes and sky noise (H, W, 3) in [0, 1]."""
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    img = np.full((h, w, 3), 0.02)
    for _ in range(6):
        cx, cy = rng.uniform(0.15, 0.85) * w, rng.uniform(0.15, 0.85) * h
        s = rng.uniform(0.12, 0.25) * min(w, h)
        amp = rng.uniform(0.15, 0.45)
        col = rng.uniform(0.3, 1.0, 3)
        img += amp * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * s * s))[..., None] * col
    # Dark lanes: segments with a 4 px Gaussian cross-section.
    for (x0, y0, x1, y1) in ((0.1 * w, 0.8 * h, 0.9 * w, 0.25 * h), (0.3 * w, 0.1 * h, 0.55 * w, 0.95 * h)):
        dx, dy = x1 - x0, y1 - y0
        t = np.clip(((xx - x0) * dx + (yy - y0) * dy) / (dx * dx + dy * dy), 0, 1)
        d2 = (xx - (x0 + t * dx)) ** 2 + (yy - (y0 + t * dy)) ** 2
        img *= (1.0 - 0.7 * np.exp(-d2 / (2 * 4.0 ** 2)))[..., None]
    img += rng.normal(0.0, 0.002, img.shape)
    return np.clip(img, 0.0, 1.0)


def make_catalog(rng, detected: Table, wcs, shape):
    """Synthetic Gaia export (ra, dec, parallax, G, bp_rp) aimed at the detected stars:
    70% of detections get a counterpart (0.7 px astrometric scatter), plus decoys inside the
    field, sources outside it, and a few low-S/N / non-positive parallaxes the cut must drop."""
    h, w = shape
    n = len(detected)
    pick = rng.choice(n, size=int(round(0.7 * n)), replace=False)
    px = np.asarray(detected["x"])[pick] + rng.normal(0, 0.7, len(pick))
    py = np.asarray(detected["y"])[pick] + rng.normal(0, 0.7, len(pick))
    n_decoy, n_out = 15, 6
    px = np.concatenate([px, rng.uniform(0, w, n_decoy), rng.uniform(-60, -10, n_out)])
    py = np.concatenate([py, rng.uniform(0, h, n_decoy), rng.uniform(0, h, n_out)])
    k = len(px)
    dist = 10 ** rng.uniform(np.log10(100.0), np.log10(3000.0), k)
    g = rng.uniform(9.0, 17.5, k)
    plx = 1000.0 / dist
    bad = rng.choice(k, size=6, replace=False)
    plx[bad[:4]] = 0.01       # S/N far below the cut
    plx[bad[4]] = -0.3        # non-positive
    plx[bad[5]] = np.nan
    bp = rng.normal(1.0, 0.6, k)
    bp[rng.choice(k, size=k // 10, replace=False)] = np.nan
    sky = wcs.pixel_to_world(px, py)
    return np.asarray(sky.ra.deg), np.asarray(sky.dec.deg), plx, g, bp


# --------------------------------------------------------------------------- the stage runner

def run_pipeline(o: Out, tag: str, stars, starless, header, raw_catalog, *, gains: Gains,
                 nebula_settings, expect_empirical_halo: bool) -> dict:
    """Run every pipeline stage on (stars, starless) with the given catalogue and dump it.

    stars / starless are (H, W[, C]) float64 arrays (already float32-exact). `tag` prefixes
    every file ('' for the colour case, 'mono_' for the mono case). raw_catalog is either
    the raw-Gaia tuple (ra, dec, parallax, G, bp_rp) or a ready distance Table.
    """
    p = tag
    cfg = GradeConfig(gains=gains)
    wcs = load_wcs(header)
    lum = stars.mean(axis=2) if stars.ndim == 3 else stars
    shape = lum.shape

    # sigma_clip
    mean, median, std = sigma_clipped_stats(lum, sigma=3.0)
    o.save(f"{p}sigma_clip", [mean, median, std])
    o.params(f"{p}sigma_clip", order=["mean", "median", "std"], sigma=3.0)

    # dao_table: the raw photutils table with the exact parameters detect_stars uses.
    finder = DAOStarFinder(fwhm=cfg.detect_fwhm, threshold=cfg.detect_threshold_sigma * std)
    found = finder(lum - median)
    if found is None or len(found) == 0:
        raise RuntimeError(f"{tag or 'colour'}: DAOStarFinder found nothing")
    xc = "x_centroid" if "x_centroid" in found.colnames else "xcentroid"
    yc = "y_centroid" if "y_centroid" in found.colnames else "ycentroid"
    dao = Table()
    dao["x"], dao["y"] = np.asarray(found[xc], float), np.asarray(found[yc], float)
    for c in ("flux", "peak", "sharpness", "roundness1", "roundness2"):
        dao[c] = np.asarray(found[c], float)
    o.save_table(f"{p}dao_table", dao, ["x", "y", "flux", "peak", "sharpness", "roundness1", "roundness2"])
    o.params(f"{p}dao_table", n=len(dao), fwhm=cfg.detect_fwhm, threshold_sigma=cfg.detect_threshold_sigma,
             threshold=cfg.detect_threshold_sigma * std)

    # detect_table: what detect_stars hands the pipeline (adds the 7x7 moment FWHM).
    detected = detect_stars(lum, cfg.detect_fwhm, cfg.detect_threshold_sigma)
    if len(detected) != len(dao):
        raise RuntimeError("detect_stars and the direct DAOStarFinder call disagree")
    o.save_table(f"{p}detect_table", detected, ["x", "y", "flux", "fwhm"])

    # footprint
    fp = field_footprint(wcs, shape)
    o.save(f"{p}footprint", [fp.center_ra, fp.center_dec, fp.radius_deg])
    o.params(f"{p}footprint", order=["center_ra", "center_dec", "radius_deg"], shape_hw=list(shape))

    # catalog
    if isinstance(raw_catalog, Table):
        catalog = raw_catalog
    else:
        ra, dec, plx, g, bp = raw_catalog
        o.save(f"{p}catalog_raw", np.column_stack([ra, dec, plx, g, bp]))
        o.params(f"{p}catalog_raw", columns=["ra", "dec", "parallax_mas", "G", "bp_rp"], snr_min=cfg.gaia_parallax_snr)
        catalog = parallax_to_distance_table(ra, dec, plx, g, cfg.gaia_parallax_snr, bp_rp=bp)
        catalog = catalog[list(_REQUIRED) + [c for c in _OPTIONAL if c in catalog.colnames]]
        o.save(f"{p}sigma_parallax", sigma_parallax_from_g(g))
    cat_cols = ["ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id"] + (
        ["bp_rp"] if "bp_rp" in catalog.colnames else [])
    o.save_table(f"{p}catalog", catalog, cat_cols)
    o.params(f"{p}catalog", n=len(catalog), columns=cat_cols)

    # catalog_xy
    cx, cy = wcs.world_to_pixel_values(catalog["ra"], catalog["dec"])
    o.save(f"{p}catalog_xy", np.column_stack([cx, cy]))

    # match_table, match_stats
    table, stats = cross_match(detected, catalog, wcs, cfg.match_tolerance_px)
    mcols = ["x", "y", "flux", "fwhm", "matched", "r_med_geo", "r_lo_geo", "r_hi_geo"] + (
        ["bp_rp"] if "bp_rp" in table.colnames else [])
    o.save_table(f"{p}match_table", table, mcols, bool_cols=("matched",))
    o.params(f"{p}match_stats", n_detected=stats.n_detected, n_matched=stats.n_matched,
             match_rate=stats.match_rate, median_offset_px=stats.median_offset_px,
             tolerance_px=cfg.match_tolerance_px)
    if stats.n_matched < 20:
        raise RuntimeError(f"{tag or 'colour'}: only {stats.n_matched} matches; fixture too weak")

    # strength, confidence, effective
    rm, rl, rh = (np.asarray(table[c]) for c in ("r_med_geo", "r_lo_geo", "r_hi_geo"))
    o.save(f"{p}strength", depth_strength(rm, cfg.p_low, cfg.p_high, cfg.neutral_strength))
    o.save(f"{p}confidence", confidence(rm, rl, rh))
    eff = effective_strength(rm, rl, rh, cfg.p_low, cfg.p_high, cfg.neutral_strength)
    o.save(f"{p}effective", eff)
    o.params(f"{p}effective", p_low=cfg.p_low, p_high=cfg.p_high, neutral=cfg.neutral_strength)

    # modulation
    mod = compute_modulation(eff, gains)
    o.save(f"{p}modulation", np.stack([mod.brightness, mod.size, mod.contrast, mod.saturation]))
    o.params(f"{p}modulation", rows=["brightness", "size", "contrast", "saturation"],
             gains=dict(brightness=gains.brightness, size=gains.size, contrast=gains.contrast,
                        saturation=gains.saturation, halo_gain=gains.halo_gain))

    # halo_profile (built from the very layer that is then rendered)
    halo = build_halo_profile(stars, table, cfg.base_sigma_px, cfg)
    if expect_empirical_halo is not None and halo.is_empirical != expect_empirical_halo:
        raise RuntimeError(f"{tag or 'colour'}: halo is_empirical={halo.is_empirical}, expected "
                           f"{expect_empirical_halo}; the star field must supply >= {cfg.halo_min_refs} clean refs")
    o.save(f"{p}halo_r_grid", halo.r_grid)
    o.save(f"{p}halo_values", halo.values)
    o.params(f"{p}halo_profile", is_empirical=bool(halo.is_empirical), base_sigma_px=cfg.base_sigma_px,
             halo_radius_sigma=cfg.halo_radius_sigma, halo_min_refs=cfg.halo_min_refs,
             halo_moffat_beta=cfg.halo_moffat_beta)

    # rendered_stars (must equal render_from_prep: the product path)
    rendered = render_stars(stars, table, mod, cfg.base_sigma_px, halo_profile=halo, halo_gain=gains.halo_gain)
    ref = render_from_prep(stars, table, cfg)
    if not np.array_equal(rendered, ref):
        raise RuntimeError("render_stars disagrees with render_from_prep")
    o.save(f"{p}rendered_stars", rendered, "f4")

    # budget, structural_depth, nebula_out
    budget = gaia_depth_budget(rm)
    o.save(f"{p}budget", [budget])
    sdep = structural_depth(starless.mean(axis=2) if starless.ndim == 3 else starless)
    o.save(f"{p}structural_depth", sdep, "f4")
    nebs = {}
    for key, (st, at, sc) in nebula_settings.items():
        nebs[key] = render_nebula(starless, table, strength=st, atmos=at, structure=sc)
        o.save(f"{p}nebula_out_{key}", nebs[key], "f4")
        o.params(f"{p}nebula_out_{key}", strength=st, atmos=at, structure=sc, budget=budget)
    if not (0.0 < budget):
        raise RuntimeError("depth budget is zero; the nebula goldens would be identity")

    # screen_blend, autostretch_u8 (first nebula setting)
    first = next(iter(nebs.values()))
    blend = screen_blend(rendered, first)
    o.save(f"{p}screen_blend", blend, "f4")
    u8 = autostretch(blend)
    o.save(f"{p}autostretch_u8", u8, "u1")
    return {"table": table, "stats": stats}


def edge_case(o: Out, rng):
    """64x48 colour frame with stars on/over every edge and corner: partial render windows,
    explicit budget (the table is too small for gaia_depth_budget), default Moffat halo."""
    h, w = ODD_H, ODD_W
    img = np.full((h, w, 3), 0.01)
    pos = [(1.3, 2.2), (62.5, 45.8), (32.0, 24.0), (10.7, 40.1), (55.2, 5.5), (0.0, 0.0), (63.0, 47.0)]
    for i, (x, y) in enumerate(pos):
        _add_star(img, x, y, 0.4 + 0.08 * i, np.array([1.0, 0.8 + 0.03 * i, 0.6 + 0.05 * i]))
    img += rng.normal(0, 0.002, img.shape)
    stars = np.clip(img, 0, 1).astype(np.float32).astype(np.float64)
    starless = make_starless(rng, w, h).astype(np.float32).astype(np.float64)
    r_med = np.array([150.0, 2200.0, 600.0, np.nan, 90.0, 1200.0, 3000.0])
    r_lo, r_hi = r_med * 0.93, r_med * 1.12
    r_lo[1], r_hi[1] = r_med[1] * 0.5, r_med[1] * 2.0  # a low-confidence star
    t = Table()
    t["x"], t["y"] = [p[0] for p in pos], [p[1] for p in pos]
    t["flux"] = np.linspace(2.0, 0.5, len(pos))
    t["r_med_geo"], t["r_lo_geo"], t["r_hi_geo"] = r_med, r_lo, r_hi
    cfg = GradeConfig(gains=DIALOG_GAINS)
    eff = effective_strength(r_med, r_lo, r_hi, cfg.p_low, cfg.p_high, cfg.neutral_strength)
    mod = compute_modulation(eff, DIALOG_GAINS)
    rendered = render_stars(stars, t, mod, cfg.base_sigma_px)  # halo_profile=None -> Moffat(beta 2.5)
    neb = render_nebula(starless, t, strength=2.0, atmos=0.5, structure=1.5, budget=0.8)
    blend = screen_blend(rendered, neb)
    o.save("odd_stars", stars, "f4")
    o.save_table("odd_table", t, ["x", "y", "flux", "r_med_geo", "r_lo_geo", "r_hi_geo"])
    o.save("odd_effective", eff)
    o.save("odd_modulation", np.stack([mod.brightness, mod.size, mod.contrast, mod.saturation]))
    o.save("odd_rendered_stars", rendered, "f4")
    o.save("odd_starless", starless, "f4")
    o.save("odd_nebula_out_s2_a0p5_t1p5", neb, "f4")
    o.save("odd_screen_blend", blend, "f4")
    o.save("odd_autostretch_u8", autostretch(blend), "u1")
    o.params("odd", width=w, height=h, halo="default Moffat (halo_profile=None)", budget=0.8,
             nebula=dict(strength=2.0, atmos=0.5, structure=1.5),
             gains=dict(brightness=0.65, size=0.85, contrast=0.12, saturation=0.4, halo_gain=1.0))
    # The Moffat fallback itself.
    m = moffat_profile(2.0, 2.5, 16.0)
    o.save("moffat_r_grid", m.r_grid)
    o.save("moffat_values", m.values)
    o.params("moffat", base_sigma_px=2.0, beta=2.5, radius_px=16.0)


def phase_c_cases(o: Out):
    """Extra cases for the Phase C ports (plan Tasks 6-9), drawn from their own seeded generator
    so every other golden stays byte-identical: DAOFIND peak separation (default 2.5*fwhm, 0 =
    kernel footprint, an odd circular footprint), the sharpness/roundness filters, a general
    elliptical kernel with border exclusion and an unscaled threshold, NaN input, an empty
    field; the render's window order on two overlapping stars; the halo's Moffat fallback."""
    from photutils.utils._convolution import _filter_data

    rng = np.random.default_rng(SEED + 2)
    dao_cols = ("x_centroid", "y_centroid", "flux", "peak", "sharpness", "roundness1", "roundness2")

    def field(w, h, stars, noise=0.002):
        yy, xx = np.mgrid[0:h, 0:w].astype(float)
        img = rng.normal(0.0, noise, (h, w))
        for x, y, amp, sx, sy in stars:
            img += amp * np.exp(-((xx - x) ** 2 / (2 * sx * sx) + (yy - y) ** 2 / (2 * sy * sy)))
        return img.astype(np.float32).astype(np.float64)

    def dao(name, data, **kw):
        found = DAOStarFinder(**kw)(data)
        rows = (np.zeros((0, 7)) if found is None
                else np.column_stack([np.asarray(found[c], float) for c in dao_cols]))
        o.save(name, rows)
        o.params(name, columns=list(dao_cols), n=len(rows),
                 **{k: (list(v) if isinstance(v, tuple) else v) for k, v in kw.items()})
        return len(rows)

    # 1. Peak separation. Two stars 4.9 px apart (inside 2.5*fwhm = 7.5), one isolated, one in a
    #    corner (cutouts and the circular footprint clipped by the frame).
    pair = field(48, 40, [(14.3, 20.6, 0.5, 1.3, 1.3), (19.1, 21.4, 0.3, 1.3, 1.3),
                          (34.7, 12.2, 0.4, 1.3, 1.3), (2.2, 37.4, 0.35, 1.3, 1.3)])
    o.save("dao_pair_input", pair, "f4")
    n_default = dao("dao_pair_default", pair, threshold=0.02, fwhm=3.0)
    n_zero = dao("dao_pair_minsep0", pair, threshold=0.02, fwhm=3.0, min_separation=0)
    dao("dao_pair_minsep3", pair, threshold=0.02, fwhm=3.0, min_separation=3.0)
    if not n_zero > n_default:
        raise RuntimeError("dao_pair: min_separation=0 must find more peaks than the 2.5*fwhm default")
    k3 = DAOStarFinder(threshold=0.02, fwhm=3.0).kernel
    o.save("dao_kernel_fwhm3_data", k3.data)
    o.save("dao_kernel_fwhm3_unmasked", k3.gaussian_kernel_unmasked)
    o.save("dao_kernel_fwhm3_mask", k3.mask.astype(bool), "?")
    o.params("dao_kernel_fwhm3", fwhm=3.0, rel_err=float(k3.rel_err), n_pixels=int(k3.n_pixels),
             x_sigma=float(k3.x_sigma), shape=list(k3.shape))
    o.save("dao_pair_conv", _filter_data(pair, k3.data, mode="constant", fill_value=0.0))

    # 2. Filters: a star, a hot pixel (sharpness > 1) and an elongated streak (roundness).
    rej = field(40, 32, [(12.4, 15.3, 0.4, 1.3, 1.3), (25.5, 24.2, 0.3, 3.5, 0.8)])
    rej[8, 28] = np.float32(rej[8, 28] + 0.5)
    o.save("dao_reject_input", rej, "f4")
    n_def = dao("dao_reject_default", rej, threshold=0.02, fwhm=3.0)
    n_sharp = dao("dao_reject_sharp_only", rej, threshold=0.02, fwhm=3.0, roundness_range=None)
    n_none = dao("dao_reject_nofilter", rej, threshold=0.02, fwhm=3.0, sharpness_range=None, roundness_range=None)
    if not (n_def < n_sharp < n_none):
        raise RuntimeError(f"dao_reject: expected default < sharp-only < no-filter rows, got {n_def}, {n_sharp}, {n_none}")

    # 3. General kernel: elliptical, rotated, wider, border exclusion, unscaled threshold, an
    #    explicit separation; on the colour field's luminance minus its sigma-clipped median.
    stars_rgb = np.load(o.dir / "stars_rgb.npy").astype(np.float64)
    lum = stars_rgb.mean(axis=2)
    _, med, _ = sigma_clipped_stats(lum, sigma=3.0)
    gen = dict(fwhm=5.0, ratio=0.6, theta=35.0, sigma_radius=2.0, exclude_border=True,
               scale_threshold=False, min_separation=4.0)
    dao("dao_general", lum - med, threshold=0.05, **gen)
    kg = DAOStarFinder(threshold=0.05, **gen).kernel
    o.save("dao_kernel_general_data", kg.data)
    o.params("dao_kernel_general", rel_err=float(kg.rel_err), n_pixels=int(kg.n_pixels), shape=list(kg.shape),
             x_sigma=float(kg.x_sigma), y_sigma=float(kg.y_sigma), **gen)
    o.save("dao_general_conv_pair", _filter_data(pair, kg.data, mode="constant", fill_value=0.0))

    # 4. NaN input (a background pixel and one beside the isolated star).
    nan_in = pair.copy()
    nan_in[5, 40] = np.nan
    nan_in[12, 37] = np.nan
    o.save("dao_nan_input", nan_in, "f4")
    dao("dao_nan", nan_in, threshold=0.02, fwhm=3.0)

    # 4b. Unit noise with a NaN block, a hugely negative threshold and no shape filters: every
    #     local maximum is a candidate, so the peak finder's ring verification, its cval-0 rule
    #     at the frame edge, both footprint parities, the NaN replacement, the finite filter and
    #     the marginal fit's non-positive-amplitude and |dx| > size/2 fallbacks are all taken.
    noise = rng.normal(0.0, 1.0, (32, 40)).astype(np.float32).astype(np.float64)
    noise[20:26, 4:10] = np.nan
    o.save("dao_noise_input", noise, "f4")
    loose = dict(threshold=-100.0, fwhm=3.0, sharpness_range=None, roundness_range=None)
    dao("dao_noise_default", noise, **loose)
    dao("dao_noise_minsep3", noise, min_separation=3.0, **loose)
    dao("dao_noise_minsep0", noise, min_separation=0, **loose)

    # 4b'. Two hand-built peak-finder edge cases (no shape filters):
    #   - a quartic bowl centred 6 px from the left edge: its convolved image has a NEGATIVE local
    #     maximum whose 7.5 px circle is clipped by the frame, so photutils' cval-0 rule rejects it;
    #   - two point sources 5 px apart on both axes: the brighter sits in the 16x16 box but outside
    #     the 7.5 px circle of the fainter, which is a peak only if the inscribed box (9x9 for an
    #     even footprint) stays inside the circle.
    yy, xx = np.mgrid[0:24, 0:24].astype(float)
    bowl = ((xx - 6.0) ** 2 + (yy - 12.0) ** 2) ** 2 / 256.0
    o.save("dao_edge_bowl_input", bowl, "f4")
    dao("dao_edge_bowl", bowl.astype(np.float32).astype(np.float64), threshold=-1e9, fwhm=3.0,
        sharpness_range=None, roundness_range=None)
    corner = np.zeros((40, 40))
    corner[15, 15], corner[20, 20] = 1.0, 2.0
    o.save("dao_corner_pair_input", corner, "f4")
    if dao("dao_corner_pair", corner, threshold=0.1, fwhm=3.0, sharpness_range=None, roundness_range=None) != 2:
        raise RuntimeError("dao_corner_pair: both point sources must be peaks")

    # 4c. The threshold is strict (data > threshold): set it to the faintest default peak's
    #     convolved value, unscaled, and that peak drops out.
    conv3 = _filter_data(pair, k3.data, mode="constant", fill_value=0.0)
    xy = DAOStarFinder._find_stars(conv3, k3, 0.02 * k3.rel_err, min_separation=7.5)
    faintest = float(min(conv3[int(y), int(x)] for x, y in xy))
    o.save("dao_pair_strict_threshold", [faintest])
    n_strict = dao("dao_pair_strict", pair, threshold=faintest, fwhm=3.0, scale_threshold=False)
    if n_strict != n_default - 1:
        raise RuntimeError("dao_pair_strict: the faintest peak must be the only one dropped")

    # 4d. Constant input: photutils returns None even when the threshold would pass every pixel.
    if DAOStarFinder(threshold=-1.0, fwhm=3.0)(np.full((20, 24), 0.3)) is not None:
        raise RuntimeError("constant input must give no detections")
    o.params("dao_constant", note="DAOStarFinder(threshold=-1, fwhm=3)(np.full((20, 24), 0.3)) is None")

    # 5. An empty field: sky noise only, nothing reaches 6 sigma.
    empty = field(32, 24, [])
    o.save("detect_empty_input", empty, "f4")
    if len(detect_stars(empty, 3.0, 6.0)) != 0:
        raise RuntimeError("detect_empty_input: detect_stars found a star in pure noise")

    # 6. Render order: two overlapping enlarged stars, rendered in table order and reversed.
    ov = np.full((24, 32, 3), 0.01)
    _add_star(ov, 13.6, 11.3, 0.5, np.array([1.0, 0.85, 0.6]))
    _add_star(ov, 17.2, 12.9, 0.35, np.array([0.55, 0.8, 1.0]))
    ov = np.clip(ov + rng.normal(0, 0.002, ov.shape), 0, 1).astype(np.float32).astype(np.float64)
    t = Table()
    t["x"], t["y"], t["flux"] = [13.6, 17.2], [11.3, 12.9], [3.1, 2.2]
    mod = compute_modulation(np.array([0.85, 0.55]), DIALOG_GAINS)
    fwd = render_stars(ov, t, mod, 2.0)
    t_rev = t[::-1]
    mod_rev = compute_modulation(np.array([0.55, 0.85]), DIALOG_GAINS)
    rev = render_stars(ov, t_rev, mod_rev, 2.0)
    if not np.abs(fwd - rev).max() > 1e-4:
        raise RuntimeError("overlap: render order made no difference; move the stars closer")
    o.save("overlap_stars", ov, "f4")
    o.save_table("overlap_table", t, ["x", "y", "flux"])
    o.save("overlap_modulation", np.stack([mod.brightness, mod.size, mod.contrast, mod.saturation]))
    o.save("overlap_rendered", fwd)  # float64: the one render golden at full precision
    o.save("overlap_rendered_reversed", rev, "f4")
    o.params("overlap", gains=dict(brightness=0.65, size=0.85, contrast=0.12, saturation=0.4, halo_gain=1.0),
             strength=[0.85, 0.55], halo="default Moffat (halo_profile=None)",
             max_abs_order_effect=float(np.abs(fwd - rev).max()))

    # 6b. The halo's reference-star selection, on a small mono field with a non-default config
    #     (base sigma 1, radius 4 sigma -> 9x9 stamps, halo_min_refs 3 -> stop after 9 stamps).
    #     Every star has a cored centre (the centre pixel is not the peak, so the normalisation
    #     acts) and a ring at r = 3 (so the monotone clamp acts), each with its own ring level
    #     (low on the special stars, high on the clean ones, so which stamps are stacked moves
    #     the median). The table carries, brightest first: a NaN-flux
    #     star, a star peaking at 0.925 (just saturated), stamps cut by the right and top edges,
    #     a dark stamp, a star with a 0.54-peak hot pixel at r = 5 in its stamp (a bright neighbour:
    #     rejected), a star with a 0.78-peak hot pixel exactly 4 px away (inside the 4-sigma
    #     core: kept); then more clean stars than the 9-stamp stop.
    hw, hh = 64, 48
    yy, xx = np.mgrid[0:hh, 0:hw].astype(float)
    halo_img = np.full((hh, hw), 0.01) + rng.normal(0.0, 0.002, (hh, hw))
    halo_img[38:47, 1:10] = -0.02  # the dark stamp's patch

    def cored_profile(r2, ring):
        return np.exp(-r2 / 2.0) - 0.5 * np.exp(-r2 / 0.5) + ring * np.exp(-(np.sqrt(r2) - 3.0) ** 2 / 0.5)

    def cored(x, y, amp, ring):
        return amp * cored_profile((xx - x) ** 2 + (yy - y) ** 2, ring)

    f1 = float(cored_profile(np.array(1.0), 0.2))  # an integer-centred star's peak / amp
    sat_amp = (0.925 - 0.01) / f1
    rows = []
    special = [(16.2, 26.3, np.nan, 0.5, 0.02), (36.0, 16.0, 9.0, sat_amp, 0.2),
               (60.0, 16.0, 8.5, 0.6, 0.2), (26.0, 44.0, 8.0, 0.6, 0.2), (5.0, 42.0, 7.5, None, 0.0),
               (46.0, 36.0, 7.0, 0.7, 0.02), (16.0, 6.0, 6.5, 0.7, 0.02)]
    for x, y, f, a, ring in special:
        if a is not None:
            halo_img += cored(x, y, a, ring)
        rows.append((x, y, f))
    peak_a = 0.7 * f1 + 0.01
    halo_img[39, 50] = 0.01 + 0.54 * peak_a  # (4, 3) from (46, 36): r = 5, far; 0.5-0.6 of the peak
    halo_img[6, 20] = 0.01 + 0.78 * peak_a   # 4 px from (16, 6): not "far" (r > 4 sigma), and below the peak
    clean = [(6.3, 6.2), (26.3, 6.4), (36.2, 6.3), (46.4, 6.2), (56.3, 6.1), (6.2, 16.4), (26.1, 16.3),
             (46.3, 16.4), (26.4, 26.3), (36.1, 26.4), (46.2, 26.1), (56.1, 26.3), (6.3, 26.2), (36.4, 36.1)]
    amps = rng.uniform(0.2, 0.65, len(clean))
    rings = rng.uniform(0.25, 0.4, len(clean))  # the accepted specials ring at 0.02: any swap moves the median
    for (x, y), a, ring in zip(clean, amps, rings):
        halo_img += cored(x, y, a, ring)
        rows.append((x, y, float(a * 10.0)))
    halo_img = halo_img.astype(np.float32).astype(np.float64)
    sat_peak = halo_img[12:21, 32:41].max()
    if not 0.9 <= sat_peak < 0.95:
        raise RuntimeError(f"halo_select: the saturation-edge star peaks at {sat_peak}, not in [0.9, 0.95)")
    ht = Table()
    ht["x"], ht["y"], ht["flux"] = [r[0] for r in rows], [r[1] for r in rows], [r[2] for r in rows]
    hcfg = GradeConfig(halo_min_refs=3, halo_radius_sigma=4.0)
    hp = build_halo_profile(halo_img, ht, 1.0, hcfg)
    if not hp.is_empirical:
        raise RuntimeError("halo_select: expected an empirical profile")
    o.save("halo_select_stars", halo_img, "f4")
    o.save_table("halo_select_table", ht, ["x", "y", "flux"])
    o.save("halo_select_r_grid", hp.r_grid)
    o.save("halo_select_values", hp.values)
    # Exactly halo_min_refs clean stamps: still empirical (the fallback needs fewer).
    three = ht[[len(special) + i for i in (0, 1, 2)]]
    hp3 = build_halo_profile(halo_img, three, 1.0, hcfg)
    if not hp3.is_empirical:
        raise RuntimeError("halo_select_three: three clean stamps must be enough for halo_min_refs=3")
    o.save("halo_select_three_values", hp3.values)
    # The same three plus one rejected stamp (the dark one, then the bright-neighbour one): a
    # fourth stamp would move the median, so these pin each rejection on its own.
    for name, row in (("dark", 4), ("neighbour", 5)):
        sub = ht[[row] + [len(special) + i for i in (0, 1, 2)]]
        hp4 = build_halo_profile(halo_img, sub, 1.0, hcfg)
        if not np.array_equal(hp4.values, hp3.values):
            raise RuntimeError(f"halo_select_plus_{name}: the {name} stamp must be rejected")
    o.params("halo_select", base_sigma_px=1.0, halo_min_refs=3, halo_radius_sigma=4.0,
             three_rows=[len(special), len(special) + 1, len(special) + 2],
             rejected_rows={"dark": 4, "neighbour": 5})

    # 7. The halo's Moffat fallback on the edge-case frame (too few clean reference stars).
    odd = np.load(o.dir / "odd_stars.npy").astype(np.float64)
    ot = Table()
    for c in ("x", "y", "flux"):
        ot[c] = np.load(o.dir / f"odd_table_{c}.npy")
    msgs = []

    class _Grab(logging.Handler):
        def emit(self, record):
            msgs.append(record.getMessage())

    psf_log = logging.getLogger("gaia_depth_grade.psf")
    grab = _Grab(level=logging.WARNING)
    psf_log.addHandler(grab)
    try:
        hp = build_halo_profile(odd, ot, 2.0, GradeConfig())
    finally:
        psf_log.removeHandler(grab)
    m = moffat_profile(2.0, 2.5, 16.0)
    if hp.is_empirical or not np.array_equal(hp.values, m.values) or len(msgs) != 1:
        raise RuntimeError("halo fallback: expected exactly one warning and the Moffat profile")
    o.params("halo_fallback", warning=msgs[0], stars="odd_stars", table="odd_table x/y/flux")


def primitives(o: Out, rng):
    """The numeric primitives the C++ core re-implements."""
    # A 48x64 field with a hot pixel, an edge ramp and noise: reflect-boundary behaviour matters.
    yy, xx = np.mgrid[0:48, 0:64]
    base = 0.2 + 0.004 * xx + 0.002 * yy + rng.normal(0, 0.05, (48, 64))
    base[10, 20] += 3.0
    base[47, 63] -= 1.0
    o.save("prim_input", base)
    for s in (0.8, 1.0, 2.0, 2.5, 4.0, 65.0):
        name = "gauss_" + str(s).replace(".", "p")
        o.save(name, gaussian_filter(base, s))
        o.params(name, sigma=s, mode="reflect", truncate=4.0)
    # axes=(0, 1) on an (H, W, 3) stack, as render/nebula use it.
    stack = rng.uniform(0, 1, (20, 17, 3))
    o.save("prim_stack3", stack)
    o.save("gauss_axes01_2", gaussian_filter(stack, sigma=2.0, axes=(0, 1)))

    # percentile: ties, outliers, tiny arrays.
    v = np.round(rng.normal(0, 1, 1000), 1)
    qs = np.array([0.0, 0.1, 5.0, 20.0, 50.0, 60.0, 95.0, 99.0, 99.5, 99.9, 100.0])
    o.save("pct_input", v)
    o.save("pct_q", qs)
    o.save("percentile", np.percentile(v, qs))
    for n in (1, 2, 5):
        small = rng.uniform(0, 1, n)
        o.save(f"pct_small{n}_input", small)
        o.save(f"pct_small{n}", np.percentile(small, qs))

    # order statistics: median, MAD (the 1.4826 factor the code uses), nanmedian over axis 0.
    m_in = np.concatenate([rng.normal(0, 1, 501), [40.0, -35.0]])
    med = float(np.median(m_in))
    mad = float(np.median(np.abs(m_in - med)))
    o.save("order_input", m_in)
    o.save("order_stats", [med, mad, 1.4826 * mad])
    o.params("order_stats", order=["median", "mad", "1.4826*mad"])
    st = rng.uniform(0, 1, (7, 17))
    st[rng.uniform(size=st.shape) < 0.25] = np.nan
    st[:, 3] = np.nan  # an all-NaN column: numpy warns and returns NaN, the code replaces it later
    o.save("nanmedian_input", st)
    with np.errstate(all="ignore"):
        import warnings
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)
            o.save("nanmedian", np.nanmedian(st, axis=0))

    # grey closing as the recon extinction code calls it (size=(15, 15)) and a small footprint.
    sm = gaussian_filter(np.clip(base, 0, None), 1.0)
    o.save("grey_closing_input", sm)
    o.save("grey_closing_15", grey_closing(sm, size=(15, 15)))
    o.save("grey_closing_5", grey_closing(sm, size=(5, 5)))

    # interp, both boundary modes the code uses.
    xp = np.array([0.0, 1.0, 2.5, 4.0])
    fp = np.array([1.0, 0.5, 0.2, 0.0])
    x = np.linspace(-1.0, 5.0, 31)
    o.save("interp_xp", xp)
    o.save("interp_fp", fp)
    o.save("interp_x", x)
    o.save("interp_clamp", np.interp(x, xp, fp))
    o.save("interp_left_fp0_right0", np.interp(x, xp, fp, left=fp[0], right=0.0))
    # HaloProfile.sample's table-lookup form, with a non-uniform query like r/zsize.
    xq = rng.uniform(-0.5, 6.0, 40)
    o.save("interp_xq", xq)
    o.save("interp_q_left_right", np.interp(xq, xp, fp, left=fp[0], right=0.0))


# --------------------------------------------------------------------------- recon

def _load_fixture(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(f"_gdg_fix_{path.stem}_{name}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    fn = getattr(mod, name)
    raw = getattr(fn, "__wrapped__", None) or getattr(fn, "_get_wrapped_function", lambda: fn)()
    return raw()


def recon(o: Out, rng):
    """Every function in recon/* on the plan's synthetic_blister fixture (tests/recon/conftest.py)."""
    b = _load_fixture(PY_TESTS / "recon" / "conftest.py", "synthetic_blister")
    ch, ion_star, lum, dust_in = b.channels, b.ionizer, b.lum, b.dust
    ny, nx = lum.shape
    o.save("recon_ha", ch.ha, "f4")
    o.save("recon_oiii", ch.oiii, "f4")
    if not np.array_equal(ch.sii, (0.5 * ch.ha).astype(np.float32)):
        raise RuntimeError("fixture sii is no longer 0.5*ha; store it")
    o.params("recon_sii", note="sii == float32(0.5 * ha) exactly in the blister fixture; asserted by the generator")
    o.save("recon_lum", lum, "f4")
    o.save("recon_dust_in", dust_in, "f4")
    o.params("recon", ny=ny, nx=nx, apex_xy=list(b.truth["apex_xy"]), R=b.truth["R"],
             pc_per_px=b.pc_per_px, ionizer=dict(x=ion_star.x, y=ion_star.y, distance_pc=ion_star.distance_pc,
                                                 source=ion_star.source),
             ratio_confidence=ch.ratio_confidence, is_linear=ch.is_linear)

    imap = ionization_ratio(ch)
    o.save("recon_oiii_fraction", imap.oiii_fraction, "f4")
    o.params("recon_ionization_ratio", confidence=imap.confidence, smooth_sigma=2.0, eps=1e-6)

    # pick_ionizer / ionizer_from_xy: scalars only. A small star table with colour and distance.
    n = 14
    tab = Table()
    tab["x"] = rng.uniform(0, nx, n)
    tab["y"] = rng.uniform(0, ny, n)
    tab["flux"] = 10 ** rng.uniform(-0.5, 1.5, n)
    tab["bp_rp"] = rng.normal(0.8, 0.7, n)
    tab["bp_rp"][3] = np.nan
    tab["r_med_geo"] = rng.uniform(400, 2500, n)
    tab["r_med_geo"][5] = np.nan
    fpf = FieldFootprint(10.0, 20.0, 0.05)

    def ion_dict(i):
        return dict(x=i.x, y=i.y, distance_pc=i.distance_pc, source=i.source)

    cases = {}
    cases["with_colour"] = ion_dict(pick_ionizer(tab, (ny, nx), fpf))
    nocol = tab.copy()
    nocol.remove_column("bp_rp")
    cases["no_bp_rp_column"] = ion_dict(pick_ionizer(nocol, (ny, nx), fpf))
    allnan = tab.copy()
    allnan["bp_rp"] = np.nan
    cases["all_nan_bp_rp"] = ion_dict(pick_ionizer(allnan, (ny, nx), fpf))
    cases["empty_table"] = ion_dict(pick_ionizer(tab[:0], (ny, nx), fpf))
    cases["none_table"] = ion_dict(pick_ionizer(None, (ny, nx), fpf))
    outside = tab.copy()
    outside["x"] = -5.0
    cases["all_outside_bounds"] = ion_dict(pick_ionizer(outside, (ny, nx), fpf))
    nofluxpos = tab.copy()
    nofluxpos["flux"] = -1.0
    cases["no_positive_flux"] = ion_dict(pick_ionizer(nofluxpos, (ny, nx), fpf))
    cases["from_xy"] = ion_dict(ionizer_from_xy(33.5, 44.25, 812.0))
    cases["from_xy_no_distance"] = ion_dict(ionizer_from_xy(1, 2, None))
    o.save_table("recon_ionizer_table", tab, ["x", "y", "flux", "bp_rp", "r_med_geo"])
    o.params("recon_pick_ionizer", shape_hw=[ny, nx], flux_weight=0.4, cases=cases)

    # geometry_prior, ionization_depth, emission_measure_relief
    L = np.clip(np.nan_to_num(lum.astype(np.float64)), 0.0, None)
    foot = rcavity._footprint_from_lum(L, 0.05)
    o.save("recon_footprint", foot, "?")
    apex = (float(ion_star.x), float(ion_star.y))
    o.save("recon_geometry_prior", rcavity.geometry_prior((ny, nx), apex, foot), "f4")
    o.save("recon_ionization_depth", rcavity.ionization_depth(imap), "f4")
    o.save("recon_emission_relief", rcavity.emission_measure_relief(L), "f4")
    # Non-default arguments on a crop (the blister's apex lies inside it) to keep the files small.
    cy0, cy1, cx0, cx1 = 40, 110, 10, 100
    sub_foot = foot[cy0:cy1, cx0:cx1]
    sub_apex = (apex[0] - cx0, apex[1] - cy0)
    o.save("recon_crop_footprint", sub_foot, "?")
    o.save("recon_crop_geometry_p3", rcavity.geometry_prior(sub_foot.shape, sub_apex, sub_foot, power=3.0), "f4")
    o.save("recon_crop_geometry_nomask", rcavity.geometry_prior(sub_foot.shape, sub_apex, None), "f4")
    sub_imap = type(imap)(oiii_fraction=imap.oiii_fraction[cy0:cy1, cx0:cx1], confidence=imap.confidence)
    o.save("recon_crop_ionization_depth_g1p5", rcavity.ionization_depth(sub_imap, gamma=1.5), "f4")
    o.save("recon_crop_emission_relief_g1", rcavity.emission_measure_relief(L[cy0:cy1, cx0:cx1], gain=1.0), "f4")
    o.params("recon_crop", y0=cy0, y1=cy1, x0=cx0, x1=cx1, apex_xy=list(sub_apex), geometry_power=3.0,
             ionization_gamma=1.5, em_gain=1.0)
    o.params("recon_terms", footprint_frac=0.05, geometry_power=2.0, ionization_gamma=1.0,
             em_sigma_small=1.0, em_sigma_large=4.0, em_gain=0.5, em_norm_pct=99.5)

    # dust
    o.save("recon_dust_structure", dust_from_structure(lum), "f4")
    hb = (0.2 * ch.ha.astype(np.float64) + 0.1 * ch.oiii.astype(np.float64)).astype(np.float32)
    o.save("recon_hb", hb, "f4")
    o.save("recon_dust_balmer", dust_from_balmer(ch.ha, hb), "f4")
    o.params("recon_dust", lane_px=15, sky_pct=5.0, bright_pct=99.0, surround_frac=0.05, case_b=2.86,
             balmer_saturation=3.0)

    # reconstruct_cavity: SHO default config in full; the other capture modes z + meta only.
    rcfg = ReconConfig()
    model = rcavity.reconstruct_cavity(ch, imap, ion_star, lum, dust_in, b.pc_per_px, rcfg)
    o.save("recon_cavity_z", model.z, "f4")
    o.save("recon_cavity_emissivity", model.emissivity, "f4")
    o.params("recon_cavity_dust", note="model.dust equals clip(recon_dust_in, 0, 1); asserted by the generator")
    if not np.array_equal(model.dust, np.clip(dust_in, 0, 1).astype(np.float32)):
        raise RuntimeError("model.dust is not clip(dust_in); store it")
    o.params("recon_cavity", apex_xy=list(model.apex_xy), pc_per_px=model.pc_per_px, meta=model.meta)

    ha, oiii, sii = (np.asarray(a, np.float64)[cy0:cy1, cx0:cx1] for a in (ch.ha, ch.oiii, ch.sii))
    lum_c, dust_c = lum[cy0:cy1, cx0:cx1], dust_in[cy0:cy1, cx0:cx1]
    ion_c = ionizer_from_xy(sub_apex[0], sub_apex[1], ion_star.distance_pc)
    variants = {
        "duoband": from_duoband((ha + oiii).astype(np.float32), (sii + oiii).astype(np.float32)),
        "osc": from_osc_rgb(np.stack([ha, oiii, oiii], axis=-1)),
        "stretched": extract_channels({"ha": np.sqrt(ha), "oiii": np.sqrt(oiii), "sii": np.sqrt(sii)}, "sho"),
    }
    for name, cs in variants.items():
        im = ionization_ratio(cs)
        m = rcavity.reconstruct_cavity(cs, im, ion_c, lum_c, dust_c, b.pc_per_px, rcfg)
        o.save(f"recon_crop_cavity_z_{name}", m.z, "f4")
        o.params(f"recon_cavity_{name}", apex_xy=list(m.apex_xy), meta=m.meta, ionization_confidence=im.confidence)
    # sii=None, no distance scale, no override ionizer (field-centre apex), non-default config.
    cs_nosii = from_osc_rgb(np.stack([ha, oiii, oiii], axis=-1))
    im_nosii = ionization_ratio(cs_nosii)
    centre = pick_ionizer(None, lum_c.shape, fpf)
    cfg2 = ReconConfig(w_geometry=0.7, w_ionization=1.2, w_emission=0.3, emission_sign=-1.0, w_dust=0.5,
                       footprint_frac=0.1, geometry_power=1.5, ionization_gamma=0.8)
    m2 = rcavity.reconstruct_cavity(cs_nosii, im_nosii, centre, lum_c, dust_c, None, cfg2)
    o.save("recon_crop_cavity_z_cfg2", m2.z, "f4")
    o.params("recon_cavity_cfg2", apex_xy=list(m2.apex_xy), pc_per_px=m2.pc_per_px, meta=m2.meta,
             config=dict(w_geometry=0.7, w_ionization=1.2, w_emission=0.3, emission_sign=-1.0, w_dust=0.5,
                         footprint_frac=0.1, geometry_power=1.5, ionization_gamma=0.8))

    # Channel ingest on small arrays (24 x 18).
    r2 = np.random.default_rng(SEED + 1)
    sh = (18, 24)
    sha, soiii, ssii = (r2.uniform(0, 0.05, sh) for _ in range(3))
    sha[4:10, 4:12] += 0.3
    soiii[8:15, 10:20] += 0.25
    ssii[2:7, 15:22] += 0.2
    for nm, a in (("ha", sha), ("oiii", soiii), ("sii", ssii)):
        o.save(f"chan_{nm}", a, "f4")
    d1 = (sha + soiii).astype(np.float32)
    d2 = (ssii + soiii).astype(np.float32)
    rgb = np.stack([sha, 0.7 * soiii, soiii], axis=-1).astype(np.float32)
    o.save("chan_d1", d1, "f4")
    o.save("chan_d2", d2, "f4")
    o.save("chan_rgb", rgb, "f4")
    for nm, cs in (("sho", from_mono_sho(sha, soiii, ssii)), ("duoband", from_duoband(d1, d2)),
                   ("osc", from_osc_rgb(rgb))):
        o.save(f"chan_out_{nm}_ha", cs.ha, "f4")
        o.save(f"chan_out_{nm}_oiii", cs.oiii, "f4")
        if cs.sii is not None:
            o.save(f"chan_out_{nm}_sii", cs.sii, "f4")
        o.params(f"chan_out_{nm}", ratio_confidence=cs.ratio_confidence, is_linear=cs.is_linear,
                 sii_present=cs.sii is not None)
    ex = extract_channels({"ha": sha ** 0.3, "oiii": soiii ** 0.3, "sii": ssii ** 0.3}, "sho")
    o.params("chan_extract_stretched", ratio_confidence=ex.ratio_confidence, is_linear=ex.is_linear)
    lin = {
        "linear_ha": looks_linear(sha), "stretched_ha": looks_linear(sha ** 0.3),
        "flat": looks_linear(np.full(sh, 0.2)), "ramp": looks_linear(np.linspace(0, 1, 500).reshape(20, 25)),
    }
    o.params("looks_linear", threshold=0.10, results=lin)


def selftest_npy(o: Out):
    """Files for test_npy.cpp (Npy.h round trips and refusals)."""
    np.save(o.dir / "npy_selftest_f8.npy", np.arange(6, dtype="<f8").reshape(3, 2))
    np.save(o.dir / "npy_selftest_b1.npy", np.array([[1, 0, 1], [0, 0, 1]], dtype=bool))
    np.save(o.dir / "npy_selftest_f4.npy", np.array([0.0, 0.5, 1.0, -2.25], dtype="<f4"))
    np.save(o.dir / "npy_selftest_i8.npy", np.array([-7, 0, 123456789012], dtype="<i8"))
    np.save(o.dir / "npy_selftest_u1.npy", np.array([[0, 1], [128, 255]], dtype="u1"))
    np.save(o.dir / "npy_selftest_scalar.npy", np.array(3.5))
    with open(o.dir / "npy_selftest_v2.npy", "wb") as fh:
        np.lib.format.write_array(fh, np.array([1.0, 2.0]), version=(2, 0))
    np.save(o.dir / "npy_selftest_fortran.npy", np.asfortranarray(np.arange(6, dtype="<f8").reshape(3, 2)))


def tap_strings(o: Out):
    """Plan Task 13: the online source's exact strings. build_adql (Python repr floats in an
    f-string) and the cache file name GaiaStarSource._cache_path derives (%.6f key + mag repr,
    SHA-1, first 16 hex), for footprints whose floats exercise repr's fixed/exponent switch."""
    import tempfile
    from gaia_depth_grade.distances import GaiaStarSource, build_adql
    footprints = [
        (10.0, 20.0, 0.033138896900215445),
        (350.1671987228608, 61.22006490843979, 0.7512345678901234),
        (0.0, -0.0001, 1e-05),
        (359.99999999999994, -89.5, 12.5),
        (123.456, 1e-17, 1e16),
        (0.1, 2.5e-07, 0.30000000000000004),
    ]
    cases = []
    with tempfile.TemporaryDirectory() as tmp:
        for ra, dec, r in footprints:
            fp = FieldFootprint(center_ra=ra, center_dec=dec, radius_deg=r)
            for top, gt, lt in ((600000, None, None), (600000, None, 18.0), (2000, 12.345, 17.5), (5, 9.0, None)):
                cases.append({"footprint": [ra, dec, r], "top": top, "mag_gt": gt, "mag_lt": lt,
                              "adql": build_adql(fp, top, mag_gt=gt, mag_lt=lt)})
            for mag in (None, 18.0, 17.25):
                name = Path(GaiaStarSource(tmp, mag_limit=mag)._cache_path(fp)).name
                cases.append({"footprint": [ra, dec, r], "cache_mag": mag, "cache_file": name})
    o.params("tap", cases=cases)


# --------------------------------------------------------------------------- manifest

def _git(*args) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args], check=True, capture_output=True, text=True).stdout.strip()


def write_manifest(o: Out, mode: str, argv: list[str]) -> None:
    selftest = sorted(p.name for p in o.dir.glob("npy_selftest_*.npy")) if mode == "synthetic" else []
    manifest = {
        "generator": "modules/gaia-depth-grade/test/golden/make_goldens.py",
        "mode": mode,
        "arguments": argv,
        "python_core": {
            "repo_head": _git("rev-parse", "HEAD"),
            "core_last_change_commit": _git("log", "-1", "--format=%H", "--", "gaia-depth-grade/src"),
            "core_tree_dirty": bool(_git("status", "--porcelain", "--", "gaia-depth-grade/src")),
        },
        "versions": {"python": sys.version.split()[0], "numpy": np.__version__, "scipy": scipy.__version__,
                     "astropy": astropy.__version__, "photutils": photutils.__version__},
        "seed": SEED,
        "field": {"width": FIELD_W, "height": FIELD_H, "n_stars": N_STARS, "odd_width": ODD_W,
                  "odd_height": ODD_H},
        "image_storage": "images float32 (inputs are float32-exact); outputs computed in float64 then stored float32",
        "stages": sorted(o.stage_json),
        "files": dict(sorted(o.files.items())),
        "selftest_files": selftest,
    }
    (o.dir / "stage.json").write_text(json.dumps(o.stage_json, indent=1, sort_keys=True, default=_jsonable) + "\n")
    # MANIFEST.json sits one level above the data directory (test/golden/MANIFEST.json) for the
    # synthetic set; real-data sets keep theirs next to the data.
    mpath = (o.dir.parent / "MANIFEST.json") if mode == "synthetic" else (o.dir / "MANIFEST.json")
    mpath.write_text(json.dumps(manifest, indent=1, sort_keys=True, default=_jsonable) + "\n")
    total = sum(p.stat().st_size for p in o.dir.glob("*") if p.is_file())
    print(f"wrote {len(o.files)} stage arrays + {len(selftest)} selftest files, {total / 1e6:.2f} MB -> {o.dir}")
    print(f"manifest: {mpath}")


# --------------------------------------------------------------------------- modes

def synthetic(outdir: Path, argv) -> None:
    rng = np.random.default_rng(SEED)
    o = Out(outdir)
    hdr = field_wcs(FIELD_W, FIELD_H)
    o.params("wcs", header={k: (hdr[k] if not isinstance(hdr[k], float) else float(hdr[k])) for k in hdr},
             note="same recipe as the simple_wcs_header fixture, sized to the image")
    # The fixture itself (200 x 300), for the footprint edge case on a non-square, non-field WCS.
    fixture_hdr = _load_fixture(PY_TESTS / "conftest.py", "simple_wcs_header")
    fx = field_footprint(load_wcs(fixture_hdr), (200, 300))
    o.save("fixture_footprint", [fx.center_ra, fx.center_dec, fx.radius_deg])
    o.params("fixture_footprint", header={k: fixture_hdr[k] for k in fixture_hdr}, shape_hw=[200, 300],
             order=["center_ra", "center_dec", "radius_deg"])

    stars_rgb, truth = make_star_field(rng)
    stars_rgb = stars_rgb.astype(np.float32).astype(np.float64)
    starless_rgb = make_starless(rng).astype(np.float32).astype(np.float64)
    stars_mono = stars_rgb.mean(axis=2).astype(np.float32).astype(np.float64)
    starless_mono = starless_rgb.mean(axis=2).astype(np.float32).astype(np.float64)
    o.save("stars_rgb", stars_rgb, "f4")
    o.save("starless_rgb", starless_rgb, "f4")
    o.save("mono_stars", stars_mono, "f4")
    o.save("mono_starless", starless_mono, "f4")
    o.params("inputs", seed=SEED, n_stars=N_STARS, truth_kinds={k: sum(1 for t in truth if t[3] == k)
                                                                for k in ("bright", "saturated", "field")},
             mono="mean of the three float32 channels, rounded to float32", sky_noise_sigma=0.002)

    # One catalogue drives both cases (the colour stars' detections; mono detections are
    # near-identical but the mono case re-matches against the same catalogue on its own table).
    wcs = load_wcs(hdr)
    detected = detect_stars(stars_rgb.mean(axis=2), 3.0, 6.0)
    raw = make_catalog(rng, detected, wcs, stars_rgb.shape[:2])

    res = run_pipeline(o, "", stars_rgb, starless_rgb, hdr, raw, gains=DIALOG_GAINS,
                       nebula_settings=NEBULA_SETTINGS, expect_empirical_halo=True)
    # Mono: engine-default gains (covers the other gain set), and only the (2, 0.5, 1.5) nebula.
    run_pipeline(o, "mono_", stars_mono, starless_mono, hdr, raw, gains=ENGINE_GAINS,
                 nebula_settings={"s2_a0p5_t1p5": NEBULA_SETTINGS["s2_a0p5_t1p5"]}, expect_empirical_halo=True)
    o.params("summary", n_detected=res["stats"].n_detected, n_matched=res["stats"].n_matched)

    edge_case(o, rng)
    primitives(o, rng)
    recon(o, rng)
    phase_c_cases(o)
    tap_strings(o)
    selftest_npy(o)
    write_manifest(o, "synthetic", argv)


def _read_image(path: str):
    with fits.open(path) as hdul:
        data = hdul[0].data.astype(float)
        header = hdul[0].header
    if data.ndim == 3:  # FITS stores colour as (3, ny, nx)
        data = np.moveaxis(data, 0, -1)
    return data, header


def real(stars_path: str, starless_path: str, catalog_path: str, outdir: Path, argv) -> None:
    """Goldens from a real capture: the same stages, with the catalogue read from a PI-Gaia
    TSV export (offline path) or an online Bailer-Jones ECSV (columns ra, dec, r_*_geo,
    source_id [, bp_rp])."""
    o = Out(outdir)
    stars, header = _read_image(stars_path)
    starless, _ = _read_image(starless_path)
    cfg = GradeConfig(gains=DIALOG_GAINS)
    if catalog_path.endswith(".ecsv"):
        catalog = Table.read(catalog_path, format="ascii.ecsv")
        missing = set(_REQUIRED) - set(catalog.colnames)
        if missing:
            raise RuntimeError(f"{catalog_path} is missing columns {sorted(missing)}")
        catalog = catalog[list(_REQUIRED) + [c for c in _OPTIONAL if c in catalog.colnames]]
    else:
        wcs = load_wcs(header)
        fpr = field_footprint(wcs, (stars.shape[0], stars.shape[1]))
        catalog = LocalGaiaSource(catalog_path, cfg.gaia_mag_limit, cfg.gaia_parallax_snr).distances_for(fpr)
        o.params("real", footprint=dict(ra=fpr.center_ra, dec=fpr.center_dec, radius_deg=fpr.radius_deg))
    o.params("real", stars=Path(stars_path).name, starless=Path(starless_path).name,
             catalog=Path(catalog_path).name, shape=list(stars.shape))
    run_pipeline(o, "", stars, starless, header, catalog, gains=DIALOG_GAINS, nebula_settings=NEBULA_SETTINGS,
                 expect_empirical_halo=None)
    write_manifest(o, "real", argv)


def main(argv: list[str]) -> int:
    logging.basicConfig(level=logging.WARNING)
    if len(argv) == 3 and argv[1] == "synthetic":
        synthetic(Path(argv[2]), argv[1:])
        return 0
    if len(argv) == 6 and argv[1] == "real":
        real(argv[2], argv[3], argv[4], Path(argv[5]), argv[1:])
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
