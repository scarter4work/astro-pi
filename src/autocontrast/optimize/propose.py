"""Deterministic action ranking (SS6.2).

No AI. The VLM director is Phase 3 and must earn its place there. Ranking works
off the distance function's own decomposition: whichever component carries the
largest gap gets its remedies proposed first.
"""

from __future__ import annotations

from autocontrast.fingerprint.distance import (
    _background_distance, _chroma_distance, _spectrum_distance, _tonal_distance,
)
from autocontrast.fingerprint.palette import palette_chroma_compatible

from .actions import Action, available_actions

# Which action kinds address which fingerprint component.
_REMEDIES = {
    "spectrum": ("local_contrast", "local_equalize", "core_hdr"),
    "tonal": ("tonal_reshape", "black_point"),
    "chroma": ("chroma",),
    "background": ("background_neutralize", "black_point"),
}


def component_gaps(ref, target) -> dict[str, float]:
    """Per-component distance between reference and target."""
    spectrum, usable = _spectrum_distance(ref, target)
    return {
        "spectrum": spectrum if usable else 0.0,
        "tonal": _tonal_distance(ref, target),
        "chroma": _chroma_distance(ref, target),
        "background": _background_distance(ref, target),
    }


def propose_actions(
    ref, target, *, applied_kinds: frozenset[str], n_scales: int, top_k: int
) -> list[Action]:
    """The top_k actions most likely to close the largest gap.

    Chroma actions are withheld entirely when the palette gate is closed (SS2.3);
    the gate is consulted here as well as in the distance function so a
    mismatched reference can never even suggest a color move.
    """
    compatible = palette_chroma_compatible(ref.palette_class, target.palette_class)
    gaps = component_gaps(ref, target)
    if not compatible:
        gaps["chroma"] = 0.0

    menu = available_actions(
        pixel_scale_arcsec=target.pixel_scale_arcsec,
        psf_fwhm_arcsec=max(ref.psf_fwhm_arcsec, target.psf_fwhm_arcsec),
        n_scales=n_scales,
        palette_compatible=compatible,
        applied_kinds=applied_kinds,
    )

    # Rank by the gap the action addresses; ties broken on the action key so the
    # ordering is fully reproducible for a given (ref, target).
    def rank(action: Action) -> tuple[float, str]:
        best = 0.0
        for component, kinds in _REMEDIES.items():
            if action.kind in kinds:
                best = max(best, gaps[component])
        return (-best, action.key)

    return sorted(menu, key=rank)[:top_k]
