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

from autocontrast.fingerprint.palette import palette_class_from_gallery_bands


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


@dataclass
class GalleryEntry:
    """One gallery render's published metadata. Position fields are ``None`` when the
    gallery does not publish them — an expected, first-class outcome (§5.2)."""

    id: str
    gallery: str
    detail_url: str
    image_url: str | None
    ra_deg: float | None
    dec_deg: float | None
    fov_w_arcmin: float | None
    fov_h_arcmin: float | None
    fov_radius_arcmin: float | None
    width_px: int | None
    height_px: int | None
    pixel_scale_arcsec: float | None
    object_name: str | None
    category: str | None
    entry_type: str | None
    palette_class: str
    license: str | None
    attribution: str | None
    published_utc: str | None
    parsed_ok: bool

    @property
    def has_position(self) -> bool:
        return (self.ra_deg is not None and self.dec_deg is not None
                and self.fov_radius_arcmin is not None)


def parse_credit(doc: str) -> str | None:
    """The full credit line from ``class="credit"``.

    The block contains nested anchors, so tags are stripped and whitespace collapsed —
    taking only the first text node would truncate "NASA, ESA, M. Robberto ..." to "NASA".
    The site states that crediting with the full line is mandatory.
    """
    match = re.search(r'class="credit"[^>]*>(.*?)</div>', doc, re.S)
    if match is None:
        return None
    text = _html.unescape(re.sub(r"(?s)<[^>]+>", " ", match.group(1)))
    text = re.sub(r"\s+", " ", text).strip()
    # Tag stripping leaves gaps around punctuation: "NASA , ESA , M. Robberto ( STScI / ESA )".
    text = re.sub(r"\s+([,.;:])", r"\1", text)
    text = re.sub(r"\(\s+", "(", text)
    text = re.sub(r"\s+\)", ")", text)
    text = re.sub(r"\s*/\s*", "/", text)
    return text or None


# No per-image machine-readable license exists on either site: the copyright block is
# site boilerplate and no CC BY string appears. The only signal is the credit itself,
# and ESA/Hubble does host third-party copyrighted images (opo0205c is AAO's).
_COPYRIGHT_MARKERS = re.compile(r"(?i)copyright|©|\(c\)\s|all rights reserved")


def asserts_copyright(credit: str | None) -> bool:
    """True when a credit line claims copyright, so the gallery default must not apply."""
    return bool(credit) and _COPYRIGHT_MARKERS.search(credit) is not None


def parse_filter_bands(doc: str) -> list[str]:
    """Filter names from the published 'Colours & filters' table, in table order.

    The Band cell nests "Optical" (or similar) above the filter name in a
    ``class="band_instrument"`` span, e.g. ``Optical<br/>B`` -> "Optical B"; the filter
    name is the trailing token ("Optical H-alpha" -> "H-alpha"). The live table also
    emits a trailing malformed ``<tr>`` with an empty Band cell (a template artifact, not
    a filter row) — skipped by requiring the Band cell itself be non-empty, not just the
    row as a whole, since its Telescope cell is populated and would otherwise slip through.
    """
    section = re.search(r"(?is)Colours?\s*&(?:amp;)?\s*[Ff]ilters?(.*?)</table>", doc)
    if section is None:
        return []
    bands: list[str] = []
    for row in re.findall(r"(?is)<tr[^>]*>(.*?)</tr>", section.group(1)):
        cells = [
            re.sub(r"\s+", " ", _html.unescape(re.sub(r"(?s)<[^>]+>", " ", cell))).strip()
            for cell in re.findall(r"(?is)<t[dh][^>]*>(.*?)</t[dh]>", row)
        ]
        if not cells or not cells[0] or cells[0].lower().startswith("band"):
            continue
        bands.append(cells[0].split()[-1])
    return bands


def parse_size_px(lines: list[str]) -> tuple[int, int] | None:
    """``'18000 x 18000 px'`` -> ``(18000, 18000)``. Cross-checks the listing dimensions."""
    value = labelled_value(lines, "Size")
    if value is None:
        return None
    match = re.match(r"\s*(\d+)\s*[x×]\s*(\d+)", value)
    return (int(match.group(1)), int(match.group(2))) if match else None


def _image_url(doc: str) -> str | None:
    """The CDN 'large' JPEG, read from the page rather than constructed, so a CDN or path
    change surfaces as a missing URL instead of a 404 at download time."""
    urls = re.findall(
        r"https?://cdn\.[^\"' ]*?/images/large/[^\"' ]+\.(?:jpg|jpeg|png)", doc
    )
    return urls[0] if urls else None


def parse_detail(
    doc: str,
    *,
    entry_id: str,
    gallery: str,
    detail_url: str,
    width_px: int | None = None,
    height_px: int | None = None,
) -> GalleryEntry:
    """Parse a gallery detail page into a :class:`GalleryEntry`.

    Never raises. ``parsed_ok`` distinguishes "the page parsed but publishes no position"
    (normal — starless treatments, artwork, older releases) from "parsing broke" (a site
    redesign or an error page), which is what lets the crawler abort loudly rather than
    quietly index nothing.
    """
    lines = text_lines(doc)
    cfg = GALLERIES.get(gallery)

    # A real detail page always carries at least an Id or a Name label. Neither means
    # this is not a detail page at all.
    parsed_ok = labelled_value(lines, "Id") is not None or labelled_value(lines, "Name") is not None

    ra_text = labelled_value(lines, "Position (RA)")
    dec_text = labelled_value(lines, "Position (Dec)")
    fov_text = labelled_value(lines, "Field of view")

    ra_deg = parse_ra_sexagesimal(ra_text) if ra_text else None
    dec_deg = parse_dec_sexagesimal(dec_text) if dec_text else None
    fov = parse_fov_arcmin(fov_text) if fov_text else None
    fov_w, fov_h = fov if fov else (None, None)
    radius = fov_radius_arcmin(fov_w, fov_h) if fov else None

    if width_px is None or height_px is None:
        size = parse_size_px(lines)
        if size is not None:
            width_px, height_px = size

    # §2.2: scale must be angular. Derived from published metadata so the cone can be
    # filtered before any download.
    pixel_scale = None
    if fov_w is not None and width_px:
        pixel_scale = fov_w * 60.0 / float(width_px)

    credit = parse_credit(doc)
    # A copyright-asserting credit means the gallery default does NOT apply. Leave the
    # license unestablished rather than attaching a false one (§5.5).
    license_text = None
    if cfg is not None and not asserts_copyright(credit):
        license_text = cfg.default_license

    release = labelled_value(lines, "Release date")

    return GalleryEntry(
        id=entry_id,
        gallery=gallery,
        detail_url=detail_url,
        image_url=_image_url(doc),
        ra_deg=ra_deg,
        dec_deg=dec_deg,
        fov_w_arcmin=fov_w,
        fov_h_arcmin=fov_h,
        fov_radius_arcmin=radius,
        width_px=width_px,
        height_px=height_px,
        pixel_scale_arcsec=pixel_scale,
        object_name=labelled_value(lines, "Name"),
        category=labelled_value(lines, "Category"),
        entry_type=labelled_value(lines, "Type"),
        palette_class=palette_class_from_gallery_bands(parse_filter_bands(doc)),
        license=license_text,
        attribution=credit,
        published_utc=parse_release_date(release) if release else None,
        parsed_ok=parsed_ok,
    )


@dataclass
class ListingItem:
    """One search result. ``width_px``/``height_px`` come free from the listing, which is
    what lets pixel scale be computed without downloading the image."""

    id: str
    title: str | None
    width_px: int | None
    height_px: int | None
    detail_path: str


def parse_listing(doc: str) -> list[ListingItem]:
    """Parse the inline ``var images = [ {...}, ... ];`` block.

    This is a JavaScript literal, not JSON — unquoted keys and single-quoted strings — so
    fields are extracted individually rather than via ``json.loads``. Preferred over
    scraping rendered markup: it is machine-oriented and survives visual redesigns.
    """
    block = re.search(r"var\s+images\s*=\s*\[(.*?)\n\s*\]\s*;", doc, re.S)
    if block is None:
        return []

    items: list[ListingItem] = []
    for record in re.findall(r"\{(.*?)\}", block.group(1), re.S):

        def field(key: str) -> str | None:
            match = re.search(rf"\b{key}\s*:\s*'([^']*)'", record)
            return _html.unescape(match.group(1)) if match else None

        def number(key: str) -> int | None:
            match = re.search(rf"\b{key}\s*:\s*(\d+)", record)
            return int(match.group(1)) if match else None

        entry_id, path = field("id"), field("url")
        if not entry_id or not path:
            continue  # nav entries and malformed records carry neither
        items.append(ListingItem(
            id=entry_id, title=field("title"),
            width_px=number("width"), height_px=number("height"),
            detail_path=path,
        ))
    return items


def parse_result_total(doc: str) -> int | None:
    """The ``Showing 1 to 35 of 35`` total, used to drive pagination."""
    text = re.sub(r"\s+", " ", _html.unescape(re.sub(r"(?s)<[^>]+>", " ", doc)))
    match = re.search(r"Showing\s+[\d,]+\s+to\s+[\d,]+\s+of\s+([\d,]+)", text)
    return int(match.group(1).replace(",", "")) if match else None
