"""Ionization ratio map: the OIII fraction as a depth coordinate.

High-ionization gas (OIII-dominant) sits close to the ionizing source; low-ionization
gas (Ha-dominant) sits at the cavity edge. The map is computed on linear data and
never stretches it.
"""

from __future__ import annotations

import numpy as np
from scipy.ndimage import gaussian_filter

from .model import ChannelSet, IonizationMap

_SMOOTH_SIGMA_PX = 2.0


def ionization_ratio(ch: ChannelSet, eps: float = 1e-6) -> IonizationMap:
    """OIII / (OIII + Ha + eps), flux-weighted smoothed, clipped to [0, 1].

    Numerator and denominator are smoothed separately so bright pixels dominate the
    local ratio and noisy faint pixels cannot swing it. ``ch.sii`` is optional and
    currently does not alter the map; ``sii=None`` is fully supported.
    """
    ha = np.clip(np.nan_to_num(ch.ha.astype(np.float64)), 0.0, None)
    oiii = np.clip(np.nan_to_num(ch.oiii.astype(np.float64)), 0.0, None)
    num = gaussian_filter(oiii, _SMOOTH_SIGMA_PX)
    den = gaussian_filter(oiii + ha, _SMOOTH_SIGMA_PX)
    frac = np.clip(num / (den + eps), 0.0, 1.0).astype(np.float32)
    return IonizationMap(oiii_fraction=frac, confidence=ch.ratio_confidence)
