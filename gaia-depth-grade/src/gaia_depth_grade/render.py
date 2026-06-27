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


def _radial_taper(h, w, cx, cy, rad):
    # Raised-cosine (Hann) feather: 1.0 at the star centre, smoothly falling to
    # EXACTLY 0.0 at r >= rad. Every per-star modulation is multiplied by this so
    # it blends seamlessly into the background. Without it, a flat factor applied
    # over the square window (win *= b, contrast, saturation) leaves a hard step
    # at the box boundary — which reads as a quilt of little squares around bright
    # or clustered stars (and a flat-topped square core on a saturated star).
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    return 0.5 * (1.0 + np.cos(np.pi * np.clip(r / rad, 0.0, 1.0)))


def render_stars(stars_layer, detected, modulation, base_sigma_px):
    out = np.array(stars_layer, dtype=float, copy=True)
    is_color = out.ndim == 3

    for i in range(len(detected)):
        x, y = float(detected["x"][i]), float(detected["y"][i])
        flux = float(detected["flux"][i])
        b = float(modulation.brightness[i])
        zsize = float(modulation.size[i])
        camount = float(modulation.contrast[i])
        sat = float(modulation.saturation[i])

        # Window scales with the size gain so a widened glow still tapers to ~0
        # inside it; the cosine feather then guarantees a seamless edge for all
        # modulations regardless of the star's actual size.
        rad = int(math.ceil(4 * base_sigma_px * max(1.0, zsize)))
        ys, xs = _window_slice(x, y, rad, out.shape)
        win = out[ys, xs]
        if win.size == 0:
            continue
        h, w = win.shape[0], win.shape[1]
        cx, cy = x - xs.start, y - ys.start
        taper = _radial_taper(h, w, cx, cy, rad)
        wtaper = taper[..., None] if is_color else taper

        # brightness: feathered multiply (factor -> 1.0 at the window edge)
        if abs(b - 1.0) > 1e-9:
            win *= 1.0 + (b - 1.0) * wtaper

        # size/glow: add a peak-normalized Gaussian halo (peak amplitude
        # (zsize-1)*flux, width scaling with zsize), feathered to 0 at the edge.
        # Peak-normalizing (not area-normalizing) is what makes a size increase
        # visibly widen the star's footprint rather than just spreading flux.
        #
        # ONE-SIDED on purpose: only enlarge (zsize > 1). For a FAR star zsize < 1,
        # so (zsize-1)*flux is negative and this would SUBTRACT a flux-scaled
        # Gaussian — over-subtracting past the star's own light, clipping to black,
        # and leaving a dark ring/donut (worst on the bright halos around big stars).
        # Far stars are made to recede by the brightness dimming (b < 1) above, never
        # by digging negative flux here.
        if zsize > 1.0 + 1e-9:
            stamp = _gaussian_stamp(h, w, cx, cy, base_sigma_px * zsize)
            peak = stamp.max()
            if peak > 0:
                stamp = stamp / peak
            stamp = stamp * taper
            extra = (zsize - 1.0) * flux
            win += extra * (stamp[..., None] if is_color else stamp)

        # local contrast (unsharp), feathered, with NOISE CORING. Plain unsharp
        # amplifies every high frequency including single-pixel sensor noise, which
        # coarsens the grain ("pixelation" on close zoom). Soft-threshold the
        # high-pass by a robust local noise estimate (MAD) so sub-noise wiggles are
        # held back while real, many-sigma star detail passes through unharmed.
        if abs(camount) > 1e-9:
            blur = gaussian_filter(win, sigma=base_sigma_px, axes=(0, 1) if is_color else None)
            hp = win - blur
            nz = 1.4826 * np.median(np.abs(hp - np.median(hp)))   # ~sigma of the noise
            hp = np.sign(hp) * np.maximum(np.abs(hp) - 0.75 * nz, 0.0)
            win += camount * wtaper * hp

        # saturation (color only), feathered
        if is_color and abs(sat - 1.0) > 1e-9:
            lum = win.mean(axis=2, keepdims=True)
            win[:] = lum + (1.0 + (sat - 1.0) * wtaper) * (win - lum)

        out[ys, xs] = win

    np.clip(out, 0.0, 1.0, out=out)
    return out
