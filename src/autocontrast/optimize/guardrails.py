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

from autocontrast.fingerprint.color import rgb_to_lab
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
    hue_tolerance_deg: float = 20.0     # SS5.1: bounded legitimate-boost drift margin
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


# Hue-angle histogram resolution (design SS5.1). This is a measurement grid,
# not a tolerance -- 1-degree bins, far finer than limits.hue_tolerance_deg
# (default 20 degrees), so discretization error against the tolerance is
# negligible. Bin width is no longer load-bearing the way it was in the first
# iteration: the tolerance below is what decides support, not bin membership.
NUM_HUE_BINS = 360

# Chroma floor (a*/b* units) below which a pixel's hue angle is dropped rather
# than measured. A pixel this close to neutral has an atan2(b, a) of two small,
# noisy numbers -- the angle is not meaningfully defined, and letting it vote
# would make "unsupported" a measure of noise, not of color. 1.0 sits comfortably
# below the ~2.3 CIELAB delta-E commonly cited as the smallest color difference a
# human observer reliably notices, so it excludes only pixels with no perceptible
# color at all.
HUE_CHROMA_FLOOR = 1.0


def _hue_mass_by_bin(rgb: np.ndarray) -> np.ndarray:
    """Chroma-weighted mass in each 1-degree hue bin (0-360 degrees), unnormalized.

    Pixels at or below ``HUE_CHROMA_FLOOR`` are excluded entirely, not binned as
    zero-chroma -- see the floor's docstring above. A source with no pixel above
    the floor (a perfectly neutral source) yields an all-zero histogram.
    """
    lab = rgb_to_lab(rgb)
    a, b = lab[..., 1], lab[..., 2]
    chroma = np.hypot(a, b)
    hue = np.degrees(np.arctan2(b, a)) % 360.0

    supported = chroma > HUE_CHROMA_FLOOR
    edges = np.linspace(0.0, 360.0, NUM_HUE_BINS + 1)
    hist, _ = np.histogram(hue[supported], bins=edges, weights=chroma[supported])
    return hist


def _dilate_circular(occupied: np.ndarray, radius_bins: int) -> np.ndarray:
    """OR ``occupied`` with itself shifted by up to ``radius_bins`` bins either
    way, wrapping at the array ends -- hue angle is circular, so a bin just
    above 359 degrees is adjacent to one just above 0, not far from it."""
    supported = occupied.copy()
    for shift in range(1, radius_bins + 1):
        supported |= np.roll(occupied, shift)
        supported |= np.roll(occupied, -shift)
    return supported


def check_hue_invention(
    candidate: np.ndarray, source: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    """No chroma mass may sit more than ``limits.hue_tolerance_deg`` from any hue
    the source supports (SS7).

    This is SS2.1 enforced mechanically, on hue ANGLE rather than a*/b* position
    (design SS5.1, twice revised on measurement). The a*/b* plane cannot tell
    "the same color, more of it" from "a color that was never there": a
    saturation boost moves a pixel radially outward, and radial motion crosses
    a*/b* grid cells even though the color itself hasn't changed. Hue angle can
    make the distinction, because saturation moves along a radius at
    (approximately) constant angle, while fabrication introduces a genuinely new
    one. "Approximately" is why this is a TOLERANCE, not exact-bin membership:
    saturation is applied in RGB, and CIELAB hue angle is a nonlinear function
    of RGB, so a legitimate boost drifts the angle by a few degrees even with no
    clipping at all. Measured legitimate drift is bounded around 6 degrees;
    measured invention is ~116 degrees or has no supporting hue whatsoever. The
    default tolerance (20 degrees) sits in that gap -- about 3x above real drift
    and 6x below real invention -- and, unlike a fixed a*/b* cell dilation, this
    bound cannot be escaped by boosting harder: radial movement in the a*/b*
    plane is unbounded, but angular drift from a legitimate operation is not.

    Detecting a channel clipped out of gamut is deliberately NOT this
    guardrail's job -- that's what ``check_shadow_clipping`` and
    ``check_highlight_clipping`` are for. Folding gamut-driven hue shift into
    this check is what produced the first iteration's over-strict result.

    A perfectly neutral source (an all-zero hue histogram, every pixel at or
    below ``HUE_CHROMA_FLOOR``) supports no hue at all, so ANY candidate chroma
    above the floor trips this check -- intentionally, not incidentally. Per
    §2.1, no color in the data means no color to legitimately intensify: a
    genuinely gray source has nothing here for a saturation move to amplify, so
    introducing color from a neutral source is fabrication by definition, not a
    boundary case this check happens to also catch.
    """
    src_hist = _hue_mass_by_bin(source)
    cand_hist = _hue_mass_by_bin(candidate)

    bin_width_deg = 360.0 / NUM_HUE_BINS
    radius_bins = int(np.ceil(limits.hue_tolerance_deg / bin_width_deg))
    supported = _dilate_circular(src_hist > 0.0, radius_bins)

    cand_total = cand_hist.sum()
    cand_norm = cand_hist / cand_total if cand_total > 0 else cand_hist

    mass = float(cand_norm[~supported].sum())
    ok = mass <= limits.hue_invention_mass
    return GuardrailVerdict(
        name="hue_invention", ok=ok,
        reason="" if ok else (
            f"hue invention: {mass:.4f} chroma mass appeared more than "
            f"{limits.hue_tolerance_deg:.0f} degrees from any hue the source "
            f"supports (limit {limits.hue_invention_mass:.4f})"
        ),
        value=mass, limit=limits.hue_invention_mass,
    )


def _channel_ratios(rgb: np.ndarray) -> np.ndarray:
    """Mean per-channel level, normalized to sum 1 -- the presentation-space stand-in
    for the linear channel ratios SS2.1 protects."""
    means = np.array([float(rgb[..., c].mean()) for c in range(rgb.shape[-1])])
    total = means.sum()
    return means / total if total > 0 else means


def check_channel_ratio_drift(
    candidate: np.ndarray, source: np.ndarray, limits: GuardrailLimits
) -> GuardrailVerdict:
    """Post-stretch channel ratios must stay near the source's (SS7, SS2.1)."""
    drift = float(np.linalg.norm(_channel_ratios(candidate) - _channel_ratios(source)))
    ok = drift <= limits.channel_ratio_drift
    return GuardrailVerdict(
        name="channel_ratio_drift", ok=ok,
        reason="" if ok else (
            f"channel ratios drifted {drift:.3f} from the source "
            f"(limit {limits.channel_ratio_drift:.3f})"
        ),
        value=drift, limit=limits.channel_ratio_drift,
    )


def evaluate_guardrails(
    candidate: np.ndarray,
    baseline: np.ndarray,
    source: np.ndarray,
    limits: GuardrailLimits = GuardrailLimits(),
) -> list[GuardrailVerdict]:
    """Every SS7 guardrail, all of them evaluated.

    We do not short-circuit on the first failure: a declined run should be able to
    report everything that was wrong, not just whichever check happened to run
    first (SS12 -- degraded paths surface).
    """
    return [
        check_noise_floor(candidate, baseline, limits),
        check_star_integrity(candidate, baseline, limits),
        check_shadow_clipping(candidate, limits),
        check_highlight_clipping(candidate, limits),
        check_hue_invention(candidate, source, limits),
        check_channel_ratio_drift(candidate, source, limits),
    ]
