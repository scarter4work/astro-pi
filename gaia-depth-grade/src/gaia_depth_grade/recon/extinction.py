"""Near-side dust layer: dark lanes that occlude the emission behind them.

A dark lane is dust only when it is dark *relative to a bright neighbourhood*; the plain
sky floor is equally dark but is not dust. Operates on linear data and never stretches it.
"""

from __future__ import annotations

import numpy as np
from scipy.ndimage import grey_closing, gaussian_filter

CASE_B_HA_HB = 2.86
_LANE_MAX_WIDTH_PX = 15      # grey closing fills dark features narrower than this
_SKY_PERCENTILE = 5.0
_BRIGHT_PERCENTILE = 99.0
_MIN_SURROUND_FRAC = 0.05    # surround must exceed sky by this fraction of the bright range
_BALMER_SATURATION = 3.0     # ha/hb excess (over Case B, as a ratio) at which dust -> ~0.63


def dust_from_structure(lum: np.ndarray, lane_px: int = _LANE_MAX_WIDTH_PX) -> np.ndarray:
    """Opacity in [0, 1] from the deficit of ``lum`` below its local bright surround.

    The surround is a grey closing (fills dark features up to ``lane_px`` wide with the
    level of the bright pixels around them, leaves sky regions and field edges alone).
    Opacity = (surround - lum) / (surround - sky), gated to zero where the surround is
    not meaningfully above the sky floor.
    """
    a = np.clip(np.nan_to_num(np.asarray(lum, dtype=np.float64)), 0.0, None)
    sky = np.percentile(a, _SKY_PERCENTILE)
    bright = np.percentile(a, _BRIGHT_PERCENTILE)
    rng = max(bright - sky, 1e-12)
    smooth = gaussian_filter(a, 1.0)
    surround = grey_closing(smooth, size=(lane_px, lane_px))
    height = surround - sky
    opacity = (surround - smooth) / np.maximum(height, 1e-12)
    gate = np.clip((height - _MIN_SURROUND_FRAC * rng) / (_MIN_SURROUND_FRAC * rng), 0.0, 1.0)
    return np.clip(opacity * gate, 0.0, 1.0).astype(np.float32)


def dust_from_balmer(ha: np.ndarray, hb: np.ndarray, eps: float = 1e-6) -> np.ndarray:
    """Opacity in [0, 1] from the Balmer decrement; ha/hb at or below Case B (2.86) -> 0."""
    ha = np.clip(np.nan_to_num(np.asarray(ha, dtype=np.float64)), 0.0, None)
    hb = np.clip(np.nan_to_num(np.asarray(hb, dtype=np.float64)), 0.0, None)
    excess = np.maximum(ha / (hb + eps) / CASE_B_HA_HB - 1.0, 0.0)
    return (1.0 - np.exp(-excess / _BALMER_SATURATION)).astype(np.float32)
