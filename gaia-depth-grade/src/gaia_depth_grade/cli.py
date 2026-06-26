from __future__ import annotations

import argparse
import json
import logging

import numpy as np
from astropy.io import fits

from . import __version__
from .cache import load_prep, save_prep
from .config import Gains, GradeConfig, load_config
from .display import autostretch, screen_blend, write_png
from .distances import DistanceSource, GaiaStarSource
from .pipeline import prepare_grade, render_from_prep

log = logging.getLogger(__name__)
HONESTY = "GAIA depth-derived; physically motivated, not per-pixel correct for gas"


def grade_array(image, header, config: GradeConfig, source: DistanceSource):
    """One-shot grade: prepare (detect+query+match) then render. Equivalent to
    `prepare_grade` followed by `render_from_prep`."""
    table, qa = prepare_grade(image, header, config, source)
    graded = render_from_prep(image, table, config)
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


def _render_config(args) -> GradeConfig:
    """Build a render-stage config from CLI gain args (override defaults)."""
    b, s, c, sat = (float(v) for v in args.gains.split(","))
    return GradeConfig(
        gains=Gains(brightness=b, size=s, contrast=c, saturation=sat),
        p_low=args.p_low, p_high=args.p_high, base_sigma_px=args.base_sigma)


def _add_render_args(sp):
    sp.add_argument("--gains", required=True, help="brightness,size,contrast,saturation")
    sp.add_argument("--p-low", dest="p_low", type=float, default=5.0)
    sp.add_argument("--p-high", dest="p_high", type=float, default=95.0)
    sp.add_argument("--base-sigma", dest="base_sigma", type=float, default=2.0)


def main(argv=None):
    p = argparse.ArgumentParser("gaia_depth_grade")
    # Bare version string: the PJSR bootstrap probes `<sidecar> --version` and
    # compares the output verbatim to the pinned SIDECAR_VERSION. The version
    # action fires (and exits) before the required-subcommand check below.
    p.add_argument("--version", action="version", version=__version__)
    sub = p.add_subparsers(dest="cmd", required=True)

    for name in ("grade", "debug"):
        sp = sub.add_parser(name)
        sp.add_argument("input"); sp.add_argument("output")
        sp.add_argument("--config", default=None)

    pp = sub.add_parser("prepare")
    pp.add_argument("stars"); pp.add_argument("cache_dir")
    pp.add_argument("--config", default=None)

    rp = sub.add_parser("render")
    rp.add_argument("cache_dir"); rp.add_argument("stars"); rp.add_argument("output")
    _add_render_args(rp)

    vp = sub.add_parser("preview")
    vp.add_argument("cache_dir"); vp.add_argument("stars"); vp.add_argument("starless")
    vp.add_argument("out_full")
    vp.add_argument("--inset", default=None)
    vp.add_argument("--region", default=None, help="x,y,w,h (1:1 inset crop)")
    vp.add_argument("--max-width", dest="max_width", type=int, default=800)
    _add_render_args(vp)

    args = p.parse_args(argv)
    logging.basicConfig(level=logging.INFO)

    if args.cmd == "debug":
        # Phase 1 has no depth-colored overlay yet. Fail loudly rather than
        # silently behaving like `grade` (no silent fallbacks).
        raise SystemExit("debug mode (depth-colored overlay) is not implemented in Phase 1")

    if args.cmd == "grade":
        cfg = load_config(args.config)
        image, header = _read_image(args.input)
        source = GaiaStarSource(cfg.cache_dir, cfg.gaia_mag_limit)
        graded, qa = grade_array(image, header, cfg, source)
        write_fits(args.output, graded, header, qa)
        log.info("wrote %s (qa: %s)", args.output, qa)
        return 0

    if args.cmd == "prepare":
        cfg = load_config(args.config)
        image, header = _read_image(args.stars)
        source = GaiaStarSource(cfg.cache_dir, cfg.gaia_mag_limit)
        table, qa = prepare_grade(image, header, cfg, source)
        save_prep(args.cache_dir, table, qa)
        log.info("prepared %s -> %s (qa: %s)", args.stars, args.cache_dir, qa)
        return 0

    if args.cmd == "render":
        cfg = _render_config(args)
        table, qa = load_prep(args.cache_dir)
        image, header = _read_image(args.stars)
        graded = render_from_prep(image, table, cfg)
        write_fits(args.output, graded, header, qa)
        log.info("rendered %s", args.output)
        return 0

    if args.cmd == "preview":
        cfg = _render_config(args)
        table, _ = load_prep(args.cache_dir)
        stars, _ = _read_image(args.stars)
        starless, _ = _read_image(args.starless)
        graded = render_from_prep(stars, table, cfg)
        stretched = autostretch(screen_blend(graded, starless))
        write_png(stretched, args.out_full, max_width=args.max_width)
        if args.inset and args.region:
            x, y, w, h = (int(v) for v in args.region.split(","))
            write_png(stretched, args.inset, region=(x, y, w, h))
        log.info("preview -> %s", args.out_full)
        return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
