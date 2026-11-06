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

from .channels import CONF_DUOBAND, CONF_OSC, CONF_SHO
from .model import (
    CavityModel,
    ChannelSet,
    Ionizer,
    IonizationMap,
    ReconConfig,
    validate_shapes,
)

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


# --- reconstruct_cavity (Task 8) --------------------------------------------
_FOOTPRINT_SKY_PCT = 10.0    # robust sky floor percentile of lum
_FOOTPRINT_PEAK_PCT = 99.5   # robust bright percentile of lum
_EMISSIVITY_PEAK_PCT = 99.9  # robust scale for the linear colour image
_RECON_EPS = 1e-12


def _footprint_from_lum(lum: np.ndarray, frac: float) -> np.ndarray:
    """Nebula footprint: pixels brighter than ``sky_floor + frac*(peak-floor)``.

    ``sky_floor`` and ``peak`` are the 10th / 99.5th percentiles of ``lum`` (robust
    to a few hot pixels and to the right-skewed emission tail). ``frac`` (default
    0.05) keeps everything within 5% of the sky floor out of the footprint, so the
    geometry bowl is fit over nebular gas rather than sky.
    """
    floor = float(np.percentile(lum, _FOOTPRINT_SKY_PCT))
    peak = float(np.percentile(lum, _FOOTPRINT_PEAK_PCT))
    rng = max(peak - floor, _RECON_EPS)
    mask = lum > (floor + float(frac) * rng)
    if not mask.any():
        mask = np.ones(lum.shape, dtype=bool)
    return mask


def _linear_emissivity(channels: ChannelSet) -> np.ndarray:
    """Linear (ny, nx, 3) colour image: [O III] -> teal, Halpha/[S II] -> red.

    SHO/HOO-like linear mapping, scaled by a single robust percentile (so the
    mapping stays linear). ``sii`` may be ``None`` (duoband / OSC); it is treated
    as zero and simply contributes nothing extra to the red channel.
    """
    ha = np.clip(np.nan_to_num(channels.ha.astype(np.float64)), 0.0, None)
    oiii = np.clip(np.nan_to_num(channels.oiii.astype(np.float64)), 0.0, None)
    if channels.sii is not None:
        sii = np.clip(np.nan_to_num(channels.sii.astype(np.float64)), 0.0, None)
    else:
        sii = np.zeros_like(ha)

    peak = float(np.percentile(np.stack([ha, oiii, sii]), _EMISSIVITY_PEAK_PCT))
    if peak <= _RECON_EPS:
        peak = 1.0

    r = (ha + sii) / peak          # Halpha + [S II]
    g = 0.5 * oiii / peak          # half-weight green -> teal, not pure green
    b = oiii / peak                # [O III]
    em = np.stack([r, g, b], axis=-1)
    return np.clip(em, 0.0, None).astype(np.float32)


def _capture_mode(channels: ChannelSet) -> str:
    c = channels.ratio_confidence
    if channels.sii is not None and abs(c - CONF_SHO) < 1e-6:
        return "sho"
    if abs(c - CONF_DUOBAND) < 1e-6:
        return "duoband"
    if abs(c - CONF_OSC) < 1e-6 or channels.sii is None:
        return "osc"
    if c == 0.0:
        return "nonlinear"
    return "unknown"


def reconstruct_cavity(
    channels: ChannelSet,
    ionization: IonizationMap,
    ionizer: Ionizer,
    lum: np.ndarray,
    dust: np.ndarray,
    pc_per_px: float | None,
    config: ReconConfig,
) -> CavityModel:
    """Assemble the three Task-7 depth terms into one blister ``CavityModel``.

    Blend (all terms share the convention 0 = nearest observer, 1 = farthest):

        c      = clip(ionization.confidence, 0, 1)
        w_geo  = w_geometry
        w_ion  = w_ionization * c                 # CONFIDENCE-WEIGHTED
        z_base = (w_geo*z_geo + w_ion*z_ion) / (w_geo + w_ion)
        z      = z_base + emission_sign * w_emission * relief   # signed limb bulge
        z      = z * (1 - w_dust*dust)            # near-side dust -> foreground
        z      = clip(z, 0, 1), sky (outside footprint) forced to 0

    Because the weight on ``ionization_depth`` scales with confidence, low-confidence
    OSC data collapses toward ``geometry_prior`` and still yields a concave bowl
    (apex deepest) rather than a flat or convex core. The emission relief is already
    signed and zero-mean; ``emission_sign=+1`` keeps the Task-7 convention that a
    bright limb ridge bulges toward the observer (smaller z).

    ``apex_xy`` is the ionizer's projected position. ``emissivity`` is a linear
    colour image ([O III]->teal, Halpha/[S II]->red); ``sii=None`` is supported.
    The footprint handed to ``geometry_prior`` is derived here from ``lum`` (Note 5).
    """
    validate_shapes(channels.ha, channels.oiii, channels.sii,
                    ionization.oiii_fraction, lum, dust)

    L = np.clip(np.nan_to_num(np.asarray(lum, dtype=np.float64)), 0.0, None)
    ny, nx = L.shape[:2]

    foot = _footprint_from_lum(L, config.footprint_frac)
    apex = (float(ionizer.x), float(ionizer.y))

    z_geo = geometry_prior((ny, nx), apex, foot, power=config.geometry_power).astype(np.float64)
    z_ion = ionization_depth(ionization, gamma=config.ionization_gamma).astype(np.float64)

    c = float(np.clip(ionization.confidence, 0.0, 1.0))
    w_geo = max(float(config.w_geometry), 0.0)
    w_ion = max(float(config.w_ionization), 0.0) * c
    denom = w_geo + w_ion
    if denom <= _RECON_EPS:
        z = z_geo.copy()  # degenerate config: fall back to the geometric bowl
    else:
        z = (w_geo * z_geo + w_ion * z_ion) / denom

    relief = emission_measure_relief(L, gain=1.0).astype(np.float64)
    z = z + float(config.emission_sign) * float(config.w_emission) * relief

    d = np.clip(np.nan_to_num(np.asarray(dust, dtype=np.float64)), 0.0, 1.0)
    z = z * (1.0 - float(np.clip(config.w_dust, 0.0, 1.0)) * d)

    z = np.where(foot, z, 0.0)
    z = np.clip(z, 0.0, 1.0).astype(np.float32)

    meta = {
        "w_geometry": w_geo,
        "w_ionization_max": float(config.w_ionization),
        "w_ionization_effective": w_ion,
        "w_emission": float(config.w_emission),
        "emission_sign": float(config.emission_sign),
        "w_dust": float(config.w_dust),
        "ionization_confidence": c,
        "ratio_confidence": float(channels.ratio_confidence),
        "is_linear": bool(channels.is_linear),
        "sii_present": channels.sii is not None,
        "capture_mode": _capture_mode(channels),
        "ionizer_source": ionizer.source,
        "footprint_frac": float(config.footprint_frac),
        "footprint_px": int(foot.sum()),
    }

    return CavityModel(
        z=z,
        emissivity=_linear_emissivity(channels),
        dust=d.astype(np.float32),
        pc_per_px=pc_per_px,
        apex_xy=apex,
        meta=meta,
    )
