from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from astropy.table import Table
from scipy.spatial import cKDTree


@dataclass(frozen=True)
class MatchStats:
    n_detected: int
    n_matched: int
    match_rate: float
    median_offset_px: float


def cross_match(detected: Table, catalog: Table, wcs, tolerance_px: float):
    out = detected.copy()
    n = len(out)
    out["matched"] = np.zeros(n, dtype=bool)
    for col in ("r_med_geo", "r_lo_geo", "r_hi_geo"):
        out[col] = np.full(n, np.nan, dtype=float)

    if n == 0 or len(catalog) == 0:
        return out, MatchStats(n, 0, 0.0 if n else 0.0, float("nan"))

    cat_x, cat_y = wcs.world_to_pixel_values(catalog["ra"], catalog["dec"])
    det_xy = np.column_stack([np.asarray(out["x"]), np.asarray(out["y"])])
    tree = cKDTree(det_xy)
    dist, idx = tree.query(np.column_stack([cat_x, cat_y]), k=1)

    # each detection keeps its closest catalog source within tolerance
    best = {}
    for ci, (d, di) in enumerate(zip(dist, idx)):
        if d > tolerance_px:
            continue
        if di not in best or d < best[di][0]:
            best[di] = (d, ci)

    offsets = []
    for di, (d, ci) in best.items():
        out["matched"][di] = True
        out["r_med_geo"][di] = catalog["r_med_geo"][ci]
        out["r_lo_geo"][di] = catalog["r_lo_geo"][ci]
        out["r_hi_geo"][di] = catalog["r_hi_geo"][ci]
        offsets.append(d)

    n_matched = len(best)
    return out, MatchStats(
        n_detected=n,
        n_matched=n_matched,
        match_rate=n_matched / n,
        median_offset_px=float(np.median(offsets)) if offsets else float("nan"),
    )
