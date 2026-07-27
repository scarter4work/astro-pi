"""The SS7 guardrails: hard constraints, evaluated every iteration.

A violation DISCARDS the candidate and rolls back. Guardrails are never a score
term -- as a penalty, a large enough distance gain could buy its way past a noise
explosion. They are a filter.

SS7 annotates noise and star metrics as "PI native". We compute them in Python
from pixels instead, deliberately: routing them through PixInsight would make
them behave differently offline than in production, and SS7 violations are what
stop this tool fabricating -- the last thing that should go untested.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from autocontrast.fingerprint.starlet import starlet_transform

# Median absolute deviation -> Gaussian sigma, for the finest wavelet plane.
_MAD_TO_SIGMA = 1.4826


@dataclass(frozen=True)
class GuardrailLimits:
    """Trip thresholds. Calibrated against real data, never tuned to rescue a
    single failing test (spec SS3.7)."""

    noise_growth: float = 0.15          # fractional sigma increase from checkpoint
    shadow_clip_fraction: float = 1e-4  # ~0.01% of pixels at 0
    highlight_clip_fraction: float = 1e-4
    star_count_drop: float = 0.05       # fractional loss of detected stars
    star_fwhm_growth: float = 0.20
    star_ecc_growth: float = 0.20
    hue_invention_mass: float = 1e-3    # chroma mass with no support in source
    channel_ratio_drift: float = 0.10


@dataclass(frozen=True)
class GuardrailVerdict:
    name: str
    ok: bool
    reason: str
    value: float
    limit: float


def mrs_noise_sigma(gray: np.ndarray) -> float:
    """Noise sigma from the finest starlet plane via a robust MAD estimator.

    The finest wavelet plane is dominated by noise rather than structure, so its
    MAD is a stable noise estimate that does not require a blank sky region.
    """
    planes, _residual = starlet_transform(gray, n_scales=1)
    finest = planes[0]
    mad = float(np.median(np.abs(finest - np.median(finest))))
    return mad * _MAD_TO_SIGMA


def _gray(rgb: np.ndarray) -> np.ndarray:
    return rgb.mean(axis=-1)


def clipped_fraction(rgb: np.ndarray, at: float) -> tuple[float, float, float]:
    """Fraction of pixels sitting exactly at ``at``, per channel."""
    return tuple(float(np.mean(rgb[..., c] == at)) for c in range(rgb.shape[-1]))


def check_noise_floor(
    candidate: np.ndarray, baseline: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    base = mrs_noise_sigma(_gray(baseline))
    cand = mrs_noise_sigma(_gray(candidate))
    growth = 0.0 if base <= 0 else (cand - base) / base
    ok = growth <= limits.noise_growth
    return GuardrailVerdict(
        name="noise_floor", ok=ok,
        reason="" if ok else (
            f"noise sigma grew {growth:.1%} from checkpoint "
            f"(limit {limits.noise_growth:.1%})"
        ),
        value=growth, limit=limits.noise_growth,
    )


def check_shadow_clipping(candidate: np.ndarray, limits: GuardrailLimits) -> GuardrailVerdict:
    worst = max(clipped_fraction(candidate, at=0.0))
    ok = worst <= limits.shadow_clip_fraction
    return GuardrailVerdict(
        name="shadow_clipping", ok=ok,
        reason="" if ok else (
            f"{worst:.4%} of pixels crushed to zero (limit {limits.shadow_clip_fraction:.4%})"
        ),
        value=worst, limit=limits.shadow_clip_fraction,
    )


def check_highlight_clipping(candidate: np.ndarray, limits: GuardrailLimits) -> GuardrailVerdict:
    worst = max(clipped_fraction(candidate, at=1.0))
    ok = worst <= limits.highlight_clip_fraction
    return GuardrailVerdict(
        name="highlight_clipping", ok=ok,
        reason="" if ok else (
            f"{worst:.4%} of pixels blown to white (limit {limits.highlight_clip_fraction:.4%})"
        ),
        value=worst, limit=limits.highlight_clip_fraction,
    )
