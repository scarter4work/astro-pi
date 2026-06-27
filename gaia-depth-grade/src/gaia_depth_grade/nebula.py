"""Nebula (starless-layer) depth grade.

Stars carry Gaia parallax -> real distance; diffuse nebula gas does not. So the
nebula depth here is STRUCTURE-driven (its own form decides where it varies) and
Gaia-CALIBRATED (the matched stars' real distance spread sets how strong the
effect is, so stars and nebula share one physical depth scale).

Polarity: dark dust/globules come forward (near, +), diffuse glow recedes (far, -).
Applied as atmospheric perspective (near brighter/warmer/more saturated) plus a
depth-weighted, noise-cored local-contrast "structural pop".
"""
from __future__ import annotations

import numpy as np
from scipy.ndimage import gaussian_filter


def _luminance(img: np.ndarray) -> np.ndarray:
    return img.mean(axis=2) if img.ndim == 3 else img


def gaia_depth_budget(r_med: np.ndarray, ref_dex: float = 0.5) -> float:
    """A 0..~1.5 scale reflecting how much real depth spread the matched stars show.

    Gaia sets the SCALE of the nebula effect (structure drives where it varies):
    a field with a wide foreground/background distance range grades harder; a flat
    field (or too few matches) grades gently or not at all. `ref_dex` is the
    log10-distance spread treated as a "normal" full-strength field.
    """
    r = np.asarray(r_med, dtype=float)
    r = r[np.isfinite(r) & (r > 0)]
    if r.size < 20:
        return 0.0
    lo, hi = np.percentile(np.log10(r), [5.0, 95.0])
    return float(np.clip((hi - lo) / ref_dex, 0.0, 1.0))


def structural_depth(lum: np.ndarray, frac: float = 0.03, sky_pct: float = 20.0) -> np.ndarray:
    """Per-pixel depth field s in [-1, 1] from the nebula's own structure.

    Dark dust/globules (local negative residual vs the large-scale glow) -> +near;
    smooth diffuse glow -> -far. Empty sky below a soft luminance floor -> 0 so the
    background isn't graded. Returns a smoothed field (a depth map, not noise).
    """
    h, w = lum.shape
    sigma = max(4.0, frac * min(h, w))
    bg = gaussian_filter(lum, sigma)
    resid = lum - bg
    scale = 1.4826 * np.median(np.abs(resid - np.median(resid))) + 1e-6
    s = np.clip(-resid / (3.0 * scale), -1.0, 1.0)          # dark dust -> +near

    floor = np.percentile(lum, sky_pct)
    span = np.percentile(lum, 60.0) - floor + 1e-6
    signal = np.clip((lum - floor) / span, 0.0, 1.0)        # 0 over sky, 1 over nebula
    s = s * signal
    return gaussian_filter(s, sigma * 0.5)


def render_nebula(starless, table, strength=1.0, atmos=1.0, structure=1.0, budget=None):
    """Grade the starless (nebula) layer. `strength` is the master amount; `atmos`
    and `structure` weight the two effects. The effective amount is also multiplied
    by the Gaia depth budget so it stays tied to the field's real depth spread.
    Returns a graded copy; a zero/near-zero amount returns the input unchanged.
    """
    out = np.array(starless, dtype=float, copy=True)
    is_color = out.ndim == 3

    if budget is None:
        budget = gaia_depth_budget(np.asarray(table["r_med_geo"]))
    amt = float(strength) * float(budget)
    if amt <= 1e-6:
        return out

    s = structural_depth(_luminance(out))
    s3 = s[..., None] if is_color else s

    # 1) atmospheric brightness: near forward (brighter), far recedes (dimmer)
    out *= 1.0 + (0.18 * atmos * amt) * s3

    # 2) atmospheric colour: near more saturated + slightly warm, far desaturated +
    # cool. Kept GENTLE — on narrowband (HaOO) a heavy warm/cool split reads as a
    # garish teal/pink cast, so the colour tilt is a small fraction of the effect.
    if is_color:
        lum = out.mean(axis=2, keepdims=True)
        out = lum + (1.0 + (0.15 * atmos * amt) * s3) * (out - lum)
        tilt = (0.025 * atmos * amt) * s
        out[..., 0] *= 1.0 + tilt           # red/Ha lifts toward the viewer
        out[..., -1] *= 1.0 - 0.5 * tilt    # blue/OIII cools into the distance

    # 3) structural pop: depth-weighted local contrast, noise-cored so it adds
    # relief (dust crisper, glow softer) without amplifying grain.
    if structure > 1e-6:
        blur = gaussian_filter(out, sigma=2.0, axes=(0, 1) if is_color else None)
        hp = out - blur
        nz = 1.4826 * np.median(np.abs(hp - np.median(hp)))
        hp = np.sign(hp) * np.maximum(np.abs(hp) - 0.75 * nz, 0.0)
        out += (0.45 * structure * amt) * s3 * hp

    np.clip(out, 0.0, 1.0, out=out)
    return out
