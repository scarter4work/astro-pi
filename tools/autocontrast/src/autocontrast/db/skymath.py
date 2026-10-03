"""Spherical geometry shared by the fingerprint store and the gallery index.

Both do §5.1 cone-overlap tests. The haversine lived privately in ``store.py``;
it is here so the two callers cannot silently diverge.
"""

from __future__ import annotations

import numpy as np


def separation_arcmin(ra1: float, dec1: float, ra2: float, dec2: float) -> float:
    """Great-circle separation in arcminutes (haversine).

    Handles RA wrap and the cos(dec) foreshortening for free — a plain coordinate
    difference gets both wrong.
    """
    r1, d1, r2, d2 = np.radians([ra1, dec1, ra2, dec2])
    a = np.sin((d2 - d1) / 2) ** 2 + np.cos(d1) * np.cos(d2) * np.sin((r2 - r1) / 2) ** 2
    return float(np.degrees(2 * np.arcsin(np.sqrt(a))) * 60.0)


def cones_overlap(sep_arcmin: float, radius_a_arcmin: float, radius_b_arcmin: float) -> bool:
    """Two cones overlap when their center separation is at most the sum of radii.

    Boundary-inclusive: exactly touching counts as overlapping, matching
    ``FingerprintStore.cone_search``.
    """
    return sep_arcmin <= radius_a_arcmin + radius_b_arcmin
