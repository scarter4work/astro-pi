from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .config import Gains


@dataclass(frozen=True)
class Modulation:
    brightness: np.ndarray
    size: np.ndarray
    contrast: np.ndarray
    saturation: np.ndarray


def compute_modulation(strength: np.ndarray, gains: Gains) -> Modulation:
    s = np.asarray(strength, dtype=float)
    return Modulation(
        brightness=1.0 + gains.brightness * s,
        size=1.0 + gains.size * s,
        contrast=gains.contrast * s,
        saturation=1.0 + gains.saturation * s,
    )
