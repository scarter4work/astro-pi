"""sRGB -> CIELAB conversion (D65).

The fingerprint reasons about tonal placement in L* and color in the a*/b* plane
(§4.2). Professional renders are delivered as display-referred sRGB, so we assume
sRGB primaries and gamma. Conversion is the standard sRGB -> linear -> XYZ -> Lab
chain; implemented directly to avoid a heavy image-processing dependency.
"""

from __future__ import annotations

import numpy as np

# sRGB (linear) -> XYZ, D65 (IEC 61966-2-1).
_RGB_TO_XYZ = np.array(
    [
        [0.4124564, 0.3575761, 0.1804375],
        [0.2126729, 0.7151522, 0.0721750],
        [0.0193339, 0.1191920, 0.9503041],
    ]
)
# D65 reference white.
_WHITE = np.array([0.95047, 1.0, 1.08883])

_DELTA = 6.0 / 29.0


def _srgb_to_linear(c: np.ndarray) -> np.ndarray:
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _lab_f(t: np.ndarray) -> np.ndarray:
    return np.where(t > _DELTA**3, np.cbrt(t), t / (3 * _DELTA**2) + 4.0 / 29.0)


def rgb_to_lab(rgb: np.ndarray) -> np.ndarray:
    """Convert an ``(H, W, 3)`` sRGB image in [0, 1] to CIELAB.

    Returns an ``(H, W, 3)`` array of ``(L*, a*, b*)``.
    """
    if rgb.ndim != 3 or rgb.shape[-1] != 3:
        raise ValueError(f"rgb_to_lab expects an (H, W, 3) sRGB image, got {rgb.shape}")

    linear = _srgb_to_linear(np.asarray(rgb, dtype=np.float64))
    xyz = linear @ _RGB_TO_XYZ.T
    f = _lab_f(xyz / _WHITE)

    L = 116.0 * f[..., 1] - 16.0
    a = 500.0 * (f[..., 0] - f[..., 1])
    b = 200.0 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], axis=-1)
