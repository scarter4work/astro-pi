"""The discretized action space (design SS6.2).

No model -- and no heuristic in this package -- may set free-form process
parameters. The search space is a bounded menu; each entry carries 2-3 magnitude
levels. Actions are also constructed BAND-LIMITED: an action whose angular scale
sits below the image's own resolvable limit is never emitted at all (SS2.2, SS4.4).
That makes it structurally impossible to propose sharpening 0.05"/px HST detail
into 1.01"/px backyard data -- the failure SS2.2 exists to prevent.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

LEVELS = ("gentle", "moderate", "strong")

# Actions that change mode rather than degree, and may be applied at most once.
ONCE_ONLY = frozenset({"star_split", "background_neutralize"})

# Magnitude -> a unitless strength the executors interpret per process.
_STRENGTH = {"gentle": 0.25, "moderate": 0.5, "strong": 0.85}


@dataclass(frozen=True)
class Action:
    """One discrete move: what to do, how hard, and at what angular scale."""

    kind: str
    level: str
    scale_arcsec: float | None
    params: dict = field(default_factory=dict, compare=False)

    @property
    def key(self) -> str:
        """Stable identity, used for recipe distinctness in the beam."""
        scale = "-" if self.scale_arcsec is None else f"{self.scale_arcsec:.3f}"
        return f"{self.kind}@{scale}/{self.level or '-'}"

    @property
    def strength(self) -> float:
        return _STRENGTH.get(self.level, 1.0)


def scale_for_layer(layer: int, pixel_scale_arcsec: float) -> float:
    """Angular center of wavelet plane ``layer`` (matches energy.py's convention)."""
    return (2.0**layer) * pixel_scale_arcsec


def layer_for_scale(scale_arcsec: float, pixel_scale_arcsec: float) -> int:
    """Inverse of :func:`scale_for_layer`, rounded to the nearest plane."""
    if scale_arcsec < pixel_scale_arcsec:
        raise ValueError(
            f"scale {scale_arcsec}\" is below the pixel scale {pixel_scale_arcsec}\"/px; "
            "there is no wavelet plane there"
        )
    return int(round(math.log2(scale_arcsec / pixel_scale_arcsec)))


def available_actions(
    *,
    pixel_scale_arcsec: float,
    psf_fwhm_arcsec: float,
    n_scales: int,
    palette_compatible: bool,
    applied_kinds: frozenset[str],
) -> list[Action]:
    """The full menu legal for this image right now.

    ``palette_compatible`` False withholds every chroma action (SS2.3) -- the
    reference's color cloud must never push a palette-mismatched image.
    ``applied_kinds`` withholds once-only actions already spent.
    """
    actions: list[Action] = []

    bands = [
        scale_for_layer(i, pixel_scale_arcsec)
        for i in range(n_scales)
        if scale_for_layer(i, pixel_scale_arcsec) >= psf_fwhm_arcsec
    ]

    for band in bands:
        for level in LEVELS:
            actions.append(Action("local_contrast", level, band,
                                  {"layer": layer_for_scale(band, pixel_scale_arcsec)}))
            actions.append(Action("local_equalize", level, band,
                                  {"radius_arcsec": band}))

    for level in LEVELS:
        actions.append(Action("tonal_reshape", level, None, {"monotone": True}))
        actions.append(Action("black_point", level, None, {"clip_limited": True}))
        actions.append(Action("core_hdr", level, None,
                              {"layers": {"gentle": 2, "moderate": 3, "strong": 4}[level]}))
        if palette_compatible:
            actions.append(Action("chroma", level, None, {}))

    for kind in ("background_neutralize", "star_split"):
        if kind not in applied_kinds:
            actions.append(Action(kind, "", None, {}))

    return actions
