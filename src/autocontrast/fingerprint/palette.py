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
