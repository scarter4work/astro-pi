"""An approximate, PI-free Executor for offline tests and CI.

IMPORTANT: these approximations do NOT match PixInsight's processes and are not
meant to. They exist so beam pruning, guardrails, and convergence can be tested
without PixInsight. A green offline suite proves the LOOP is correct; it proves
nothing about whether the output is beautiful. Only the live tests can say that
(spec SS7.4).
"""

from __future__ import annotations

import math

import numpy as np
from scipy.ndimage import gaussian_filter

from autocontrast.fingerprint.starlet import starlet_transform

from ..actions import Action


def _sigma_for_scale(scale_arcsec: float, pixel_scale_arcsec: float) -> float:
    return max(scale_arcsec / max(pixel_scale_arcsec, 1e-9) / 2.355, 0.5)


def _max_supportable_layer(height: int, width: int) -> int:
    """The highest starlet layer worth asking for from an image this size.

    The a trous kernel at layer L is ``4*2**L + 1`` px wide (see starlet.py's
    ``_dilate``); scipy's ``convolve1d`` is a direct, non-FFT convolution, so its
    cost is linear in kernel width -- exponential in L. Past ``L ~ log2(min(H,W))``
    the kernel already dwarfs the image, ``planes[L]`` decays to float noise (its
    "detail" is indistinguishable from the mirror-padding boundary effect), and the
    cost keeps doubling for no signal in return. Bounding L here turns a silent
    exponential hang into an immediate, loud error.
    """
    return int(math.floor(math.log2(min(height, width))))


class NumpyExecutor:
    """Approximate each SS6.2 action with a cheap numpy analogue."""

    def apply(
        self, rgb: np.ndarray, action: Action, *, pixel_scale_arcsec: float
    ) -> np.ndarray:
        out = np.array(rgb, dtype=np.float64, copy=True)
        s = action.strength

        if action.kind == "local_contrast":
            layer = int(action.params.get("layer", 3))
            max_layer = _max_supportable_layer(out.shape[0], out.shape[1])
            if layer > max_layer:
                raise ValueError(
                    f"local_contrast layer {layer} exceeds what a "
                    f"{out.shape[0]}x{out.shape[1]} image supports (max layer "
                    f"{max_layer}); rejecting rather than hanging on an a trous "
                    f"kernel far larger than the image"
                )
            for c in range(out.shape[-1]):
                planes, residual = starlet_transform(out[..., c], n_scales=layer + 1)
                planes[layer] *= 1.0 + s
                out[..., c] = planes.sum(axis=0) + residual

        elif action.kind == "local_equalize":
            sigma = _sigma_for_scale(action.scale_arcsec or 8.0, pixel_scale_arcsec)
            for c in range(out.shape[-1]):
                local_mean = gaussian_filter(out[..., c], sigma=sigma)
                out[..., c] = out[..., c] + s * (out[..., c] - local_mean)

        elif action.kind == "core_hdr":
            # Compress the bright end, which is what HDRMT does to cores.
            out = np.log1p(out * (1.0 + 8.0 * s)) / np.log1p(1.0 + 8.0 * s)

        elif action.kind == "tonal_reshape":
            # Monotone S-curve about the midpoint (SS6.2: monotone-constrained).
            out = np.clip(0.5 + (out - 0.5) * (1.0 + s), 0.0, 1.0)

        elif action.kind == "black_point":
            # Clip-limited: never move the black point past the 1st percentile.
            floor = float(np.percentile(out, 1.0)) * s
            out = np.clip((out - floor) / max(1.0 - floor, 1e-9), 0.0, 1.0)

        elif action.kind == "chroma":
            gray = out.mean(axis=-1, keepdims=True)
            out = gray + (out - gray) * (1.0 + s)

        elif action.kind == "background_neutralize":
            medians = np.array([np.median(out[..., c]) for c in range(out.shape[-1])])
            out = out - medians + medians.mean()

        elif action.kind == "star_split":
            # A mode change, not a pixel change; the loop handles layer routing.
            pass

        else:
            raise ValueError(f"unknown action kind {action.kind!r}")

        return np.clip(out, 0.0, 1.0)
