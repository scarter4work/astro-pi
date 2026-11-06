"""Cavity depth transfer functions for the blister/champagne-flow HII region.

Physical picture: a hot star carves a CONCAVE cavity into the near face of a
molecular cloud, and we look into the open mouth of the bowl. Three independent
depth terms, each a pure function of one input, are blended by
``reconstruct_cavity`` (Task 8). Keep every function here pure and standalone.

Depth convention (must match ``CavityModel.z`` in model.py):
    z is pixel-relative depth with the observer at -inf; **0 = nearest the
    observer, 1 = farthest** (larger = farther).

Terms:
- ``geometry_prior``: a concave bowl. The apex/center sightline looks all the
  way to the bottom of the bowl, so it is the DEEPEST (largest z); depth falls
  off toward the rim (the near lip).
- ``ionization_depth``: [O III]-rich gas fills the cavity interior near the hot
  star, forward on the wall (SMALL z); H-alpha/[S II]-rich gas hugs the far
  ionization front / outer skin (LARGE z).
- ``emission_measure_relief``: a SIGNED relief centered on 0. Bright filamentary
  edges are tangent sightlines (limb-brightening) that bulge toward the observer
  (nearer, negative); flat sky carries no relief (~0).
"""

from __future__ import annotations

import numpy as np
from scipy.ndimage import gaussian_filter

from .model import IonizationMap

# --- geometry_prior ---------------------------------------------------------
_GEOMETRY_POWER = 2.0  # paraboloid bowl; >1 steepens the well toward the apex

# --- ionization_depth -------------------------------------------------------
_IONIZATION_GAMMA = 1.0  # shaping of the oiii_fraction -> depth mapping

# --- emission_measure_relief ------------------------------------------------
_EM_SIGMA_SMALL_PX = 1.0   # filament-scale smoothing
_EM_SIGMA_LARGE_PX = 4.0   # local-background scale; band-pass = small - large
_EM_GAIN = 0.5             # peak |relief| at the normalization percentile
_EM_NORM_PCT = 99.5        # robust scale of |band-pass| over the frame
_EM_EPS = 1e-12


def geometry_prior(
    shape: tuple[int, int],
    apex_xy: tuple[float, float],
    footprint_mask: np.ndarray | None = None,
    power: float = _GEOMETRY_POWER,
) -> np.ndarray:
    """Concave-bowl depth prior in [0, 1]; apex sightline deepest (largest z).

    ``apex_xy`` is ``(x=col, y=row)`` of the projected cavity axis. Depth is a
    paraboloid well, ``z = 1 - (r / R)**power``, where ``r`` is the projected
    distance from the apex and ``R`` is the rim radius (the farthest footprint
    pixel from the apex). The apex (r=0) is the deepest point (z=1) and depth
    decreases smoothly to the near rim (z->0). Pixels outside the footprint are
    sky and return 0.
    """
    ny, nx = int(shape[0]), int(shape[1])
    ax, ay = float(apex_xy[0]), float(apex_xy[1])

    yy, xx = np.mgrid[0:ny, 0:nx]
    r = np.hypot(xx.astype(np.float64) - ax, yy.astype(np.float64) - ay)

    if footprint_mask is not None:
        mask = np.asarray(footprint_mask, dtype=bool)
        if mask.shape != (ny, nx):
            raise ValueError(f"footprint_mask shape {mask.shape} != {(ny, nx)}")
    else:
        mask = np.ones((ny, nx), dtype=bool)

    if mask.any():
        R = float(r[mask].max())
    else:
        R = float(r.max())
    if R <= 0.0:
        R = 1.0

    rn = np.clip(r / R, 0.0, 1.0)
    z = 1.0 - rn ** float(power)
    z = np.clip(z, 0.0, 1.0)
    z[~mask] = 0.0
    return z.astype(np.float32)


def ionization_depth(
    ion_map: IonizationMap,
    gamma: float = _IONIZATION_GAMMA,
) -> np.ndarray:
    """Ionization depth term in [0, 1] from the OIII fraction.

    High ``oiii_fraction`` (cavity interior, near the hot star) maps to SMALL z
    (forward on the wall); low ``oiii_fraction`` (outer skin / ionization front)
    maps to LARGE z (farther). The transfer is ``z = 1 - f**gamma`` with ``f``
    the clipped OIII fraction.
    """
    f = np.clip(np.nan_to_num(ion_map.oiii_fraction.astype(np.float64)), 0.0, 1.0)
    if gamma != 1.0:
        f = f ** float(gamma)
    z = np.clip(1.0 - f, 0.0, 1.0)
    return z.astype(np.float32)


def emission_measure_relief(
    lum: np.ndarray,
    sigma_small: float = _EM_SIGMA_SMALL_PX,
    sigma_large: float = _EM_SIGMA_LARGE_PX,
    gain: float = _EM_GAIN,
) -> np.ndarray:
    """Signed limb-brightening relief centered on 0.

    Emission measure (~integral of n_e^2 along the LOS) spikes where the sightline
    runs tangent to an emitting surface, so bright filamentary edges bulge toward
    the observer. A band-pass (difference of Gaussians) isolates filament-scale
    ridges; its DC component is removed, so a flat sky of any brightness returns
    ~0. The result is signed: bright ridges are negative (nearer the observer),
    their flanks positive (farther). Normalized by a robust percentile of the
    band-pass magnitude so the output is scale-free, then clipped to [-gain, gain].
    """
    L = np.nan_to_num(np.asarray(lum, dtype=np.float64))
    L = np.clip(L, 0.0, None)

    band = gaussian_filter(L, sigma_small) - gaussian_filter(L, sigma_large)

    scale = float(np.percentile(np.abs(band), _EM_NORM_PCT))
    if scale <= _EM_EPS:
        return np.zeros(L.shape, dtype=np.float32)

    # Negative sign: bright ridges bulge toward the observer -> smaller z.
    z = -gain * (band / scale)
    z = np.clip(z, -gain, gain)
    return z.astype(np.float32)
