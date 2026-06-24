from __future__ import annotations

import argparse
import json
import logging

import numpy as np
from astropy.io import fits

from .config import GradeConfig, load_config
from .detect import detect_stars
from .distances import DistanceSource, GaiaStarSource
from .match import cross_match
from .modulate import compute_modulation
from .render import render_stars
from .transform import effective_strength
from .wcs import field_footprint, load_wcs

log = logging.getLogger(__name__)
HONESTY = "GAIA depth-derived; physically motivated, not per-pixel correct for gas"


def _luminance(image: np.ndarray) -> np.ndarray:
    return image.mean(axis=2) if image.ndim == 3 else image


def grade_array(image, header, config: GradeConfig, source: DistanceSource):
    wcs = load_wcs(header)
    lum = _luminance(image)
    detected = detect_stars(lum, config.detect_fwhm, config.detect_threshold_sigma)
    fp = field_footprint(wcs, lum.shape)
    catalog = source.distances_for(fp)
    matched, stats = cross_match(detected, catalog, wcs, config.match_tolerance_px)
    strength = effective_strength(
        np.asarray(matched["r_med_geo"]), np.asarray(matched["r_lo_geo"]),
        np.asarray(matched["r_hi_geo"]), config.p_low, config.p_high, config.neutral_strength)
    modulation = compute_modulation(strength, config.gains)
    graded = render_stars(image, matched, modulation, config.base_sigma_px)

    low = stats.match_rate < config.min_match_rate
    if low:
        log.warning("LOW MATCH RATE %.2f < %.2f — depth grade is unreliable",
                    stats.match_rate, config.min_match_rate)
    qa = {
        "n_detected": stats.n_detected, "n_matched": stats.n_matched,
        "match_rate": stats.match_rate, "median_offset_px": stats.median_offset_px,
        "low_match_warning": bool(low),
    }
    return graded, qa


def write_fits(path, image, header, qa):
    data = np.moveaxis(image, -1, 0) if image.ndim == 3 else image
    hdr = header.copy()
    hdr["DEPTHTAG"] = HONESTY   # astropy CONTINUE stores the full verbatim string
    hdr.add_history(HONESTY)
    fits.PrimaryHDU(data=data.astype(np.float32), header=hdr).writeto(path, overwrite=True)
    with open(path + ".qa.json", "w") as fh:
        json.dump(qa, fh, indent=2)


def _read_image(path):
    with fits.open(path) as hdul:
        data = hdul[0].data.astype(float)
        header = hdul[0].header
    if data.ndim == 3:               # FITS stores color as (3, ny, nx)
        data = np.moveaxis(data, 0, -1)
    return data, header


def main(argv=None):
    p = argparse.ArgumentParser("gaia_depth_grade")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("grade", "debug"):
        sp = sub.add_parser(name)
        sp.add_argument("input"); sp.add_argument("output")
        sp.add_argument("--config", default=None)
    args = p.parse_args(argv)

    if args.cmd == "debug":
        # Phase 1 has no depth-colored overlay yet. Fail loudly rather than
        # silently behaving like `grade` (no silent fallbacks).
        raise SystemExit("debug mode (depth-colored overlay) is not implemented in Phase 1")

    logging.basicConfig(level=logging.INFO)
    cfg = load_config(args.config)
    image, header = _read_image(args.input)
    source = GaiaStarSource(cfg.cache_dir)
    graded, qa = grade_array(image, header, cfg, source)
    write_fits(args.output, graded, header, qa)
    log.info("wrote %s (qa: %s)", args.output, qa)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
