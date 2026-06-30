from __future__ import annotations

import math

import numpy as np
from scipy.ndimage import gaussian_filter

from .psf import moffat_profile


def _core_color(win, cx, cy, base_sigma_px):
    """Mean per-channel colour over the star's core, normalized so the brightest
    channel is 1.0. Multiplying the (luminance-scaled) halo by this tints it to the
    star's own hue — a blue star gets a blue glow, a red star a red one."""
    h, w = win.shape[0], win.shape[1]
    yy, xx = np.mgrid[0:h, 0:w]
    core = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) <= max(base_sigma_px, 1.0)
    if not core.any():
        return np.ones(win.shape[2])
    rgb = win[core].mean(axis=0)
    m = rgb.max()
    return rgb / m if m > 0 else np.ones_like(rgb)


def _window_slice(x, y, rad, shape):
    ny, nx = shape[0], shape[1]
    x0, x1 = max(0, int(x) - rad), min(nx, int(x) + rad + 1)
    y0, y1 = max(0, int(y) - rad), min(ny, int(y) + rad + 1)
    return slice(y0, y1), slice(x0, x1)


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


def render_stars(stars_layer, detected, modulation, base_sigma_px,
                 halo_profile=None, halo_gain=1.0):
    out = np.array(stars_layer, dtype=float, copy=True)
    is_color = out.ndim == 3

    # The enlarge branch adds a halo whose radial *shape* comes from this profile.
    # In production it is the image-derived PSF (built in render_from_prep); when a
    # caller supplies none (direct/test use) we fall back to a natural Moffat.
    if halo_profile is None:
        halo_profile = moffat_profile(base_sigma_px, beta=2.5, radius_px=8.0 * base_sigma_px)

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

        # size/glow: add a star-shine halo whose radial SHAPE is `halo_profile`
        # (the image's own bright-star PSF, wings and all — not a fast-collapsing
        # Gaussian), stretched by zsize so the footprint genuinely widens, feathered
        # to 0 at the window edge, and tinted to the star's own colour so it blends
        # in instead of reading as a pasted-on grey blob. Amplitude is anchored to
        # the star's ACTUAL peak pixel (not DAO flux) so the glow sits correctly on
        # the real core, and scaled by the user's halo_gain.
        #
        # ONE-SIDED on purpose: only enlarge (zsize > 1). A far star (zsize < 1)
        # would give negative amplitude here, over-subtracting past the star's own
        # light into a dark ring/donut. Far stars recede via the brightness dimming
        # (b < 1) above, never by digging negative flux here.
        if zsize > 1.0 + 1e-9:
            r = np.sqrt((np.arange(w)[None, :] - cx) ** 2
                        + (np.arange(h)[:, None] - cy) ** 2)
            shape = halo_profile.sample(r / zsize) * taper   # stretch + feather
            core = np.sqrt((np.arange(w)[None, :] - cx) ** 2
                           + (np.arange(h)[:, None] - cy) ** 2) <= max(base_sigma_px, 1.0)
            lum = win.mean(axis=2) if is_color else win
            peak_core = float(lum[core].max()) if core.any() else float(lum.max())
            extra = halo_gain * (zsize - 1.0) * peak_core
            if is_color:
                color = _core_color(win, cx, cy, base_sigma_px)
                win += extra * shape[..., None] * color
            else:
                win += extra * shape

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
