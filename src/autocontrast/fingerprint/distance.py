"""The fingerprint distance function (design §4.5).

    D = w_e · D_spectrum   (band-limited, normalized shape)
      + w_t · D_tonal       (L2 over quantile vector)
      + w_c · D_chroma       (EMD over a*/b* histogram)   [0 if palette mismatch]
      + w_b · D_background   (weighted L2)

Weights are configuration, not constants — ship sane defaults, expose them. The
one rule that is NOT a tuning knob: when the reference's palette class does not
match the acquisition's, the chroma term is dropped entirely (§2.3). Letting the
reference's color cloud influence a palette-mismatched image is the failure that
turns this tool into a fabrication engine (§2.1, §12). That gate lives in code,
never in the weights a user can edit.
"""

from __future__ import annotations

from dataclasses import dataclass, replace

import numpy as np
from scipy.stats import wasserstein_distance_nd

from .bandlimit import band_limit
from .palette import palette_chroma_compatible


@dataclass(frozen=True)
class DistanceWeights:
    """Relative importance of each fingerprint component. Defaults sum to 1.0;
    the spectrum (the depth signature, §4.2) carries the most weight."""

    spectrum: float = 0.40
    tonal: float = 0.25
    chroma: float = 0.20
    background: float = 0.15


@dataclass(frozen=True)
class DistanceComponents:
    """The four scalar component distances, each already computed and (roughly)
    commensurate in [0, ~1]. Combining them is a separate, deliberate step so the
    palette gate is impossible to bypass."""

    spectrum: float
    tonal: float
    chroma: float
    background: float


def combine_distance(
    components: DistanceComponents,
    weights: DistanceWeights = DistanceWeights(),
    *,
    palette_match: bool,
) -> float:
    """Combine the four component distances into a single fingerprint distance D.

    Contract (pinned by tests):
      * When ``palette_match`` is False, the chroma component must have ZERO
        influence on the result, whatever its value (§2.3). This is the hard gate.
      * When all components are equal and the palette matches, D equals that common
        value (the spectrum/tonal/chroma/background weights sum to 1.0).
      * D is non-negative for non-negative inputs.

    Design decision left to you: when chroma is gated out on a palette mismatch, do
    you (a) simply drop the ``w_c · D_chroma`` term so the maximum possible D is
    ``1 - w_c``, or (b) redistribute ``w_c`` across the surviving three components
    so D stays on a [0, 1] scale? Both are defensible; (a) makes a mismatched
    reference score systematically "closer", (b) keeps scores comparable across
    matched and mismatched references. Pick one and encode it here.
    """
    # Option (b): gate chroma out on palette mismatch (§2.3), then redistribute its
    # weight across the surviving components so matched and mismatched references
    # stay on one comparable [0, 1] scale.
    w_c = weights.chroma if palette_match else 0.0
    used_weight = weights.spectrum + weights.tonal + w_c + weights.background
    total = (
        weights.spectrum * components.spectrum
        + weights.tonal * components.tonal
        + w_c * components.chroma
        + weights.background * components.background
    )
    return total / used_weight


# --------------------------------------------------------------------------
# Component distances: each maps a pair of fingerprints to a roughly [0, 1]
# scalar so the weights above are meaningful. Absolute scale matters less than
# monotonic sensibleness — the Phase 0 exit criterion is about *ranking*.
# --------------------------------------------------------------------------

_L_MAX = 100.0  # CIELAB L* upper bound, for normalizing tonal quantile distance.


def _spectrum_distance(ref, target) -> tuple[float, bool]:
    """Band-limited spectral shape distance (§4.4, §4.5).

    Returns ``(distance, usable)``. The two L2-normalized shapes lie in the
    positive orthant, so their Euclidean distance is in ``[0, sqrt(2)]``; dividing
    by ``sqrt(2)`` puts it on ``[0, 1]``. When the bands do not overlap, spectral
    guidance is unusable and the caller drops this component (§4.4)."""
    bl = band_limit(ref.energy, target.energy, ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec)
    if not bl.usable:
        return 0.0, False
    dist = float(np.linalg.norm(bl.ref_shape - bl.target_shape) / np.sqrt(2.0))
    return dist, True


def _tonal_distance(ref, target) -> float:
    """RMS difference of L* quantiles, normalized by the L* range."""
    diff = (ref.tonal_quantiles - target.tonal_quantiles) / _L_MAX
    return float(np.sqrt(np.mean(diff**2)))


def _chroma_distance(ref, target, extent: float = 100.0) -> float:
    """Earth Mover's Distance between the a*/b* chroma histograms, normalized by
    the grid diagonal. Zero if either image is neutral (empty histogram)."""
    h_ref, h_target = ref.chroma_hist, target.chroma_hist
    if h_ref.sum() == 0 or h_target.sum() == 0:
        return 0.0

    bins = h_ref.shape[0]
    edges = np.linspace(-extent, extent, bins + 1)
    centers = 0.5 * (edges[:-1] + edges[1:])
    grid_a, grid_b = np.meshgrid(centers, centers, indexing="ij")
    coords = np.column_stack([grid_a.ravel(), grid_b.ravel()])

    emd = wasserstein_distance_nd(coords, coords, h_ref.ravel(), h_target.ravel())
    diagonal = np.hypot(2 * extent, 2 * extent)
    return float(emd / diagonal)


def _squash(x: float) -> float:
    """Map an unbounded non-negative ratio into [0, 1) (inf -> 1)."""
    if not np.isfinite(x):
        return 1.0
    return x / (1.0 + x)


def _background_distance(ref, target) -> float:
    """Mean of the three background sub-distances (floor/peak, structure/gradient,
    channel balance), each normalized to roughly [0, 1] (§4.2 background block)."""
    rb, tb = ref.background, target.background
    d_floor = abs(rb["floor_to_peak_ratio"] - tb["floor_to_peak_ratio"])
    d_grad = abs(_squash(rb["structure_to_gradient_ratio"]) - _squash(tb["structure_to_gradient_ratio"]))
    balance_diff = np.asarray(rb["channel_balance"]) - np.asarray(tb["channel_balance"])
    d_balance = float(np.linalg.norm(balance_diff) / np.sqrt(len(balance_diff)))
    return float(np.mean([d_floor, d_grad, d_balance]))


def fingerprint_distance(ref, target, weights: DistanceWeights = DistanceWeights()) -> float:
    """Full fingerprint distance D (§4.5).

    ``ref`` and ``target`` are ``FingerprintData`` (see :mod:`.extract`). When the
    instruments share no resolvable band, the spectrum component is dropped by
    zeroing its weight — :func:`combine_distance`'s redistribution then rescales
    the survivors (the §4.4 fall-back to tonal/chroma/background)."""
    palette_match = palette_chroma_compatible(ref.palette_class, target.palette_class)

    d_spectrum, spectrum_usable = _spectrum_distance(ref, target)
    effective_weights = weights if spectrum_usable else replace(weights, spectrum=0.0)

    components = DistanceComponents(
        spectrum=d_spectrum,
        tonal=_tonal_distance(ref, target),
        chroma=_chroma_distance(ref, target),
        background=_background_distance(ref, target),
    )
    return combine_distance(components, effective_weights, palette_match=palette_match)
