from __future__ import annotations

import numpy as np


def depth_strength(r_med, p_low=5.0, p_high=95.0, neutral=0.0) -> np.ndarray:
    r = np.asarray(r_med, dtype=float)
    out = np.full(r.shape, float(neutral), dtype=float)
    finite = np.isfinite(r) & (r > 0)
    if not np.any(finite):
        return out
    logr = np.log10(r[finite])
    lo = np.percentile(logr, p_low)
    hi = np.percentile(logr, p_high)
    if hi <= lo:
        out[finite] = 1.0
        return out
    norm = (np.clip(logr, lo, hi) - lo) / (hi - lo)  # 0=near..1=far
    out[finite] = 1.0 - 2.0 * norm                    # +1 near .. -1 far
    return out


def confidence(r_med, r_lo, r_hi) -> np.ndarray:
    rm = np.asarray(r_med, dtype=float)
    rl = np.asarray(r_lo, dtype=float)
    rh = np.asarray(r_hi, dtype=float)
    with np.errstate(invalid="ignore", divide="ignore"):
        frac = (rh - rl) / (2.0 * rm)
    c = 1.0 - np.clip(frac, 0.0, 1.0)
    c[~np.isfinite(c)] = 0.0
    return c


def effective_strength(r_med, r_lo, r_hi, p_low=5.0, p_high=95.0, neutral=0.0) -> np.ndarray:
    s = depth_strength(r_med, p_low, p_high, neutral)
    c = confidence(r_med, r_lo, r_hi)
    rm = np.asarray(r_med, dtype=float)
    e = s * c
    nanmask = ~np.isfinite(rm)
    e[nanmask] = neutral
    return e
