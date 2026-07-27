"""Deterministic action ranking (SS6.2, SS3.5).

No AI. The VLM director is Phase 3 and must earn its place there. Non-scale
actions are ranked off the distance function's own decomposition: whichever
component carries the largest aggregate gap gets its remedies proposed first.
Scale-denominated actions (local_contrast, local_equalize) are ranked instead
by the structural deficit AT THEIR OWN band (SS3.5) -- an aggregate spectrum
gap is a single scalar and cannot tell a 2" deficit from a 32" one, so scoring
every band's action off of it collapses the whole band structure onto a tie,
decided only by an accidental tiebreak (the failure mode this fix closes).
"""

from __future__ import annotations

import numpy as np

from autocontrast.fingerprint.bandlimit import band_limit
from autocontrast.fingerprint.distance import (
    _background_distance, _chroma_distance, _spectrum_distance, _tonal_distance,
)
from autocontrast.fingerprint.palette import palette_chroma_compatible

from .actions import Action, available_actions, layer_for_scale

# Which action kinds address which fingerprint component (aggregate score).
# local_contrast/local_equalize are NOT here -- they are scale-denominated and
# ranked per-band by _band_deficits instead (SS3.5).
_REMEDIES = {
    "spectrum": ("core_hdr",),
    "tonal": ("tonal_reshape", "black_point"),
    "chroma": ("chroma",),
    "background": ("background_neutralize", "black_point"),
}

_SCALE_KINDS = frozenset({"local_contrast", "local_equalize"})


def component_gaps(ref, target) -> dict[str, float]:
    """Per-component distance between reference and target."""
    spectrum, usable = _spectrum_distance(ref, target)
    return {
        "spectrum": spectrum if usable else 0.0,
        "tonal": _tonal_distance(ref, target),
        "chroma": _chroma_distance(ref, target),
        "background": _background_distance(ref, target),
    }


def _band_deficits(ref, target) -> np.ndarray:
    """Per-wavelet-layer structural deficit: ``ref_shape - target_shape`` at each
    of the target's own bands (SS3.5), indexed the same way ``scale_for_layer``/
    ``layer_for_scale`` index a band (plane ``i`` -> ``target.energy.energy[i]``).

    Positive means the target is short of the reference's structure at that
    scale -- exactly where local_contrast/local_equalize at that band should be
    proposed. Bands outside the comparable window -- including every band, when
    band-limiting is unusable entirely -- score 0: neither confirmed short nor
    confirmed matched, so neither promoted nor penalized.
    """
    deficits = np.zeros_like(target.energy.energy)
    bl = band_limit(ref.energy, target.energy, ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec)
    if not bl.usable:
        return deficits
    mask = (target.energy.centers_arcsec >= bl.lo_arcsec) & (target.energy.centers_arcsec <= bl.hi_arcsec)
    deficits[mask] = bl.ref_shape - bl.target_shape
    return deficits


def propose_actions(
    ref, target, *, applied_kinds: frozenset[str], n_scales: int, top_k: int
) -> list[Action]:
    """The top_k actions most likely to close the largest gap.

    local_contrast/local_equalize are ranked by the structural deficit at their
    own band (SS3.5); every other action kind is ranked by the aggregate
    component gap it addresses. Chroma actions are withheld entirely when the
    palette gate is closed (SS2.3); the gate is consulted here as well as in the
    distance function so a mismatched reference can never even suggest a color
    move.

    Every action kind that isn't once-only carries three tied magnitude levels
    (gentle/moderate/strong) that always score identically -- if the ranking
    just sorted-and-sliced, the single highest-priority (kind, band) pair would
    supply all three of its levels and fill top_k on its own, monopolising the
    slate exactly the way the first implementation's spectrum collapse did
    (SS3.5: "no single kind may monopolise top_k"). So actions are grouped by
    (priority, band, kind) -- i.e. by everything BUT magnitude -- and top_k is
    filled by round-robin across those groups in priority order: one action
    from each group per round, gentlest level first. A single dominant remedy
    still comes first every round, but a second- and third-place remedy get a
    look before the top remedy's own moderate/strong repeats do.
    """
    compatible = palette_chroma_compatible(ref.palette_class, target.palette_class)
    gaps = component_gaps(ref, target)
    if not compatible:
        gaps["chroma"] = 0.0
    deficits = _band_deficits(ref, target)

    menu = available_actions(
        pixel_scale_arcsec=target.pixel_scale_arcsec,
        psf_fwhm_arcsec=max(ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec),
        n_scales=n_scales,
        palette_compatible=compatible,
        applied_kinds=applied_kinds,
    )

    def group_key(action: Action) -> tuple[float, float, str]:
        if action.kind in _SCALE_KINDS:
            layer = layer_for_scale(action.scale_arcsec, target.pixel_scale_arcsec)
            priority = float(deficits[layer]) if 0 <= layer < len(deficits) else 0.0
        else:
            priority = 0.0
            for component, kinds in _REMEDIES.items():
                if action.kind in kinds:
                    priority = max(priority, gaps[component])
        # Genuinely numeric/categorical fields -- scale_arcsec compared as a
        # float, never as the formatted string in action.key (which
        # string-sorts "16.000" before "2.000"). ``kind`` is included so that
        # two different remedies tied on the same band/priority (e.g.
        # background_neutralize and black_point, which share the background
        # gap) are still treated as distinct groups rather than merged.
        scale_key = action.scale_arcsec if action.scale_arcsec is not None else -1.0
        return (-priority, scale_key, action.kind)

    groups: dict[tuple[float, float, str], list[Action]] = {}
    for action in menu:
        groups.setdefault(group_key(action), []).append(action)
    # action.key is kept only as a final, purely-cosmetic backstop within a
    # group -- by then priority/band/kind are already tied and only magnitude
    # (action.strength) distinguishes members, so this never decides ordering
    # between different remedies.
    ordered_groups = sorted(groups)
    for gk in ordered_groups:
        groups[gk].sort(key=lambda a: (a.strength, a.key))

    proposed: list[Action] = []
    round_idx = 0
    while len(proposed) < top_k and any(round_idx < len(groups[gk]) for gk in ordered_groups):
        for gk in ordered_groups:
            if len(proposed) >= top_k:
                break
            members = groups[gk]
            if round_idx < len(members):
                proposed.append(members[round_idx])
        round_idx += 1
    return proposed
