"""The angular energy spectrum — the fingerprint's "depth signature" (§4.2).

Decomposes a luminance image with the starlet transform, measures L2 energy per
wavelet plane, and labels each plane with its characteristic angular scale in
arcseconds (§2.2). The residual (gradient/DC) plane is excluded — it is not
structure.

Scale convention (must be identical for reference and target so their spectra
are comparable): wavelet plane ``i`` (0-indexed) isolates structure around
``2**i`` pixels, since the à trous kernel at step ``i`` is dilated by ``2**i``.
The angular center is therefore ``2**i * pixel_scale_arcsec``. The absolute
constant is unimportant; what matters is that both images use the same rule and
each uses its *own* pixel scale.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .starlet import starlet_transform


@dataclass
class EnergySpectrum:
    """Per-scale structural energy, labeled in angular units.

    ``energy`` is raw L2 energy per plane. It is intentionally NOT normalized
    here: band-limiting (§4.4) selects the comparable bins and re-normalizes over
    the survivors at comparison time, which is the only place normalization is
    physically meaningful.
    """

    centers_arcsec: np.ndarray
    energy: np.ndarray


def energy_spectrum(
    image: np.ndarray, pixel_scale_arcsec: float, n_scales: int
) -> EnergySpectrum:
    """Compute the angular energy spectrum of a 2D luminance image."""
    if image.ndim != 2:
        raise ValueError(f"energy_spectrum expects a 2D luminance image, got {image.shape}")

    planes, _residual = starlet_transform(image, n_scales=n_scales)
    energy = np.sum(planes**2, axis=(1, 2))

    scale_px = 2.0 ** np.arange(n_scales)
    centers_arcsec = scale_px * pixel_scale_arcsec

    return EnergySpectrum(centers_arcsec=centers_arcsec, energy=energy)
