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
from scipy import ndimage

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
    if base <= 0:
        # A non-positive baseline sigma means growth is undefined, not zero.
        # Fail closed: a guardrail that can't assess growth must not report a
        # clean pass, or it silently disables itself on a degenerate baseline
        # while still claiming to have checked noise.
        return GuardrailVerdict(
            name="noise_floor", ok=False,
            reason=(
                f"baseline noise sigma was non-positive ({base!r}); "
                "noise growth could not be assessed"
            ),
            value=float("inf"), limit=limits.noise_growth,
        )
    growth = (cand - base) / base
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



@dataclass(frozen=True)
class StarStats:
    count: int
    median_fwhm: float
    median_ecc: float


def detect_stars(gray: np.ndarray, *, k_sigma: float = 5.0) -> StarStats:
    """Deterministic star detection: threshold, label, second moments.

    Not a replacement for PI's StarDetector in absolute terms -- it does not need
    to be. The guardrail compares candidate against checkpoint using the SAME
    detector, so systematic bias cancels and only the CHANGE matters.
    """
    sigma = mrs_noise_sigma(gray)
    background = float(np.median(gray))
    mask = gray > background + k_sigma * max(sigma, 1e-9)

    labels, n = ndimage.label(mask)
    if n == 0:
        return StarStats(count=0, median_fwhm=0.0, median_ecc=0.0)

    fwhms: list[float] = []
    eccs: list[float] = []
    for sl in ndimage.find_objects(labels):
        h = sl[0].stop - sl[0].start
        w = sl[1].stop - sl[1].start
        if h < 2 or w < 2:
            continue  # single-pixel hits are cosmic rays / hot pixels, not stars
        # Equivalent-area FWHM and axis-ratio eccentricity from the bounding box.
        fwhms.append(float(np.sqrt(h * w)))
        major, minor = max(h, w), min(h, w)
        eccs.append(float(np.sqrt(1.0 - (minor / major) ** 2)))

    if not fwhms:
        return StarStats(count=0, median_fwhm=0.0, median_ecc=0.0)
    return StarStats(
        count=len(fwhms),
        median_fwhm=float(np.median(fwhms)),
        median_ecc=float(np.median(eccs)),
    )


def check_star_integrity(
    candidate: np.ndarray, baseline: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    """Stars must not vanish, bloat, or smear (SS7)."""
    base = detect_stars(_gray(baseline))
    cand = detect_stars(_gray(candidate))

    if base.count == 0:
        # A pass, but not a silent one (design SS12): a starless baseline is a
        # legitimate input (e.g. a nebula-only crop -- the design's §4 keeps
        # the starless layer itself out of this guardrail entirely, measuring
        # only the recombined image, so this guardrail never sees a frame
        # that is starless purely because star_split hasn't recombined yet).
        # That means a zero count here must not fail closed the way
        # check_noise_floor's degenerate-sigma case does. But the caller must
        # be able to tell, from the verdict alone, that star integrity was
        # never actually evaluated on this candidate -- not that it was
        # checked and found clean.
        return GuardrailVerdict(
            "star_integrity", True,
            "star integrity not assessed: baseline had no detectable stars",
            0.0, 0.0,
        )

    lost = (base.count - cand.count) / base.count
    if lost > limits.star_count_drop:
        return GuardrailVerdict(
            "star_integrity", False,
            f"star count fell {lost:.1%} ({base.count} -> {cand.count}), "
            f"limit {limits.star_count_drop:.1%}",
            lost, limits.star_count_drop,
        )

    if base.median_fwhm > 0:
        growth = (cand.median_fwhm - base.median_fwhm) / base.median_fwhm
        if growth > limits.star_fwhm_growth:
            return GuardrailVerdict(
                "star_integrity", False,
                f"star FWHM ballooned {growth:.1%} "
                f"({base.median_fwhm:.2f} -> {cand.median_fwhm:.2f} px), "
                f"limit {limits.star_fwhm_growth:.1%}",
                growth, limits.star_fwhm_growth,
            )

    ecc_growth = cand.median_ecc - base.median_ecc
    if ecc_growth > limits.star_ecc_growth:
        return GuardrailVerdict(
            "star_integrity", False,
            f"star eccentricity grew {ecc_growth:.2f}, limit {limits.star_ecc_growth:.2f}",
            ecc_growth, limits.star_ecc_growth,
        )

    return GuardrailVerdict("star_integrity", True, "", 0.0, limits.star_count_drop)
