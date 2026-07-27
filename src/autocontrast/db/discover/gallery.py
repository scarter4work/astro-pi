"""Djangoplicity gallery adapter — pure parsing, no network (§5.2.1).

The ESA/Hubble and ESO galleries expose no JSON API and no positional search, but their
detail pages *publish* position, field of view, dimensions, filters, and credit as text,
and their listing pages carry an inline ``var images = [...]`` JavaScript data literal.
This module turns both into dataclasses.

Two deliberate parsing strategies:

* **Metadata comes from published LABELS** ("Position (RA):", "Field of view:"), not from
  DOM paths. A CSS or layout redesign leaves the labels intact, so label matching is far
  more drift-resistant than selectors.
* **Credit comes from ``class="credit"``** because no label precedes it. That is the one
  DOM-shaped dependency here; gate G5 turns a class rename into loud rejections rather
  than silently unattributed ingest (§5.5).
"""

from __future__ import annotations

import html as _html
import math
import re
from dataclasses import dataclass
from urllib.parse import urlencode


@dataclass(frozen=True)
class GalleryConfig:
    """One Djangoplicity site. The galleries differ only in URL shape and default license."""

    key: str
    base_url: str
    search_path: str
    detail_path: str
    results_paginated: bool
    default_license: str


# Verified 2026-07-26. ESA/Hubble serves search results only under /page/N/; ESO serves
# them at the bare search path and returns nothing under /page/N/.
GALLERIES: dict[str, GalleryConfig] = {
    "esa_hubble": GalleryConfig(
        key="esa_hubble",
        base_url="https://esahubble.org",
        search_path="/images/archive/search/",
        detail_path="/images/{id}/",
        results_paginated=True,
        default_license="CC BY 4.0",
    ),
    "eso": GalleryConfig(
        key="eso",
        base_url="https://www.eso.org",
        search_path="/public/images/archive/search/",
        detail_path="/public/images/{id}/",
        results_paginated=False,
        default_license="CC BY 4.0",
    ),
}


def search_url(cfg: GalleryConfig, criteria: dict[str, str], page: int = 1) -> str:
    """Build a search URL. A search with no criteria renders empty on both sites, so
    callers must pass at least one."""
    if not criteria:
        raise ValueError("Djangoplicity renders an empty result set for a criteria-free "
                         "search; pass at least one criterion.")
    path = cfg.search_path
    if cfg.results_paginated:
        path = f"{path}page/{page}/"
    return f"{cfg.base_url}{path}?{urlencode(criteria)}"


def detail_url(cfg: GalleryConfig, entry_id: str) -> str:
    return f"{cfg.base_url}{cfg.detail_path.format(id=entry_id)}"


def text_lines(doc: str) -> list[str]:
    """Visible text as non-empty stripped lines, scripts and styles removed."""
    body = re.sub(r"(?is)<(script|style)[^>]*>.*?</\1>", " ", doc)
    text = _html.unescape(re.sub(r"(?s)<[^>]+>", "\n", body))
    return [line.strip() for line in text.split("\n") if line.strip()]


def labelled_value(lines: list[str], label: str) -> str | None:
    """The line following an exact published label, ignoring a trailing colon and case."""
    want = label.rstrip(":").strip().lower()
    for i, line in enumerate(lines):
        if line.rstrip(":").strip().lower() == want:
            return lines[i + 1] if i + 1 < len(lines) else None
    return None


def parse_ra_sexagesimal(text: str) -> float | None:
    """``'5 35 9.73'`` (hours minutes seconds) -> degrees."""
    parts = re.findall(r"\d+(?:\.\d+)?", text or "")
    if len(parts) < 3:
        return None
    hours, minutes, seconds = (float(p) for p in parts[:3])
    return (hours + minutes / 60.0 + seconds / 3600.0) * 15.0


def parse_dec_sexagesimal(text: str) -> float | None:
    """``'-5° 24\\' 50.32"'`` -> degrees.

    The sign is read from the leading character, not from the degrees value: a
    ``-0° 30'`` declination is southern, and ``float('0')`` would lose that.
    """
    raw = (text or "").strip()
    parts = re.findall(r"\d+(?:\.\d+)?", raw)
    if len(parts) < 3:
        return None
    degrees, minutes, seconds = (float(p) for p in parts[:3])
    value = degrees + minutes / 60.0 + seconds / 3600.0
    negative = raw.startswith("-") or raw.startswith("−")
    return -value if negative else value


# Published fields of view use arcminutes almost always, arcseconds for close-ups.
# An unrecognised unit must NOT default to arcminutes: the resulting pixel scale would
# be wrong by a factor of 60, corrupting band-limiting (§4.4).
_FOV_UNITS_TO_ARCMIN = {
    "arcminute": 1.0, "arcminutes": 1.0, "arcmin": 1.0,
    "arcsecond": 1.0 / 60.0, "arcseconds": 1.0 / 60.0, "arcsec": 1.0 / 60.0,
    "degree": 60.0, "degrees": 60.0, "deg": 60.0,
}


def parse_fov_arcmin(text: str) -> tuple[float, float] | None:
    """``'30.03 x 30.03 arcminutes'`` -> ``(30.03, 30.03)`` in arcminutes."""
    match = re.match(r"\s*([\d.]+)\s*[x×]\s*([\d.]+)\s*([A-Za-z]+)", text or "")
    if match is None:
        return None
    unit = match.group(3).lower()
    factor = _FOV_UNITS_TO_ARCMIN.get(unit)
    if factor is None:
        return None
    return float(match.group(1)) * factor, float(match.group(2)) * factor


def fov_radius_arcmin(width_arcmin: float, height_arcmin: float) -> float:
    """Half-diagonal, matching ``WcsResult.fov_radius_arcmin``'s corner-radius convention."""
    return math.hypot(width_arcmin, height_arcmin) / 2.0


_MONTHS = {
    "january": 1, "february": 2, "march": 3, "april": 4, "may": 5, "june": 6,
    "july": 7, "august": 8, "september": 9, "october": 10, "november": 11, "december": 12,
}


def parse_release_date(text: str) -> str | None:
    """``'11 January 2006, 16:00'`` -> ``'2006-01-11T16:00:00Z'``.

    Month names are mapped explicitly rather than via ``%B``, which is locale-dependent
    and would break on a non-English system.
    """
    match = re.match(
        r"\s*(\d{1,2})\s+([A-Za-z]+)\s+(\d{4})(?:,\s*(\d{1,2}):(\d{2}))?", text or ""
    )
    if match is None:
        return None
    month = _MONTHS.get(match.group(2).lower())
    if month is None:
        return None
    day, year = int(match.group(1)), int(match.group(3))
    hour = int(match.group(4)) if match.group(4) else 0
    minute = int(match.group(5)) if match.group(5) else 0
    return f"{year:04d}-{month:02d}-{day:02d}T{hour:02d}:{minute:02d}:00Z"
