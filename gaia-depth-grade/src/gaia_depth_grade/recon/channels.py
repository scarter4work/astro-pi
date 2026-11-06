"""Linear channel ingest for SHO / duoband / OSC data.

All outputs are float32 (ny, nx); nothing here stretches or renormalises data.
"""

from __future__ import annotations

import numpy as np

from .model import ChannelSet, validate_shapes

CONF_SHO = 1.0
CONF_DUOBAND = 0.6
CONF_OSC = 0.25

# Normalised-median ceiling for "linear". A linear starless emission master is
# sky-floored and right-skewed: median sits within a few % of the black point
# relative to the bright tail. Stretching lifts midtones so the median moves to
# a large fraction of the range (an autostretch targets ~0.25). 0.10 separates linear (~0.003)
# from the asinh(1000x)-stretched fixture (~0.27) with wide margin on both sides.
LINEAR_MEDIAN_FRAC_MAX = 0.10


def _f32(a) -> np.ndarray:
    return np.asarray(a, dtype=np.float32)


def from_mono_sho(ha, oiii, sii) -> ChannelSet:
    ha, oiii, sii = _f32(ha), _f32(oiii), _f32(sii)
    validate_shapes(ha, oiii, sii)
    return ChannelSet(ha, oiii, sii, CONF_SHO, True)


def from_duoband(d1_hao3, d2_s2o3) -> ChannelSet:
    """Unmix duoband masters. Valid where at most one of Ha / S II is nonzero."""
    d1, d2 = _f32(d1_hao3), _f32(d2_s2o3)
    validate_shapes(d1, d2)
    oiii = np.minimum(d1, d2)
    ha = np.maximum(d1 - oiii, 0).astype(np.float32)
    sii = np.maximum(d2 - oiii, 0).astype(np.float32)
    return ChannelSet(ha, oiii.astype(np.float32), sii, CONF_DUOBAND, True)


def from_osc_rgb(rgb) -> ChannelSet:
    rgb = _f32(rgb)
    if rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError(f"expected (ny, nx, 3) RGB, got {rgb.shape}")
    ha = rgb[..., 0].copy()
    oiii = (0.5 * (rgb[..., 1] + rgb[..., 2])).astype(np.float32)
    return ChannelSet(ha, oiii, None, CONF_OSC, True)


def looks_linear(arr) -> bool:
    """False for an obviously stretched array (median far above the black point)."""
    a = np.asarray(arr, dtype=np.float64)
    lo, hi = np.percentile(a, [0.1, 99.9])
    if hi - lo <= 0:
        return True  # flat: nothing indicates a stretch
    frac = (np.median(a) - lo) / (hi - lo)
    return bool(frac < LINEAR_MEDIAN_FRAC_MAX)


_KEYS = {"sho": ("ha", "oiii", "sii"), "duoband": ("d1", "d2"), "osc": ("rgb",)}


def extract_channels(inputs: dict, mode: str) -> ChannelSet:
    if mode not in _KEYS:
        raise ValueError(f"unknown mode {mode!r}; expected one of {sorted(_KEYS)}")
    missing = [k for k in _KEYS[mode] if k not in inputs]
    if missing:
        raise ValueError(f"mode {mode!r} missing inputs: {missing}")
    arrays = [np.asarray(inputs[k]) for k in _KEYS[mode]]
    validate_shapes(*arrays)  # hard error on misregistration
    if mode == "sho":
        cs = from_mono_sho(*arrays)
    elif mode == "duoband":
        cs = from_duoband(*arrays)
    else:
        cs = from_osc_rgb(arrays[0])

    probes = arrays if mode != "osc" else [arrays[0][..., i] for i in range(3)]
    if all(looks_linear(p) for p in probes):
        return cs
    return ChannelSet(cs.ha, cs.oiii, cs.sii, 0.0, False)
