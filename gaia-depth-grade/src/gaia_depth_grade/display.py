"""Display helpers for the interactive preview: screen-blend the graded stars
over the starless layer (same formula as the harness PixelMath), apply an
STF-style autostretch for screen, and write PNGs (with optional 1:1 region crop
and downscale). These touch pixels only for *display* — the Execute path stays
linear.
"""
from __future__ import annotations

import numpy as np
from PIL import Image


def screen_blend(stars, starless):
    """Screen blend, identical to the harness PixelMath ~((~A)*(~B))."""
    a = np.asarray(stars, dtype=float)
    b = np.asarray(starless, dtype=float)
    return 1.0 - (1.0 - a) * (1.0 - b)


def _mtf(x, m):
    """PixInsight midtones transfer function."""
    den = (2.0 * m - 1.0) * x - m
    out = np.where(den == 0.0, x, ((m - 1.0) * x) / den)
    return out


def autostretch(rgb, target_bg: float = 0.25, shadows_clip: float = -2.8):
    """STF-style autostretch -> uint8. Shadows from median+shadows_clip*MAD,
    midtones solved so the median maps to target_bg."""
    x = np.clip(np.asarray(rgb, dtype=float), 0.0, None)
    med = float(np.median(x))
    mad = float(np.median(np.abs(x - med))) * 1.4826
    shadows = float(np.clip(med + shadows_clip * mad, 0.0, 1.0))
    denom = max(1e-8, 1.0 - shadows)
    r = (med - shadows) / denom
    B = target_bg
    sden = (2.0 * B * r - B - r)
    m = 0.5 if abs(sden) < 1e-12 else (r * (B - 1.0)) / sden
    m = float(np.clip(m, 1e-3, 1.0 - 1e-3))
    y = np.clip((x - shadows) / denom, 0.0, 1.0)
    out = np.clip(_mtf(y, m), 0.0, 1.0)
    return (out * 255.0 + 0.5).astype(np.uint8)


def write_png(arr, path, max_width: int | None = None, region=None):
    """Write `arr` (uint8 or 0..1 float, HxW or HxWx3) to a PNG.

    region=(x, y, w, h) crops first (1:1). max_width downscales (aspect kept).
    """
    a = np.asarray(arr)
    if a.dtype != np.uint8:
        a = (np.clip(a.astype(float), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    if region is not None:
        x, y, w, h = (int(v) for v in region)
        a = a[y:y + h, x:x + w]
    img = Image.fromarray(a)
    if max_width is not None and img.width > max_width:
        scale = max_width / img.width
        img = img.resize((max_width, max(1, int(round(img.height * scale)))))
    img.save(path)
