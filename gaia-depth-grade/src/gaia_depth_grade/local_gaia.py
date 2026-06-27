"""Offline distance source: PixInsight's local Gaia DR3 database.

PI's `Gaia` process can cone-search the local DR3 `.bin` files that every PI user
already has (plate-solving requires them) and write a tab-separated table. That
record carries `parallax` but NOT `parallax_error` or `source_id`, so we cannot
reproduce the online Bailer-Jones geometric distances. Instead we take the
honest parallax route: distance = 1000/parallax (pc), gated by a parallax
signal-to-noise cut, with the per-star uncertainty SYNTHESIZED from a published
Gaia DR3 sigma_parallax-vs-G relation so the pipeline's confidence weighting
(which damps the grade for uncertain stars) still works.

This trades Bailer-Jones accuracy for being fully offline and fast — well suited
to a *relative* depth grade on the brighter stars the magnitude cut keeps.
"""
from __future__ import annotations

import logging

import numpy as np
from astropy.table import Table

from .distances import DistanceSource, _REQUIRED
from .wcs import FieldFootprint

log = logging.getLogger(__name__)

# Median Gaia DR3 parallax uncertainty (mas) as a function of G magnitude.
# Anchored to the published EDR3/DR3 medians (Lindegren et al. 2021 / ESA Gaia
# DR3 performance). Used only to derive a confidence weight, not the distance.
_G_KNOTS = np.array([6.0, 13.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0, 21.0])
_SIGMA_PI_KNOTS = np.array([0.02, 0.02, 0.025, 0.035, 0.05, 0.08, 0.15, 0.30, 0.65])


def sigma_parallax_from_g(g_mag: np.ndarray) -> np.ndarray:
    """Approximate sigma_parallax (mas) from G; clamps outside the knot range."""
    g = np.asarray(g_mag, dtype=float)
    return np.interp(g, _G_KNOTS, _SIGMA_PI_KNOTS)


def parallax_to_distance_table(ra, dec, parallax_mas, g_mag, snr_min=5.0):
    """Build the distance table the pipeline expects from raw local-Gaia columns.

    distance = 1000/parallax (pc); r_lo/r_hi propagate a synthesized sigma so
    `confidence()` damps low-S/N stars. Sources failing the cut (non-positive
    parallax, or parallax/sigma < snr_min) are dropped LOUDLY — never silently.
    """
    ra = np.asarray(ra, dtype=float)
    dec = np.asarray(dec, dtype=float)
    plx = np.asarray(parallax_mas, dtype=float)
    g = np.asarray(g_mag, dtype=float)

    sigma = sigma_parallax_from_g(g)
    with np.errstate(divide="ignore", invalid="ignore"):
        snr = plx / sigma
    keep = np.isfinite(plx) & (plx > 0) & np.isfinite(snr) & (snr >= snr_min)
    n_drop = int((~keep).sum())
    if n_drop:
        log.warning("local Gaia: dropped %d/%d sources failing the parallax S/N "
                    ">= %.1f cut (non-positive or low-S/N parallax — distance "
                    "unreliable). Kept %d.", n_drop, len(plx), snr_min, int(keep.sum()))

    ra, dec, plx, sigma = ra[keep], dec[keep], plx[keep], sigma[keep]
    r_med = 1000.0 / plx
    # Distance bounds from the parallax +/- sigma interval (pc). A near-zero
    # lower-parallax bound -> very large r_hi -> low confidence, as intended.
    plx_hi = plx + sigma
    plx_lo = np.clip(plx - sigma, 1e-6, None)
    r_lo = 1000.0 / plx_hi
    r_hi = 1000.0 / plx_lo

    t = Table()
    t["ra"] = ra
    t["dec"] = dec
    t["r_med_geo"] = r_med
    t["r_lo_geo"] = r_lo
    t["r_hi_geo"] = r_hi
    # No source_id in the local record; synthesize a stable index so the table
    # satisfies the same schema as the online source (used only for dedup/joins).
    t["source_id"] = np.arange(len(ra), dtype=np.int64)
    return t


class LocalGaiaSource(DistanceSource):
    """DistanceSource backed by a PI-Gaia-process tab-separated export."""

    def __init__(self, tsv_path: str, mag_limit: float | None = None, snr_min: float = 5.0):
        self.tsv_path = tsv_path
        self.mag_limit = mag_limit
        self.snr_min = snr_min

    def distances_for(self, footprint: FieldFootprint) -> Table:
        ra, dec, plx, g = _read_pi_gaia_tsv(self.tsv_path)
        if self.mag_limit is not None:
            keep = g < self.mag_limit
            n = int((~keep).sum())
            if n:
                log.warning("local Gaia: magnitude cut G<%.1f excludes %d source(s).",
                            self.mag_limit, n)
            ra, dec, plx, g = ra[keep], dec[keep], plx[keep], g[keep]
        tbl = parallax_to_distance_table(ra, dec, plx, g, self.snr_min)
        if len(tbl) == 0:
            raise RuntimeError("local Gaia produced zero usable sources after the "
                               "parallax/magnitude cuts for this field")
        log.info("local Gaia: %d usable sources (parallax distances)", len(tbl))
        return tbl[list(_REQUIRED)]


def _read_pi_gaia_tsv(path: str):
    """Parse the tab-separated `ra dec parallax G` export into arrays.

    The PJSR dialog writes this clean format directly from the Gaia process's
    in-memory `sources` array (fields ra=0, dec=1, parallax=2, G=5), so the
    column layout is ours to define — not PI's tabular text format. A header
    line and any comment/blank lines are skipped.
    """
    with open(path) as fh:
        return _parse_lines(fh.read().splitlines())


def _parse_lines(lines):
    ra, dec, plx, g = [], [], [], []
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        parts = ln.replace(",", "\t").split("\t")
        if len(parts) < 4:
            continue
        try:
            r, d, p, gg = (float(parts[0]), float(parts[1]),
                           float(parts[2]), float(parts[3]))
        except ValueError:
            continue  # header row ("ra\tdec\t...") or stray text
        ra.append(r); dec.append(d); plx.append(p); g.append(gg)
    if not ra:
        raise RuntimeError("no usable rows parsed from the local Gaia export "
                           "(expected tab/comma-separated: ra dec parallax G)")
    return np.array(ra), np.array(dec), np.array(plx), np.array(g)
