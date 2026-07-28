"""Deterministic action ranking (SS6.2, SS3.5).

No AI. The VLM director is Phase 3 and must earn its place there. Non-scale
actions are ranked off the distance function's own decomposition: whichever
component carries the largest aggregate gap gets its remedies proposed first.
Scale-denominated actions (local_contrast, local_equalize) are ranked instead
by the structural deficit AT THEIR OWN band (SS3.5) -- an aggregate spectrum
gap is a single scalar and cannot tell a 2" deficit from a 32" one, so scoring
every band's action off of it collapses the whole band structure onto a tie,
decided only by an accidental tiebreak (the failure mode this fix closes).

Every leveled kind also carries three tied magnitude levels for a given band,
which created a second, same-shaped monopoly once the first was fixed: with
LEVELS having exactly three entries and top_k defaulting to 3, a naive
sort-and-slice always let the single top (kind, band) group supply all three of
its own levels. Actions are grouped by everything but magnitude, one action per
group fills top_k, and that group's OWN magnitude is chosen by its gap size
(SS3.5, "Magnitude is chosen by gap size") rather than defaulting to gentle.
"""

from __future__ import annotations

import numpy as np

from autocontrast.fingerprint.bandlimit import band_limit
from autocontrast.fingerprint.distance import (
    _background_distance, _chroma_distance, _spectrum_distance, _tonal_distance,
)
from autocontrast.fingerprint.palette import palette_chroma_compatible

from .actions import Action, ONCE_ONLY, available_actions, layer_for_scale

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

# Magnitude bucket boundaries for "gap size -> level" (SS3.5, "Magnitude is chosen
# by gap size", spec commit e4d2611). Tunables under SS3.7's calibration
# discipline -- calibrated against the measured per-band deficits and aggregate
# component gaps across this project's own fixtures (task-7 report, fix round 2),
# not picked by feel. That survey found single-component mismatches between
# visually similar images clustering below ~0.10, and a wide, empirically empty
# range between ~0.17 and ~0.36 separating "a real but non-dominant issue" from
# "the dominant defect in the image" -- the boundaries sit inside those two
# observed breaks.
GAP_GENTLE_MAX = 0.12
GAP_MODERATE_MAX = 0.25


def _level_for_gap(gap: float) -> str:
    """Map a (kind, band) group's gap size to a magnitude (SS3.5): a large deficit
    warrants a large step. A gap at or below zero -- the band already has as much
    or more structure than the reference, or the group's own aggregate component
    gap happens to be zero -- falls through to the gentlest step, same as a small
    positive one; this function is never asked to pick a level for a group that
    isn't going to be proposed at all (that's decided by ranking, upstream)."""
    if gap >= GAP_MODERATE_MAX:
        return "strong"
    if gap >= GAP_GENTLE_MAX:
        return "moderate"
    return "gentle"


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

    Every leveled action kind carries three tied magnitude levels (gentle/
    moderate/strong) that always score identically for a given (kind, band) --
    sorting and slicing directly would let the single highest-priority group
    supply all three of its levels and monopolise top_k, exactly the way the
    first implementation's spectrum collapse did (SS3.5: "no single kind may
    monopolise top_k"). So actions are grouped by (priority, band, kind) --
    everything BUT magnitude -- and each group contributes exactly ONE action to
    top_k, in priority order. Kind diversity survives because every group still
    yields one entry; magnitude is no longer fixed at "gentle" but chosen by the
    size of that group's own gap (SS3.5, "Magnitude is chosen by gap size") --
    a large deficit gets a large step. Mode-change actions (background_neutralize,
    star_split) have no magnitude axis (level "") and are passed through as-is.
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

    def gap_for(action: Action) -> float:
        if action.kind in _SCALE_KINDS:
            layer = layer_for_scale(action.scale_arcsec, target.pixel_scale_arcsec)
            return float(deficits[layer]) if 0 <= layer < len(deficits) else 0.0
        gap = 0.0
        for component, kinds in _REMEDIES.items():
            if action.kind in kinds:
                gap = max(gap, gaps[component])
        return gap

    def group_key(action: Action) -> tuple[float, float, str]:
        # Genuinely numeric/categorical fields -- scale_arcsec compared as a
        # float, never as the formatted string in action.key (which
        # string-sorts "16.000" before "2.000"). ``kind`` is included so that
        # two different remedies tied on the same band/priority (e.g.
        # background_neutralize and black_point, which share the background
        # gap) are still treated as distinct groups rather than merged.
        scale_key = action.scale_arcsec if action.scale_arcsec is not None else -1.0
        return (-gap_for(action), scale_key, action.kind)

    groups: dict[tuple[float, float, str], list[Action]] = {}
    for action in menu:
        groups.setdefault(group_key(action), []).append(action)
    ordered_groups = sorted(groups)

    proposed: list[Action] = []
    for gk in ordered_groups:
        if len(proposed) >= top_k:
            break
        members = groups[gk]
        kind = members[0].kind
        if kind in ONCE_ONLY:
            # No magnitude axis -- a single, level-less member.
            proposed.append(members[0])
            continue
        gap = -gk[0]  # gk[0] is -gap_for(action); recover the group's own gap.
        level = _level_for_gap(gap)
        chosen = next((a for a in members if a.level == level), None)
        if chosen is None:
            # available_actions always emits all three LEVELS for every leveled
            # kind, so this should be unreachable -- surfaced loudly rather than
            # silently skipping the group or falling back to another level (SS12).
            raise AssertionError(
                f"no {level!r} action found for kind {kind!r} in group {gk!r}; "
                "available_actions should have emitted all three LEVELS"
            )
        proposed.append(chosen)
    return proposed
