"""Palette-class derivation from FITS FILTER keywords (design §4.3).

    "On the user's side, palette class is auto-derived from FITS headers — the
     FILTER keyword written by the ZWO EFW makes this deterministic. Never prompt
     for what the headers already state."

The palette class is what §2.3 gates on: a reference whose palette class does not
match the acquisition's may contribute structural and tonal components only, never
chroma. So an unrecognised filter must resolve to ``unknown`` (which forfeits chroma)
rather than being guessed into a class — guessing here would let a mismatched
reference push channel ratios, the exact fabrication failure §2.1 forbids.
"""

from __future__ import annotations

# §4.3
PALETTE_CLASSES = frozenset(
    {"LRGB", "RGB", "SHO", "HOO", "HaRGB", "HaOIII-RGB", "L-only", "unknown"}
)

# Dual-band (Ha + OIII) filters sold under many names; all yield an HOO palette.
_DUAL_BAND = {
    "hao3", "haoiii", "ha-o3", "ha-oiii", "haoiii-dual", "duo", "duo-band", "duoband",
    "dualband", "dual-band", "l-extreme", "l-enhance", "l-ultimate", "lextreme",
    "lultimate", "alp-t", "alpt",
}

# Canonical single-band / broadband names.
_ALIASES = {
    "ha": "Ha", "h-alpha": "Ha", "halpha": "Ha", "h_alpha": "Ha",
    "oiii": "OIII", "o3": "OIII",
    "sii": "SII", "s2": "SII",
    "l": "L", "lum": "L", "luminance": "L",
    "r": "R", "red": "R",
    "g": "G", "green": "G",
    "b": "B", "blue": "B",
}


def _canonical(filters: list[str]) -> set[str] | None:
    """Normalize filter names. Returns None if any name is unrecognised."""
    out: set[str] = set()
    for raw in filters:
        key = str(raw).strip().lower()
        if key in _DUAL_BAND:
            out.update({"Ha", "OIII"})  # a dual-band filter captures both
        elif key in _ALIASES:
            out.add(_ALIASES[key])
        else:
            return None
    return out


def palette_class_from_filters(filters: list[str]) -> str:
    """Derive the §4.3 palette class from the FILTER keyword(s) of an acquisition.

    An empty filter list means a one-shot-colour camera with no filter wheel — an
    RGB acquisition. An unrecognised filter yields ``unknown`` (never a guess).
    """
    if not filters:
        return "RGB"  # OSC/DSLR: no filter wheel, no FILTER keyword

    bands = _canonical(filters)
    if bands is None:
        return "unknown"

    narrow = bands & {"Ha", "OIII", "SII"}
    broad = bands & {"R", "G", "B"}
    has_lum = "L" in bands

    if narrow == {"SII", "Ha", "OIII"} and not broad:
        return "SHO"
    if narrow == {"Ha", "OIII"} and not broad:
        return "HOO"
    if narrow == {"Ha", "OIII"} and broad == {"R", "G", "B"}:
        return "HaOIII-RGB"
    if narrow == {"Ha"} and broad == {"R", "G", "B"}:
        return "HaRGB"
    if broad == {"R", "G", "B"}:
        return "LRGB" if has_lum else "RGB"
    if has_lum and not broad and not narrow:
        return "L-only"

    return "unknown"


# Professional galleries publish photometric band letters and emission-line names rather
# than amateur filter-wheel labels, so they need their own mapping. Broadband letters
# span UV through near-IR; all of them contribute to a broadband colour composite.
_GALLERY_BROADBAND = {
    "U", "B", "V", "R", "I", "Z", "Y", "G", "J", "H", "K", "W", "L", "M",
}

_GALLERY_NARROWBAND = {
    "HA": "Ha", "H-ALPHA": "Ha", "HALPHA": "Ha", "H_ALPHA": "Ha",
    "OIII": "OIII", "O-III": "OIII", "O3": "OIII", "[O III]": "OIII", "[OIII]": "OIII",
    "SII": "SII", "S-II": "SII", "S2": "SII", "[S II]": "SII", "[SII]": "SII",
}


def palette_class_from_gallery_bands(bands: list[str]) -> str:
    """Derive a §4.3 palette class from a gallery's published filter bands.

    Note the asymmetry against :func:`palette_class_from_filters`: an *empty* FITS FILTER
    keyword is positive evidence of a one-shot-colour camera and yields ``RGB``, whereas
    an empty gallery filter table is simply absent metadata and must yield ``unknown``.
    Guessing here would let a mismatched reference push channel ratios (§2.1).

    A broadband-dominated composite is classified ``RGB`` even when a narrowband layer is
    blended in, matching how the curated catalog labels heic0601a (B, V, H-alpha, I, Z).
    """
    if not bands:
        return "unknown"

    broad: set[str] = set()
    narrow: set[str] = set()
    for raw in bands:
        key = str(raw).strip().upper()
        if not key:
            continue
        if key in _GALLERY_NARROWBAND:
            narrow.add(_GALLERY_NARROWBAND[key])
        elif key in _GALLERY_BROADBAND:
            broad.add(key)

    # Two or more broadband filters make a colour composite; incidental narrowband
    # blending does not change the presentation palette.
    if len(broad) >= 2:
        return "RGB"
    if not broad:
        if narrow == {"SII", "Ha", "OIII"}:
            return "SHO"
        if narrow == {"Ha", "OIII"}:
            return "HOO"
    return "unknown"


# §2.3 chroma compatibility. Palette EQUALITY is too strict a gate: professional
# broadband composites routinely blend a narrowband layer — heic0601a is B, V, H-alpha,
# I, Z and is classified RGB — so an HaRGB acquisition would be denied chroma from its
# closest possible reference. Compatibility is therefore a FAMILY relation.
#
#   broadband — true-ish colour built on R/G/B-like bands; a blended narrowband layer
#               does not change the colour presentation
#   sho       — the Hubble palette (SII->R, Ha->G, OIII->B): gold/teal
#   hoo       — Ha/OIII bi-colour: red/teal
#
# SHO and HOO are deliberately in separate families: both are narrowband, but their
# colour presentations are not interchangeable.
#
# 'L-only' and 'unknown' belong to NO family and are therefore never chroma-compatible,
# not even with themselves: a monochrome image has no chroma to share, and 'unknown'
# means undetermined — §2.1 forbids letting an unverified palette push channel ratios.
_CHROMA_FAMILIES = {
    "RGB": "broadband",
    "LRGB": "broadband",
    "HaRGB": "broadband",
    "HaOIII-RGB": "broadband",
    "SHO": "sho",
    "HOO": "hoo",
}


def palette_chroma_compatible(a: str, b: str) -> bool:
    """Whether two palette classes may share chroma guidance (§2.3).

    Replaces bare equality at the two gate sites (``FingerprintStore.cone_search`` and
    ``fingerprint_distance``). Reflexive only for chroma-bearing palettes, and symmetric
    by construction.
    """
    family = _CHROMA_FAMILIES.get(a)
    return family is not None and family == _CHROMA_FAMILIES.get(b)
