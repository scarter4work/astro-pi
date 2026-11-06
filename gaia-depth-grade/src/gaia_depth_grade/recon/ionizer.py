"""Exciting-star (ionizer) identification.

The ionizer is the hot blue star that carves the HII cavity: its position anchors
the cavity apex and its Gaia distance anchors absolute scale.
"""

from __future__ import annotations

import numpy as np

from ..wcs import FieldFootprint
from .model import Ionizer

# Scoring (lower is better): score = bp_rp - _FLUX_WEIGHT * log10(flux / flux_max).
# A star 10x fainter must be 0.4 mag bluer in BP-RP to win.
_FLUX_WEIGHT = 0.4


def ionizer_from_xy(x: float, y: float, distance_pc: float | None) -> Ionizer:
    return Ionizer(x=float(x), y=float(y), distance_pc=distance_pc, source="override")


def _distance(table, i: int) -> float | None:
    if "r_med_geo" not in table.colnames:
        return None
    d = float(table["r_med_geo"][i])
    return d if np.isfinite(d) else None


def pick_ionizer(table, shape: tuple[int, int], footprint: FieldFootprint) -> Ionizer:
    """Pick the bluest luminous star in the field.

    Footprint membership: the table holds pixel coordinates, so "inside the footprint"
    means inside the image bounds ``0 <= x < nx, 0 <= y < ny`` (``shape`` is (ny, nx));
    the footprint cone is the image's own sky area, so no WCS round trip is needed.

    ``bp_rp`` may be absent or all-NaN (stale cache, no-colour capture): then the
    brightest star by ``flux`` is returned with ``source="gaia-bright"``.
    No usable star: the image-centre apex with ``source="field-center"``.
    """
    ny, nx = shape
    centre = Ionizer(x=nx / 2.0, y=ny / 2.0, distance_pc=None, source="field-center")
    if table is None or len(table) == 0:
        return centre

    x = np.asarray(table["x"], dtype=float)
    y = np.asarray(table["y"], dtype=float)
    flux = np.asarray(table["flux"], dtype=float)
    ok = (np.isfinite(x) & np.isfinite(y) & np.isfinite(flux) & (flux > 0)
          & (x >= 0) & (x < nx) & (y >= 0) & (y < ny))
    if not ok.any():
        return centre

    if "bp_rp" in table.colnames:
        col = np.asarray(table["bp_rp"], dtype=float)
        coloured = ok & np.isfinite(col)
        if coloured.any():
            fmax = flux[coloured].max()
            score = np.full(len(table), np.inf)
            score[coloured] = col[coloured] - _FLUX_WEIGHT * np.log10(flux[coloured] / fmax)
            i = int(np.argmin(score))
            return Ionizer(float(x[i]), float(y[i]), _distance(table, i), "gaia-blue")

    i = int(np.argmax(np.where(ok, flux, -np.inf)))
    return Ionizer(float(x[i]), float(y[i]), _distance(table, i), "gaia-bright")
