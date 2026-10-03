"""The à trous (starlet) multiscale transform.

This is the engine behind the fingerprint's energy spectrum (design §4.2) — the
"depth signature" that distinguishes a flat image from one with real dimensional
structure. It is a stationary (undecimated) wavelet transform: every plane keeps
the full image resolution, so wavelet layers map cleanly onto physical angular
scales once we know the pixel scale (§2.2, §4.4).

Property: ``image == planes.sum(axis=0) + residual`` to floating-point precision.
"""

from __future__ import annotations

import numpy as np
from scipy.ndimage import convolve1d

# B3-spline scaling function — the standard starlet kernel.
_B3_KERNEL = np.array([1.0, 4.0, 6.0, 4.0, 1.0]) / 16.0


def _dilate(kernel: np.ndarray, scale: int) -> np.ndarray:
    """Insert ``2**scale - 1`` zeros between taps ("holes"), giving the à trous
    kernel for a given scale without decimating the image."""
    step = 2**scale
    dilated = np.zeros((len(kernel) - 1) * step + 1)
    dilated[::step] = kernel
    return dilated


def _smooth(image: np.ndarray, scale: int) -> np.ndarray:
    """Separable convolution with the dilated B3-spline, mirrored at borders."""
    kernel = _dilate(_B3_KERNEL, scale)
    smoothed = convolve1d(image, kernel, axis=0, mode="mirror")
    smoothed = convolve1d(smoothed, kernel, axis=1, mode="mirror")
    return smoothed


def starlet_transform(image: np.ndarray, n_scales: int) -> tuple[np.ndarray, np.ndarray]:
    """Decompose a 2D image into ``n_scales`` wavelet planes plus a smooth residual.

    Parameters
    ----------
    image : 2D float array
    n_scales : number of wavelet planes to produce

    Returns
    -------
    planes : array of shape ``(n_scales, H, W)`` — detail at each scale, finest first.
    residual : array of shape ``(H, W)`` — the coarse approximation left over.

    Together ``planes.sum(axis=0) + residual`` reconstructs ``image`` exactly.
    """
    if image.ndim != 2:
        raise ValueError(f"starlet_transform expects a 2D image, got shape {image.shape}")
    if n_scales < 1:
        raise ValueError(f"n_scales must be >= 1, got {n_scales}")

    current = image.astype(np.float64, copy=True)
    planes = np.empty((n_scales, *image.shape), dtype=np.float64)
    for scale in range(n_scales):
        smoothed = _smooth(current, scale)
        planes[scale] = current - smoothed
        current = smoothed

    return planes, current
