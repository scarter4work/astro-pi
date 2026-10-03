import re

_HEADER_MAP = {
    "light": "light", "light frame": "light",
    "dark": "dark", "dark frame": "dark",
    "flat": "flat", "flat field": "flat", "flat frame": "flat",
    "bias": "bias", "bias frame": "bias", "zero": "bias",
}

_DERIVED = re.compile(
    r"^(pp_light|r_pp_light|ASIVideoStack|AS_P|Autosave)", re.IGNORECASE)
_FLAT = re.compile(r"^flat|master_flat", re.IGNORECASE)


def classify(filename: str, header: dict) -> str:
    """
    Classify a FITS file as light, dark, flat, bias, derived, or unknown.

    Args:
        filename: The filename (may contain spaces)
        header: FITS header dict from read_header(), may be empty {}

    Returns:
        One of: "light", "dark", "flat", "bias", "derived", "unknown"
    """
    # IMAGETYP header wins when present
    raw = header.get("IMAGETYP")
    if isinstance(raw, str):
        mapped = _HEADER_MAP.get(raw.strip().lower())
        if mapped:
            return mapped

    # Filename conventions are fallback (case-insensitive)
    filename_lower = filename.lower()
    if _DERIVED.match(filename):
        return "derived"
    if filename_lower.startswith("light_"):
        return "light"
    if filename_lower.startswith("dark"):
        return "dark"
    if filename_lower.startswith("bias"):
        return "bias"
    if _FLAT.match(filename) or "master_flat" in filename_lower:
        return "flat"
    return "unknown"
