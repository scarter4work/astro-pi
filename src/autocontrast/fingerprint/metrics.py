"""Tonal, chroma, saturation, and background metrics (§4.2).

All operate on already-converted CIELAB channels (see :mod:`.color`) except the
starlet-based background ratio and the channel-balance check, which need
luminance and RGB respectively. Everything is FOV- and exposure-robust: quantiles
and normalized distributions only — never a raw histogram of absolute values
(§4.2).
"""

from __future__ import annotations

import numpy as np

from .starlet import starlet_transform

# Quantile levels (percent) used for both tonal (L*) and saturation (C*) vectors.
QUANTILE_LEVELS: list[float] = [1, 5, 10, 25, 50, 75, 90, 95, 99, 99.9]


def tonal_quantiles(luminance: np.ndarray) -> np.ndarray:
    """L* quantiles at :data:`QUANTILE_LEVELS` — tonal placement without
    FOV-dependent histogram shape."""
    return np.percentile(luminance, QUANTILE_LEVELS)


def saturation_quantiles(chroma: np.ndarray) -> np.ndarray:
    """Quantiles of C* magnitude — distinguishes tasteful saturation structure
    from a flat global boost."""
    return np.percentile(chroma, QUANTILE_LEVELS)


def chroma_histogram(a: np.ndarray, b: np.ndarray, bins: int = 16, extent: float = 100.0) -> np.ndarray:
    """Chroma-weighted 2D histogram over the a*/b* plane, normalized to sum 1.

    Each pixel contributes weight equal to its chroma magnitude C*, so a large
    neutral background does not dominate the genuinely colorful pixels. A fully
    neutral image yields an all-zero histogram. ``extent`` fixes the a*/b* range
    to ``[-extent, +extent]`` so histograms from different images share a grid
    (required for the chroma EMD in §4.5).
    """
    weights = np.hypot(a, b)
    edges = np.linspace(-extent, extent, bins + 1)
    hist, _, _ = np.histogram2d(
        a.ravel(), b.ravel(), bins=[edges, edges], weights=weights.ravel()
    )
    total = hist.sum()
    return hist / total if total > 0 else hist


def floor_to_peak_ratio(luminance: np.ndarray) -> float:
    """Background floor over signal peak (§4.2): faint-end p1 divided by bright-end
    p99.9. ~1 for a flat field, small when a dark floor sits under a bright peak."""
    floor = np.percentile(luminance, 1)
    peak = np.percentile(luminance, 99.9)
    if peak == 0:
        return 0.0
    return float(floor / peak)


def structure_to_gradient_ratio(luminance: np.ndarray, n_scales: int = 5) -> float:
    """Ratio of multiscale structure energy to residual gradient energy.

    The starlet residual is dominated by the image mean, so gradient energy is the
    residual's *variance* (DC removed) — that is the uncorrected sky gradient.
    Structure energy is the summed energy of the (zero-mean) wavelet planes. High
    ratio = detail dominates; low ratio = a gradient/muddy floor dominates.
    """
    planes, residual = starlet_transform(luminance, n_scales=n_scales)
    structure = float(np.sum(planes**2))
    gradient = float(np.sum((residual - residual.mean()) ** 2))
    if gradient == 0:
        return float("inf") if structure > 0 else 0.0
    return structure / gradient


def background_channel_balance(rgb: np.ndarray, percentile: float = 25.0) -> np.ndarray:
    """Per-channel background level, normalized so a neutral background reads
    ``[1, 1, 1]`` and a color cast shows as a deviating channel (§4.2).

    The background is the darkest ``percentile`` fraction of pixels by luminance.
    """
    luminance = rgb.mean(axis=-1)
    threshold = np.percentile(luminance, percentile)
    mask = luminance <= threshold
    channel_means = np.array([rgb[..., c][mask].mean() for c in range(rgb.shape[-1])])
    overall = channel_means.mean()
    if overall == 0:
        return np.ones_like(channel_means)
    return channel_means / overall
