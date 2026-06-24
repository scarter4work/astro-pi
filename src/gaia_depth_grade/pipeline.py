"""Two-stage depth grade.

`prepare_grade` does the expensive, gain-independent work (detect + Gaia query +
cross-match) and returns the full detected table annotated with Bailer-Jones
distances (NaN for unmatched stars) plus a QA dict. `render_from_prep` does the
cheap, gain-dependent work (strength -> modulation -> render) and can be re-run
for every gain change without touching the network or StarXTerminator.
"""
from __future__ import annotations

import logging

import numpy as np

from .config import GradeConfig
from .detect import detect_stars
from .distances import DistanceSource
from .match import cross_match
from .modulate import compute_modulation
from .render import render_stars
from .transform import effective_strength
from .wcs import field_footprint, load_wcs

log = logging.getLogger(__name__)


def _luminance(image: np.ndarray) -> np.ndarray:
    return image.mean(axis=2) if image.ndim == 3 else image


def prepare_grade(image, header, config: GradeConfig, source: DistanceSource):
    """Gain-independent stage: detect stars, query Gaia, cross-match.

    Returns (table, qa) where `table` is the full detected set with x/y/flux and
    r_med_geo/r_lo_geo/r_hi_geo columns (NaN for unmatched rows), and `qa` is the
    match-quality summary.
    """
    wcs = load_wcs(header)
    lum = _luminance(image)
    detected = detect_stars(lum, config.detect_fwhm, config.detect_threshold_sigma)
    fp = field_footprint(wcs, lum.shape)
    catalog = source.distances_for(fp)
    table, stats = cross_match(detected, catalog, wcs, config.match_tolerance_px)

    low = stats.match_rate < config.min_match_rate
    if low:
        log.warning("LOW MATCH RATE %.2f < %.2f — depth grade is unreliable",
                    stats.match_rate, config.min_match_rate)
    qa = {
        "n_detected": stats.n_detected, "n_matched": stats.n_matched,
        "match_rate": stats.match_rate, "median_offset_px": stats.median_offset_px,
        "low_match_warning": bool(low),
    }
    return table, qa


def render_from_prep(stars_layer, table, config: GradeConfig):
    """Gain-dependent stage: strength -> modulation -> render onto stars_layer."""
    strength = effective_strength(
        np.asarray(table["r_med_geo"]), np.asarray(table["r_lo_geo"]),
        np.asarray(table["r_hi_geo"]), config.p_low, config.p_high,
        config.neutral_strength)
    modulation = compute_modulation(strength, config.gains)
    return render_stars(stars_layer, table, modulation, config.base_sigma_px)
