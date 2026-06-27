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
    # 4 px (not 3) absorbs the ~2 px astrometric scatter left by exporting PI's
    # LINEAR WCS (PI solves with a spline; we only carry CD+TAN). Validated on a
    # real HaO3 master: lifts match_rate 0.27 -> 0.43 with ~80% of added matches real.
    match_tolerance_px: float = 4.0
    min_match_rate: float = 0.3
    detect_fwhm: float = 3.0
    # 6 sigma trims the faintest, noisiest detections (near-neutral on match_rate).
    detect_threshold_sigma: float = 6.0
    base_sigma_px: float = 2.0
    cache_dir: str = ".cache"
    neutral_strength: float = 0.0
    # Drop Gaia sources fainter than this G magnitude before matching. Bailer-Jones
    # geometric distances grow unreliable faintward, and faint sources also dominate
    # the catalogue count in dense fields (Cygnus: 160k in a 0.74deg cone) without
    # improving the visual depth effect. Cutting them speeds the (paginated, sync)
    # query and raises match quality. Set to None to disable the cut (full depth).
    gaia_mag_limit: float | None = 18.0
    # Parallax signal-to-noise floor for the offline local-DR3 source (PI's Gaia
    # process gives parallax but no error, so we synthesize sigma from G and cut
    # on parallax/sigma). Sources below this are dropped loudly. Unused online.
    gaia_parallax_snr: float = 5.0


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
