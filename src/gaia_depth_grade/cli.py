from __future__ import annotations

import argparse
import json
import logging

import numpy as np
from astropy.io import fits

from .config import GradeConfig, load_config
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
