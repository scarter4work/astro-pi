"""Shared data types for the nebula 3D depth reconstruction.

Conventions:
- All 2-D arrays are float32 with shape (ny, nx).
- ``emissivity`` is (ny, nx, 3), linear.
- ``dust`` is (ny, nx), opacity in [0, 1].
- ``z`` is pixel-relative depth: observer at -inf, larger = farther.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


def validate_shapes(*arrays) -> None:
    """Raise ValueError unless all non-None arrays share the same leading (ny, nx) shape."""
    shapes = [a.shape[:2] for a in arrays if a is not None]
    if len(set(shapes)) > 1:
        raise ValueError(f"array shape mismatch: {shapes}")


@dataclass(frozen=True)
class ChannelSet:
    ha: np.ndarray
    oiii: np.ndarray
    sii: np.ndarray | None
    ratio_confidence: float
    is_linear: bool

    def __post_init__(self) -> None:
        validate_shapes(self.ha, self.oiii, self.sii)


@dataclass(frozen=True)
class IonizationMap:
    oiii_fraction: np.ndarray  # (ny, nx), values in [0, 1]
    confidence: float


@dataclass(frozen=True)
class Ionizer:
    x: float
    y: float
    distance_pc: float | None
    source: str  # one of "gaia-blue", "gaia-bright", "field-center", "override"


@dataclass(frozen=True)
class ReconConfig:
    """Tunable weights for the depth blend plus 2D-render parameters.

    One config object flows through both ``reconstruct_cavity`` (Task 8) and the
    Task 9/10 ``render_cavity_2d(model, config)``, so it carries both families of
    knobs. A bare ``ReconConfig()`` is a sensible default for real captures.
    """

    # --- depth-blend weights (reconstruct_cavity) ---
    w_geometry: float = 1.0
    """Weight on ``geometry_prior``. Always on, so low-confidence data still gets
    a concave bowl from geometry alone."""
    w_ionization: float = 0.6
    """MAXIMUM weight on ``ionization_depth``; the weight actually used is
    ``w_ionization * ionization.confidence`` (confidence-weighted blending)."""
    w_emission: float = 0.15
    """Gain on the signed ``emission_measure_relief`` folded into z (limb bulges)."""
    emission_sign: float = 1.0
    """Sign applied to the emission relief. +1 keeps the Task-7 convention
    (a bright limb ridge bulges toward the observer -> smaller z)."""
    w_dust: float = 0.8
    """How strongly near-side dust pulls z toward the foreground: a pixel with
    opacity 1 is scaled to ``(1 - w_dust)`` of its depth."""
    footprint_frac: float = 0.05
    """Nebula footprint = ``lum > sky_floor + footprint_frac * (peak - sky_floor)``
    with sky_floor/peak the 10th/99.5th percentiles of ``lum``."""
    geometry_power: float = 2.0
    """Paraboloid steepness passed through to ``geometry_prior``."""
    ionization_gamma: float = 1.0
    """Shaping exponent passed through to ``ionization_depth``."""

    # --- 2D render parameters (render_cavity_2d, Task 9/10) ---
    render_gamma: float = 1.0
    """Display gamma applied by the 2D render."""
    render_ambient: float = 0.05
    """Ambient floor so the deepest regions are not rendered pure black."""
    render_depth_shading: float = 0.5
    """How strongly the 2D render darkens far (large-z) gas as a depth cue."""
    render_dust_strength: float = 1.0
    """How strongly dust opacity darkens the 2D render."""


@dataclass(frozen=True)
class CavityModel:
    z: np.ndarray           # (ny, nx) pixel-relative depth
    emissivity: np.ndarray  # (ny, nx, 3) linear
    dust: np.ndarray        # (ny, nx) opacity in [0, 1]
    pc_per_px: float | None
    apex_xy: tuple[float, float]
    meta: dict

    def __post_init__(self) -> None:
        validate_shapes(self.z, self.emissivity, self.dust)
