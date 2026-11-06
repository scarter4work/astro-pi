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
class CavityModel:
    z: np.ndarray           # (ny, nx) pixel-relative depth
    emissivity: np.ndarray  # (ny, nx, 3) linear
    dust: np.ndarray        # (ny, nx) opacity in [0, 1]
    pc_per_px: float | None
    apex_xy: tuple[float, float]
    meta: dict

    def __post_init__(self) -> None:
        validate_shapes(self.z, self.emissivity, self.dust)
