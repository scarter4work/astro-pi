from __future__ import annotations

import tomllib
from dataclasses import dataclass, fields, replace


@dataclass(frozen=True)
class Gains:
    brightness: float = 0.5
    size: float = 0.4
    contrast: float = 0.3
    saturation: float = 0.3


@dataclass(frozen=True)
class GradeConfig:
    gains: Gains = Gains()
    p_low: float = 5.0
    p_high: float = 95.0
    match_tolerance_px: float = 3.0
    min_match_rate: float = 0.3
    detect_fwhm: float = 3.0
    detect_threshold_sigma: float = 5.0
    base_sigma_px: float = 2.0
    cache_dir: str = ".cache"
    neutral_strength: float = 0.0


def _apply(obj, data: dict):
    valid = {f.name for f in fields(obj)}
    unknown = set(data) - valid
    if unknown:
        raise ValueError(f"Unknown config keys: {sorted(unknown)}")
    return data


def load_config(path: str | None) -> GradeConfig:
    if path is None:
        return GradeConfig()
    with open(path, "rb") as fh:
        raw = tomllib.load(fh)
    gains_raw = raw.pop("gains", {})
    _apply(GradeConfig(), raw)
    _apply(Gains(), gains_raw)
    gains = replace(Gains(), **gains_raw)
    return replace(GradeConfig(), gains=gains, **raw)
