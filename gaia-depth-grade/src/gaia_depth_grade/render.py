from __future__ import annotations

import math

import numpy as np
from scipy.ndimage import gaussian_filter


def _window_slice(x, y, rad, shape):
    ny, nx = shape[0], shape[1]
    x0, x1 = max(0, int(x) - rad), min(nx, int(x) + rad + 1)
    y0, y1 = max(0, int(y) - rad), min(ny, int(y) + rad + 1)
    return slice(y0, y1), slice(x0, x1)


def _gaussian_stamp(h, w, cx, cy, sigma):
    yy, xx = np.mgrid[0:h, 0:w]
    return np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * sigma**2))


def render_stars(stars_layer, detected, modulation, base_sigma_px):
    out = np.array(stars_layer, dtype=float, copy=True)
    is_color = out.ndim == 3
    rad = int(math.ceil(4 * base_sigma_px))

    for i in range(len(detected)):
        x, y = float(detected["x"][i]), float(detected["y"][i])
        flux = float(detected["flux"][i])
        b = float(modulation.brightness[i])
        zsize = float(modulation.size[i])
        camount = float(modulation.contrast[i])
        sat = float(modulation.saturation[i])

        ys, xs = _window_slice(x, y, rad, out.shape)
        win = out[ys, xs]
        if win.size == 0:
            continue

        # brightness
        win *= b

        # size/glow: add a peak-normalized Gaussian halo whose peak amplitude
        # is (zsize-1)*flux and whose width scales with zsize. Peak-normalizing
        # (not area-normalizing) is what makes a size increase visibly widen the
        # star's footprint rather than just adding a negligible flux spread.
        if abs(zsize - 1.0) > 1e-9:
            h, w = win.shape[0], win.shape[1]
            cx, cy = x - xs.start, y - ys.start
            stamp = _gaussian_stamp(h, w, cx, cy, base_sigma_px * zsize)
            peak = stamp.max()
            if peak > 0:
                stamp = stamp / peak
            extra = (zsize - 1.0) * flux
            if is_color:
                for c in range(win.shape[2]):
                    win[..., c] += extra * stamp
            else:
                win += extra * stamp

        # local contrast (unsharp)
        if abs(camount) > 1e-9:
            blur = gaussian_filter(win, sigma=base_sigma_px, axes=(0, 1) if is_color else None)
            win += camount * (win - blur)

        # saturation (color only)
        if is_color and abs(sat - 1.0) > 1e-9:
            lum = win.mean(axis=2, keepdims=True)
            win[:] = lum + sat * (win - lum)

        out[ys, xs] = win

    np.clip(out, 0.0, 1.0, out=out)
    return out
