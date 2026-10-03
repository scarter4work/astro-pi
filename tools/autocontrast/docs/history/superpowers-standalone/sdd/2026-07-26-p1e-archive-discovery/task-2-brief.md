### Task 2: Gallery configs and coordinate parsers

Pure functions, no fixtures, no network. These are the units the detail parser composes.

**Files:**
- Create: `src/autocontrast/db/discover/__init__.py`, `src/autocontrast/db/discover/gallery.py`
- Test: `tests/test_gallery_parse.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `GalleryConfig` (frozen dataclass: `key`, `base_url`, `search_path`, `detail_path`,
  `results_paginated: bool`, `default_license: str`); `GALLERIES: dict[str, GalleryConfig]`;
  `search_url(cfg, criteria: dict, page: int = 1) -> str`;
  `text_lines(doc: str) -> list[str]`; `labelled_value(lines, label) -> str | None`;
  `parse_ra_sexagesimal(s) -> float | None`; `parse_dec_sexagesimal(s) -> float | None`;
  `parse_fov_arcmin(s) -> tuple[float, float] | None`; `fov_radius_arcmin(w, h) -> float`;
  `parse_release_date(s) -> str | None`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_gallery_parse.py
"""Pure parsers for Djangoplicity gallery pages (§5.2.1).

Values are measured from the real pages — see the plan's Verified Ground Truth table.
"""

from __future__ import annotations

import pytest

from autocontrast.db.discover.gallery import (
    GALLERIES,
    fov_radius_arcmin,
    labelled_value,
    parse_dec_sexagesimal,
    parse_fov_arcmin,
    parse_ra_sexagesimal,
    parse_release_date,
    search_url,
    text_lines,
)


def test_both_galleries_configured():
    assert set(GALLERIES) == {"esa_hubble", "eso"}
    assert GALLERIES["esa_hubble"].results_paginated is True
    assert GALLERIES["eso"].results_paginated is False


def test_search_url_paginates_hubble_but_not_eso():
    """Confirmed live: Hubble serves results at /page/N/, ESO only at the bare path."""
    hub = search_url(GALLERIES["esa_hubble"], {"subject_name": "Orion Nebula"}, page=2)
    assert "/images/archive/search/page/2/" in hub
    assert "subject_name=Orion+Nebula" in hub

    eso = search_url(GALLERIES["eso"], {"subject_name": "Orion Nebula"}, page=2)
    assert "/page/" not in eso
    assert eso.endswith("subject_name=Orion+Nebula")


def test_text_lines_drops_scripts_and_unescapes_entities():
    doc = "<div>Name:</div><script>var x='Nope';</script><p>M&nbsp;42 &amp; friends</p>"
    lines = text_lines(doc)
    assert "Nope" not in " ".join(lines)
    assert any("friends" in line for line in lines)


def test_labelled_value_returns_following_line_ignoring_colon_and_case():
    lines = ["Position (RA):", "5 35 9.73", "Position (Dec):", "-5° 24' 50.32\""]
    assert labelled_value(lines, "Position (RA)") == "5 35 9.73"
    assert labelled_value(lines, "position (dec)") == "-5° 24' 50.32\""
    assert labelled_value(lines, "Field of view") is None


def test_labelled_value_at_end_of_document_returns_none():
    assert labelled_value(["Field of view:"], "Field of view") is None


@pytest.mark.parametrize(
    "text, expected",
    [("5 35 9.73", 83.79054), ("5 35 17.32", 83.82217), ("0 0 0", 0.0), ("12 0 0", 180.0)],
)
def test_parse_ra_sexagesimal(text, expected):
    assert parse_ra_sexagesimal(text) == pytest.approx(expected, abs=1e-4)


@pytest.mark.parametrize(
    "text, expected",
    [
        ("-5° 24' 50.32\"", -5.41398),   # heic0601a
        ("-5° 23' 27.55\"", -5.39099),   # eso1103a
        ("+22° 0' 52.20\"", 22.01450),
        ("22° 0' 52.20\"", 22.01450),
    ],
)
def test_parse_dec_sexagesimal(text, expected):
    assert parse_dec_sexagesimal(text) == pytest.approx(expected, abs=1e-4)


def test_parse_dec_keeps_the_sign_when_degrees_are_zero():
    """-0° 30' 0" is a southern position; losing the sign flips the hemisphere."""
    assert parse_dec_sexagesimal("-0° 30' 0\"") == pytest.approx(-0.5, abs=1e-6)


def test_unparseable_coordinates_return_none_rather_than_guessing():
    assert parse_ra_sexagesimal("unknown") is None
    assert parse_dec_sexagesimal("") is None


@pytest.mark.parametrize(
    "text, expected",
    [
        ("30.03 x 30.03 arcminutes", (30.03, 30.03)),
        ("35.49 x 34.10 arcminutes", (35.49, 34.10)),
        ("2.5 × 2.5 arcminutes", (2.5, 2.5)),          # unicode multiplication sign
        ("120 x 90 arcseconds", (2.0, 1.5)),           # arcsec -> arcmin
        ("1.5 x 1.0 degrees", (90.0, 60.0)),           # deg -> arcmin
    ],
)
def test_parse_fov_arcmin_normalizes_units(text, expected):
    got = parse_fov_arcmin(text)
    assert got == pytest.approx(expected, rel=1e-6)


def test_parse_fov_rejects_unknown_units_rather_than_assuming_arcmin():
    """A silent unit assumption corrupts pixel scale, and hence band-limiting (§4.4)."""
    assert parse_fov_arcmin("30 x 30 parsecs") is None
    assert parse_fov_arcmin("not a field of view") is None


def test_fov_radius_is_the_half_diagonal():
    assert fov_radius_arcmin(30.03, 30.03) == pytest.approx(21.23, abs=0.01)
    assert fov_radius_arcmin(35.49, 34.10) == pytest.approx(24.61, abs=0.01)


def test_parse_release_date_is_locale_independent():
    """%B depends on locale, so month names are mapped explicitly."""
    assert parse_release_date("11 January 2006, 16:00") == "2006-01-11T16:00:00Z"
    assert parse_release_date("19 January 2011, 12:00") == "2011-01-19T12:00:00Z"
    assert parse_release_date("3 December 1999, 09:30") == "1999-12-03T09:30:00Z"
    assert parse_release_date("garbage") is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_gallery_parse.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.db.discover'`

- [ ] **Step 3: Write the implementation**

```python
# src/autocontrast/db/discover/__init__.py
"""Archive discovery (§5.2.1): local positional index over professional gallery renders."""

from __future__ import annotations
```

```python
# src/autocontrast/db/discover/gallery.py
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_gallery_parse.py -v`
Expected: PASS, 20 tests.

- [ ] **Step 5: Lint and full suite**

Run: `.venv/bin/python -m ruff check <the files this task created or modified> && .venv/bin/python -m pytest -q`
Expected: no findings in your files; **164 passed**. (The repo has 25 pre-existing ruff
findings in untouched files — ignore them.)

- [ ] **Step 6: Commit**

```bash
git add src/autocontrast/db/discover tests/test_gallery_parse.py
git commit -m "discover/gallery.py: gallery configs + coordinate parsers

Label-based extraction rather than DOM selectors, so a CSS redesign doesn't break
parsing. Unit handling is strict: an unrecognised field-of-view unit returns None
instead of assuming arcminutes, because a 60x scale error corrupts band-limiting
(§4.4). Declination sign is read from the leading character so -0d 30' stays
southern. Release dates map month names explicitly — %B is locale-dependent.

Verified against the live pages: 5 35 9.73 -> 83.79054, -5d 24' 50.32\" -> -5.41398."
```

---

