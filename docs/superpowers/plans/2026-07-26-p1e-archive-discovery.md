# P1e Archive Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the Phase 1 remainder — on a fingerprint-DB miss, discover and ingest a professional gallery render for the solved position, unattended.

**Architecture:** Crawl the ESA/Hubble and ESO galleries once into a local SQLite *position* index, then cone-search that index locally on a miss, fetch only the winning candidate, and push it through the existing `ingest_reference_auto()` behind a fail-closed verification gate. Parsing is pure functions over HTML strings (fixture-tested, no network); network lives behind an injected `Fetcher` protocol.

**Tech Stack:** Python 3.11+, stdlib only for HTTP and HTML (`urllib.request`, `re`, `html`), SQLite via `sqlite3`, numpy for spherical math, pytest.

## Global Constraints

- **No new third-party dependencies.** `pyproject.toml` states: *"Keep this list minimal — every dependency here must run in the offline eval harness (§10)."* Use `urllib.request` and `re`/`html`, never `requests`, `httpx`, `beautifulsoup4`, or `lxml`.
- **Line length 100** (`[tool.ruff] line-length = 100`).
- **`from __future__ import annotations`** at the top of every new module, matching existing files.
- **Never ingest a linear science FITS as `professional_render`** (§2.1). Only gallery renders.
- **Never guess a palette class.** Unrecognized or absent filter data yields `'unknown'` (§4.3).
- **Never force-parse an AVM tag.** pyavm rejecting a tag is protective; `acquire_wcs` already falls through correctly.
- **Every degraded path logs and surfaces** (§12). Unusable candidates produce reason strings, never silent skips and never exceptions.
- **Politeness, fixed values:** 1 request/second with jitter; User-Agent `AutoContrast/0.1 (+https://github.com/scarter4work/autocontrast; scarter4work@yahoo.com)`; exponential backoff on 5xx; honor `429 Retry-After`; per-run request cap **8000**; image download cap **256 MB**; sync safety window **7 days**; crawl drift-abort threshold **>50%** of detail pages on one listing page failing to parse.
- **Tests must not touch the network** except one test marked `live`, deselected by default.

---

## File Structure

| Path | Responsibility |
|---|---|
| `src/autocontrast/db/skymath.py` | **new** — spherical separation + cone overlap, shared by store and index |
| `src/autocontrast/db/store.py` | **modify** — import `separation_arcmin` from `skymath` instead of defining `_separation_arcmin` |
| `src/autocontrast/db/discover/__init__.py` | **new** — public re-exports |
| `src/autocontrast/db/discover/gallery.py` | **new** — gallery configs + all pure HTML parsers |
| `src/autocontrast/db/discover/index.py` | **new** — `GalleryIndex`: SQLite position index + local cone search + sync state |
| `src/autocontrast/db/discover/crawl.py` | **new** — `Fetcher` protocol, `PoliteFetcher`, crawl + incremental sync |
| `src/autocontrast/db/discover/discover.py` | **new** — ranking, verification gate, miss-path orchestration |
| `src/autocontrast/fingerprint/palette.py` | **modify** — add `palette_class_from_gallery_bands()` |
| `tools/capture_gallery_fixtures.sh` | **new** — one-time fixture capture (documented URLs) |
| `tests/fixtures/gallery/*.html` | **new** — six committed real pages |
| `tests/test_skymath.py`, `test_gallery_parse.py`, `test_gallery_listing.py`, `test_palette_gallery.py`, `test_gallery_index.py`, `test_crawl.py`, `test_discover_rank.py`, `test_discover_gate.py`, `test_discover_e2e.py`, `test_gallery_live.py` | **new** — tests |

---

## Verified Ground Truth

Every value below was measured from the live pages on 2026-07-26 and cross-checks against
`data/seed_catalog.json`. Use these as test expectations.

| | `heic0601a` | `eso1103a` |
|---|---|---|
| `Position (RA)` | `5 35 9.73` → **83.79054°** | `5 35 17.32` → **83.82217°** |
| `Position (Dec)` | `-5° 24' 50.32"` → **−5.41398°** | `-5° 23' 27.55"` → **−5.39099°** |
| `Field of view` | `30.03 x 30.03 arcminutes` | `35.49 x 34.10 arcminutes` |
| `fov_radius_arcmin` (√(w²+h²)/2) | **21.23** | **24.61** |
| `Size` / listing dims | **18000 × 18000** | **8948 × 8597** |
| pixel scale (fov_w·60/width) | **0.1001″/px** | **0.238″/px** |
| `Type` | `Observation` | `Observation` |
| `Name` | `Messier 42` | `M 42` |
| `Release date` | `11 January 2006, 16:00` | `19 January 2011, 12:00` |
| filter bands | Optical B 435, V 555, H-alpha 658, Infrared I 775, Z 850 | Ultraviolet U 340, Optical B 451, V 539, R 651, H-alpha 658 |
| palette | **RGB** | **RGB** |
| image URL | `https://cdn.esahubble.org/archives/images/large/heic0601a.jpg` | `https://cdn.eso.org/images/large/eso1103a.jpg` |

Negative fixtures: `opo0205c` (`Type: Photographic`, **no** position, **no** FoV, credit
`Copyright © Anglo-Australian Observatory. Photograph by David Malin`) and `heic0211i`
(`Type: Artwork`, no position, no FoV).

Listing counts: Hubble `subject_name=Orion Nebula&minimum_size=2` → **35** items; ESO
`subject_name=Orion Nebula` → **26** items.

**Access quirks confirmed:** ESA/Hubble serves results at `/images/archive/search/page/N/?…`;
ESO serves them at `/public/images/archive/search/?…` (the `/page/N/` form returns no results
on ESO). Results are an inline `var images = [ {...} ];` JavaScript literal carrying `id`,
`title`, `width`, `height`, `url`. A search with no criteria renders empty on both sites.

---

### Task 1: Extract shared spherical math

Pure refactor, no behavior change. `index.py` needs the same separation math `store.py` has
privately; duplicating a haversine in two files invites silent divergence.

**Files:**
- Create: `src/autocontrast/db/skymath.py`
- Modify: `src/autocontrast/db/store.py:23-27` (delete `_separation_arcmin`, import instead)
- Test: `tests/test_skymath.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `separation_arcmin(ra1, dec1, ra2, dec2) -> float`;
  `cones_overlap(sep_arcmin, radius_a_arcmin, radius_b_arcmin) -> bool`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_skymath.py
"""Shared spherical helpers (§5.1 cone-search geometry)."""

from __future__ import annotations

import pytest

from autocontrast.db.skymath import cones_overlap, separation_arcmin


def test_zero_separation_for_identical_positions():
    assert separation_arcmin(83.82, -5.39, 83.82, -5.39) == pytest.approx(0.0, abs=1e-9)


def test_one_degree_of_declination_is_sixty_arcmin():
    assert separation_arcmin(10.0, 0.0, 10.0, 1.0) == pytest.approx(60.0, rel=1e-9)


def test_ra_separation_shrinks_with_cos_dec():
    """One degree of RA subtends less angle away from the equator — the cos(dec)
    factor is exactly what a naive coordinate difference gets wrong."""
    at_equator = separation_arcmin(0.0, 0.0, 1.0, 0.0)
    at_sixty = separation_arcmin(0.0, 60.0, 1.0, 60.0)
    assert at_equator == pytest.approx(60.0, rel=1e-6)
    assert at_sixty == pytest.approx(30.0, rel=1e-3)


def test_ra_wrap_across_zero_is_short_way_round():
    """359.5 -> 0.5 is one degree apart, not 359."""
    assert separation_arcmin(359.5, 0.0, 0.5, 0.0) == pytest.approx(60.0, rel=1e-6)


def test_matches_the_two_real_m42_references():
    """heic0601a vs eso1103a, from data/seed_catalog.json."""
    sep = separation_arcmin(83.7905, -5.4140, 83.82217, -5.39099)
    assert sep == pytest.approx(2.34, abs=0.05)


def test_cones_overlap_at_and_beyond_the_boundary():
    assert cones_overlap(10.0, 4.0, 6.0) is True     # exactly touching
    assert cones_overlap(10.001, 4.0, 6.0) is False   # just past
    assert cones_overlap(0.0, 1.0, 1.0) is True
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_skymath.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.db.skymath'`

- [ ] **Step 3: Write the implementation**

```python
# src/autocontrast/db/skymath.py
"""Spherical geometry shared by the fingerprint store and the gallery index.

Both do §5.1 cone-overlap tests. The haversine lived privately in ``store.py``;
it is here so the two callers cannot silently diverge.
"""

from __future__ import annotations

import numpy as np


def separation_arcmin(ra1: float, dec1: float, ra2: float, dec2: float) -> float:
    """Great-circle separation in arcminutes (haversine).

    Handles RA wrap and the cos(dec) foreshortening for free — a plain coordinate
    difference gets both wrong.
    """
    r1, d1, r2, d2 = np.radians([ra1, dec1, ra2, dec2])
    a = np.sin((d2 - d1) / 2) ** 2 + np.cos(d1) * np.cos(d2) * np.sin((r2 - r1) / 2) ** 2
    return float(np.degrees(2 * np.arcsin(np.sqrt(a))) * 60.0)


def cones_overlap(sep_arcmin: float, radius_a_arcmin: float, radius_b_arcmin: float) -> bool:
    """Two cones overlap when their center separation is at most the sum of radii.

    Boundary-inclusive: exactly touching counts as overlapping, matching
    ``FingerprintStore.cone_search``.
    """
    return sep_arcmin <= radius_a_arcmin + radius_b_arcmin
```

- [ ] **Step 4: Rewire `store.py` to the shared helper**

Delete lines 23–27 of `src/autocontrast/db/store.py` (the `_separation_arcmin` definition),
add the import alongside the existing `from .records import ...`:

```python
from .records import SOURCE_TYPES, ReferenceRecord
from .skymath import separation_arcmin
```

Then update the single call site inside `cone_search` (was line 125):

```python
            sep = separation_arcmin(ra_deg, dec_deg, row["ra_deg"], row["dec_deg"])
```

- [ ] **Step 5: Run the full suite — the refactor must not change any existing behavior**

Run: `.venv/bin/python -m pytest -q`
Expected: PASS, **144 passed** (138 existing + 6 new). If any pre-existing test fails, the
refactor is wrong — revert and redo, do not adjust the old tests.

- [ ] **Step 6: Lint**

Run: `.venv/bin/python -m ruff check src tests`
Expected: no findings.

- [ ] **Step 7: Commit**

```bash
git add src/autocontrast/db/skymath.py src/autocontrast/db/store.py tests/test_skymath.py
git commit -m "Extract db/skymath.py: shared cone-overlap geometry

index.py needs the same haversine store.py had privately. Two copies of a
spherical-distance formula is a silent-divergence risk, so it now has one home.
Pure refactor: no behavior change, existing tests untouched."
```

---

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

Run: `.venv/bin/python -m ruff check src tests && .venv/bin/python -m pytest -q`
Expected: no lint findings; **164 passed**.

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

### Task 3: Capture fixtures and parse detail pages

**Files:**
- Create: `tools/capture_gallery_fixtures.sh`, `tests/fixtures/gallery/` (6 files)
- Modify: `src/autocontrast/db/discover/gallery.py` (add `GalleryEntry`, `parse_detail`, credit and filter helpers)
- Test: `tests/test_gallery_detail.py`

**Interfaces:**
- Consumes: Task 2's parsers and `GalleryConfig`/`GALLERIES`.
- Produces: `GalleryEntry` dataclass (fields listed in the code below);
  `parse_detail(doc, *, entry_id, gallery, detail_url, width_px=None, height_px=None) -> GalleryEntry`;
  `parse_credit(doc) -> str | None`; `asserts_copyright(credit) -> bool`;
  `parse_filter_bands(doc) -> list[str]`; `parse_size_px(lines) -> tuple[int, int] | None`.

- [ ] **Step 1: Write the fixture capture script**

```bash
# tools/capture_gallery_fixtures.sh
#!/usr/bin/env bash
# Capture the gallery HTML fixtures used by the discovery parser tests.
#
# Fixtures are COMMITTED, so the test suite never touches the network. Re-run this only
# to refresh them deliberately (e.g. after tests/test_gallery_live.py reports drift).
# One request per second, as everywhere else in this subsystem.
set -euo pipefail

DEST="$(dirname "$0")/../tests/fixtures/gallery"
UA='AutoContrast/0.1 (+https://github.com/scarter4work/autocontrast; scarter4work@yahoo.com)'
mkdir -p "$DEST"

fetch () {  # $1 = destination filename, $2 = URL
  echo "  $1"
  curl -sSL -A "$UA" --max-time 60 "$2" -o "$DEST/$1"
  sleep 1
}

echo "Capturing gallery fixtures into $DEST"
fetch hubble_listing_orion.html \
  'https://esahubble.org/images/archive/search/page/1/?subject_name=Orion+Nebula&minimum_size=2'
fetch eso_listing_orion.html \
  'https://www.eso.org/public/images/archive/search/?subject_name=Orion+Nebula'
fetch hubble_detail_heic0601a.html   'https://esahubble.org/images/heic0601a/'
fetch eso_detail_eso1103a.html       'https://www.eso.org/public/images/eso1103a/'
fetch hubble_detail_opo0205c.html    'https://esahubble.org/images/opo0205c/'
fetch hubble_detail_heic0211i.html   'https://esahubble.org/images/heic0211i/'
echo "Done. Review the diff before committing — these are test ground truth."
```

Run it, then confirm six files exist:

```bash
chmod +x tools/capture_gallery_fixtures.sh
./tools/capture_gallery_fixtures.sh
ls -l tests/fixtures/gallery/
```

Expected: six HTML files, each 40–90 KB.

- [ ] **Step 2: Write the failing test**

```python
# tests/test_gallery_detail.py
"""Detail-page parsing against committed real pages (§5.2.1).

Every expectation here is measured ground truth from data/seed_catalog.json or from the
pages themselves. Fixture choice is deliberate: alongside two clean observations there is
a render with no published position (opo0205c) and an artist's impression (heic0211i).
Commit 12e7041 shipped two bugs past 138 green tests because every fixture was a 2D mono
FITS — the gap was fixture diversity, not coverage. Hence the awkward cases are here from
the start.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from autocontrast.db.discover.gallery import (
    GALLERIES,
    asserts_copyright,
    parse_credit,
    parse_detail,
    parse_filter_bands,
    parse_size_px,
    text_lines,
)

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


def load(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8", errors="replace")


@pytest.fixture
def heic0601a():
    return parse_detail(
        load("hubble_detail_heic0601a.html"),
        entry_id="heic0601a", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/heic0601a/",
        width_px=18000, height_px=18000,
    )


@pytest.fixture
def eso1103a():
    return parse_detail(
        load("eso_detail_eso1103a.html"),
        entry_id="eso1103a", gallery="eso",
        detail_url="https://www.eso.org/public/images/eso1103a/",
        width_px=8948, height_px=8597,
    )


def test_hubble_position_matches_the_curated_catalog(heic0601a):
    """seed_catalog.json declares 83.7905 / -5.4140 from the embedded AVM tag; the
    published text must agree, since that is what makes pre-download filtering sound."""
    assert heic0601a.ra_deg == pytest.approx(83.7905, abs=1e-3)
    assert heic0601a.dec_deg == pytest.approx(-5.4140, abs=1e-3)


def test_eso_position_matches_the_curated_catalog(eso1103a):
    assert eso1103a.ra_deg == pytest.approx(83.82217, abs=1e-4)
    assert eso1103a.dec_deg == pytest.approx(-5.39099, abs=1e-4)


def test_footprint_radii_match_the_curated_catalog(heic0601a, eso1103a):
    assert heic0601a.fov_radius_arcmin == pytest.approx(21.23, abs=0.02)
    assert eso1103a.fov_radius_arcmin == pytest.approx(24.61, abs=0.02)


def test_pixel_scale_is_derivable_from_published_metadata(heic0601a, eso1103a):
    """fov_w * 60 / width_px. Catalog says 0.1001 and 0.238 arcsec/px. This is what lets
    gate G4 be satisfied without downloading a 324-megapixel JPEG."""
    assert heic0601a.pixel_scale_arcsec == pytest.approx(0.1001, abs=1e-4)
    assert eso1103a.pixel_scale_arcsec == pytest.approx(0.238, abs=1e-3)


def test_image_urls_point_at_the_cdn(heic0601a, eso1103a):
    assert heic0601a.image_url == \
        "https://cdn.esahubble.org/archives/images/large/heic0601a.jpg"
    assert eso1103a.image_url == "https://cdn.eso.org/images/large/eso1103a.jpg"


def test_names_types_and_release_dates(heic0601a, eso1103a):
    assert heic0601a.object_name == "Messier 42"
    assert eso1103a.object_name == "M 42"          # the naming swamp §5.1 warns about
    assert heic0601a.entry_type == "Observation"
    assert heic0601a.published_utc == "2006-01-11T16:00:00Z"
    assert eso1103a.published_utc == "2011-01-19T12:00:00Z"


def test_palette_is_derived_from_published_filters(heic0601a, eso1103a):
    """Both are broadband-dominated composites; the catalog declares RGB for both."""
    assert heic0601a.palette_class == "RGB"
    assert eso1103a.palette_class == "RGB"


def test_filter_bands_are_extracted(heic0601a):
    bands = parse_filter_bands(load("hubble_detail_heic0601a.html"))
    assert bands == ["B", "V", "H-alpha", "I", "Z"]


def test_size_is_published_as_a_cross_check_on_listing_dimensions():
    lines = text_lines(load("hubble_detail_heic0601a.html"))
    assert parse_size_px(lines) == (18000, 18000)


def test_attribution_is_the_full_credit_line(heic0601a):
    """The site states crediting with the FULL credit line is mandatory, so the whole
    string is captured, not the first fragment before a nested tag."""
    assert "NASA" in heic0601a.attribution
    assert "ESA" in heic0601a.attribution
    assert "Robberto" in heic0601a.attribution
    assert "Orion Treasury Project Team" in heic0601a.attribution


def test_license_defaults_to_the_gallery_license_when_no_copyright_is_asserted(heic0601a):
    assert heic0601a.license == "CC BY 4.0"
    assert GALLERIES["esa_hubble"].default_license == "CC BY 4.0"


def test_render_with_no_published_position_yields_nulls_not_a_guess():
    """opo0205c: ESA/Hubble publishes neither coordinates nor field of view. It must
    become a NULL row so the crawler remembers it, never a guessed position — a wrong
    pixel scale corrupts band-limiting (§4.4)."""
    entry = parse_detail(
        load("hubble_detail_opo0205c.html"),
        entry_id="opo0205c", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/opo0205c/",
    )
    assert entry.ra_deg is None
    assert entry.dec_deg is None
    assert entry.fov_radius_arcmin is None
    assert entry.pixel_scale_arcsec is None
    assert entry.entry_type == "Photographic"
    assert entry.parsed_ok is True     # the page parsed; the data is simply absent


def test_copyrighted_render_is_flagged_so_it_is_never_ingested_as_cc_by():
    """opo0205c's credit asserts AAO copyright. ESA/Hubble hosts third-party
    copyrighted images, so the per-gallery CC BY 4.0 default is NOT universal. Claiming
    CC BY 4.0 here would attach a false license to every fingerprint record (§5.5)."""
    credit = parse_credit(load("hubble_detail_opo0205c.html"))
    assert asserts_copyright(credit) is True
    entry = parse_detail(
        load("hubble_detail_opo0205c.html"),
        entry_id="opo0205c", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/opo0205c/",
    )
    assert entry.license is None       # unestablished, not defaulted


def test_permissive_credits_are_not_flagged_as_copyrighted():
    assert asserts_copyright(parse_credit(load("hubble_detail_heic0601a.html"))) is False
    assert asserts_copyright(parse_credit(load("eso_detail_eso1103a.html"))) is False
    assert asserts_copyright(None) is False


def test_artwork_has_no_position_and_no_filters():
    """An artist's impression has nothing to match; it must not enter the position index."""
    entry = parse_detail(
        load("hubble_detail_heic0211i.html"),
        entry_id="heic0211i", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/heic0211i/",
    )
    assert entry.entry_type == "Artwork"
    assert entry.ra_deg is None
    assert entry.palette_class == "unknown"


def test_a_page_that_does_not_parse_at_all_reports_failure():
    """Distinguishing 'parsed fine, data absent' from 'parse broke' is what lets the
    crawler abort loudly on a site redesign instead of indexing an empty archive."""
    entry = parse_detail(
        "<html><body><p>503 Service Unavailable</p></body></html>",
        entry_id="whatever", gallery="eso",
        detail_url="https://www.eso.org/public/images/whatever/",
    )
    assert entry.parsed_ok is False
```

- [ ] **Step 3: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_gallery_detail.py -v`
Expected: FAIL — `ImportError: cannot import name 'parse_detail'`

- [ ] **Step 4: Write the implementation**

Append to `src/autocontrast/db/discover/gallery.py`:

```python
from autocontrast.fingerprint.palette import palette_class_from_gallery_bands


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

    Rows look like ``Optical B | 435 nm | Hubble Space Telescope | ACS``; the filter name
    is the trailing token of the band cell ("Optical H-alpha" -> "H-alpha").
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
        cells = [c for c in cells if c]
        if not cells or cells[0].lower().startswith("band"):
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
```

Note: this imports `palette_class_from_gallery_bands`, built in Task 4. Implement Task 4
first if working strictly test-green, or add a temporary local stub returning `"unknown"`
and delete it in Task 4. **Preferred: do Task 4 before Task 3's Step 5.**

- [ ] **Step 5: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_gallery_detail.py -v`
Expected: PASS, 16 tests.

- [ ] **Step 6: Commit**

```bash
git add tools/capture_gallery_fixtures.sh tests/fixtures/gallery \
        src/autocontrast/db/discover/gallery.py tests/test_gallery_detail.py
git commit -m "discover: parse gallery detail pages; six committed fixtures

Published metadata reproduces the hand-built catalog exactly — eso1103a parses to
83.82217 / -5.39099 and 24.61' against catalog values of the same, and pixel scale
falls out as 0.1001 and 0.238 arcsec/px from fov/width. So the cone can be filtered
BEFORE downloading a 324-megapixel JPEG.

Fixtures deliberately include the awkward cases, not just the happy path: opo0205c
(no published position -> NULL row) and heic0211i (artwork). Commit 12e7041 shipped
two bugs past 138 green tests because every fixture was 2D mono FITS; that was a
fixture-diversity gap, so the diversity is here from the start.

Licensing finding: ESA/Hubble hosts third-party COPYRIGHTED renders (opo0205c is
'Copyright (c) Anglo-Australian Observatory'), and there is no per-image
machine-readable license anywhere on either site. So the per-gallery CC BY 4.0
default applies only when the credit does not assert copyright; otherwise the
license is left unestablished. Attaching a false CC BY 4.0 to a fingerprint record
would be a §5.5 violation that travels with the record forever.

parsed_ok separates 'parsed fine, data absent' from 'parsing broke', which is what
lets the crawler abort loudly on a redesign instead of indexing an empty archive."
```

---

### Task 4: Palette class from gallery filter bands

**Files:**
- Modify: `src/autocontrast/fingerprint/palette.py`
- Test: `tests/test_palette_gallery.py`

**Interfaces:**
- Consumes: nothing (pure).
- Produces: `palette_class_from_gallery_bands(bands: list[str]) -> str`, returning a member
  of the existing `PALETTE_CLASSES`.

**AUTHOR DECISION.** The classification body encodes a domain judgment: how a professional
multi-filter composite maps onto the amateur palette taxonomy. The default below treats a
broadband-dominated composite as `RGB` even when a narrowband layer is blended in, because
that matches how the curated catalog labels `heic0601a` (B, V, **H-alpha**, I, Z → `RGB`).
The alternative — calling it `HaRGB` — is defensible too, and would change which references
pass the §2.3 chroma gate. Adjust the rules if you disagree; the tests encode the default.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_palette_gallery.py
"""Palette derivation from gallery-published filter bands (§4.3)."""

from __future__ import annotations

import pytest

from autocontrast.fingerprint.palette import (
    PALETTE_CLASSES,
    palette_class_from_filters,
    palette_class_from_gallery_bands,
)


def test_absent_filter_table_is_unknown_not_rgb():
    """The critical asymmetry against palette_class_from_filters: an empty FITS FILTER
    keyword is POSITIVE evidence of a one-shot-colour camera, so that function returns
    RGB. An absent gallery filter table is merely missing metadata, so this one must
    return 'unknown'. Same empty input, opposite correct answers."""
    assert palette_class_from_gallery_bands([]) == "unknown"
    assert palette_class_from_filters([]) == "RGB"


def test_real_hubble_bands_are_rgb():
    """heic0601a: B, V, H-alpha, I, Z — broadband-dominated; catalog declares RGB."""
    assert palette_class_from_gallery_bands(["B", "V", "H-alpha", "I", "Z"]) == "RGB"


def test_real_eso_bands_are_rgb():
    """eso1103a: U, B, V, R, H-alpha."""
    assert palette_class_from_gallery_bands(["U", "B", "V", "R", "H-alpha"]) == "RGB"


def test_pure_narrowband_triplet_is_sho():
    assert palette_class_from_gallery_bands(["S-II", "H-alpha", "O-III"]) == "SHO"


def test_pure_narrowband_pair_is_hoo():
    assert palette_class_from_gallery_bands(["H-alpha", "O-III"]) == "HOO"


@pytest.mark.parametrize("alias", ["H-alpha", "Halpha", "Ha", "H-Alpha"])
def test_h_alpha_aliases_are_recognised(alias):
    assert palette_class_from_gallery_bands([alias, "O-III"]) == "HOO"


@pytest.mark.parametrize("alias", ["O-III", "OIII", "O3", "[O III]"])
def test_oiii_aliases_are_recognised(alias):
    assert palette_class_from_gallery_bands(["H-alpha", alias]) == "HOO"


def test_single_broadband_filter_is_not_a_colour_palette():
    """One filter cannot make a colour composite; claiming RGB would be a guess."""
    assert palette_class_from_gallery_bands(["V"]) == "unknown"


def test_single_narrowband_filter_is_unknown():
    assert palette_class_from_gallery_bands(["H-alpha"]) == "unknown"


def test_unrecognised_bands_are_unknown_never_guessed():
    """§2.1: a mis-derived palette lets a mismatched reference push channel ratios."""
    assert palette_class_from_gallery_bands(["Radio 21cm"]) == "unknown"
    assert palette_class_from_gallery_bands(["", "  "]) == "unknown"


def test_result_is_always_a_declared_palette_class():
    for bands in ([], ["V"], ["B", "V"], ["H-alpha", "O-III"], ["nonsense"]):
        assert palette_class_from_gallery_bands(bands) in PALETTE_CLASSES
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_palette_gallery.py -v`
Expected: FAIL — `ImportError: cannot import name 'palette_class_from_gallery_bands'`

- [ ] **Step 3: Write the implementation**

Append to `src/autocontrast/fingerprint/palette.py`:

```python
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_palette_gallery.py -v`
Expected: PASS, 18 tests.

- [ ] **Step 5: Confirm the existing palette tests still pass unchanged**

Run: `.venv/bin/python -m pytest tests/test_palette.py -q`
Expected: PASS — the new function must not alter `palette_class_from_filters`.

- [ ] **Step 6: Commit**

```bash
git add src/autocontrast/fingerprint/palette.py tests/test_palette_gallery.py
git commit -m "§4.3: derive palette class from gallery-published filter bands

Both galleries publish a Colours & filters table (heic0601a: B 435, V 555,
H-alpha 658, I 775, Z 850), so a discovered reference's palette is DERIVED, never
guessed or defaulted to RGB.

Key asymmetry, encoded in a test: an empty FITS FILTER keyword means a one-shot-colour
camera and correctly yields RGB, but an absent gallery filter table is just missing
metadata and must yield 'unknown'. Same empty input, opposite right answers, so the
new function does not delegate the empty case to the old one.

Broadband-dominated composites classify as RGB even with a blended narrowband layer,
matching the curated catalog's own label for heic0601a."
```

---

### Task 5: Parse listing pages

**Files:**
- Modify: `src/autocontrast/db/discover/gallery.py`
- Test: `tests/test_gallery_listing.py`

**Interfaces:**
- Consumes: Task 2's `GalleryConfig`.
- Produces: `ListingItem` dataclass (`id`, `title`, `width_px`, `height_px`, `detail_path`);
  `parse_listing(doc: str) -> list[ListingItem]`;
  `parse_result_total(doc: str) -> int | None`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_gallery_listing.py
"""Listing-page parsing. Results arrive as an inline `var images = [...]` JS literal —
machine-oriented data that churns far less than rendered markup, and it publishes pixel
dimensions, which is what makes gate G4 satisfiable without a download."""

from __future__ import annotations

from pathlib import Path

from autocontrast.db.discover.gallery import parse_listing, parse_result_total

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


def load(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8", errors="replace")


def test_hubble_listing_yields_every_result():
    """The page reports 'Showing 1 to 35 of 35'."""
    items = parse_listing(load("hubble_listing_orion.html"))
    assert len(items) == 35


def test_eso_listing_yields_every_result():
    items = parse_listing(load("eso_listing_orion.html"))
    assert len(items) == 26


def test_listing_publishes_dimensions_used_for_pixel_scale():
    items = {i.id: i for i in parse_listing(load("hubble_listing_orion.html"))}
    assert items["heic0601a"].width_px == 18000
    assert items["heic0601a"].height_px == 18000
    assert items["heic0601a"].detail_path == "/images/heic0601a/"


def test_eso_detail_paths_carry_the_public_prefix():
    items = {i.id: i for i in parse_listing(load("eso_listing_orion.html"))}
    assert items["eso1103a"].width_px == 8948
    assert items["eso1103a"].height_px == 8597
    assert items["eso1103a"].detail_path == "/public/images/eso1103a/"


def test_titles_are_html_unescaped():
    items = {i.id: i for i in parse_listing(load("hubble_listing_orion.html"))}
    assert items["heic0601a"].title == "Hubble's sharpest view of the Orion Nebula"


def test_listing_contains_the_curated_seed_ids():
    """Four of the five hand-curated M42 seeds are discovered automatically, which is
    the evidence that the curated catalog is a subset of what discovery returns."""
    ids = {i.id for i in parse_listing(load("hubble_listing_orion.html"))}
    assert {"heic0601a", "opo0205c", "opo0205d", "opo9545a"} <= ids


def test_navigation_links_are_not_mistaken_for_results():
    ids = {i.id for i in parse_listing(load("hubble_listing_orion.html"))}
    assert not ids & {"feed", "potm", "potw", "search", "viewall", "archive"}


def test_result_total_is_read_from_the_page():
    assert parse_result_total(load("hubble_listing_orion.html")) == 35
    assert parse_result_total(load("eso_listing_orion.html")) == 26


def test_page_without_a_results_block_yields_nothing_rather_than_raising():
    assert parse_listing("<html><body>No results.</body></html>") == []
    assert parse_result_total("<html><body>No results.</body></html>") is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_gallery_listing.py -v`
Expected: FAIL — `ImportError: cannot import name 'parse_listing'`

- [ ] **Step 3: Write the implementation**

Append to `src/autocontrast/db/discover/gallery.py`:

```python
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_gallery_listing.py -v`
Expected: PASS, 9 tests.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/db/discover/gallery.py tests/test_gallery_listing.py
git commit -m "discover: parse listing pages from the inline JS data literal

Results are not rendered markup — they arrive as 'var images = [ {id, title, width,
height, url}, ... ];'. Parsing that instead of HTML is both more robust (machine-
oriented data, survives visual redesigns) and strictly more informative: width and
height are published, so pixel scale is computable without downloading the image.

Verified: 35 items for the Hubble Orion query, 26 for ESO, matching each page's own
reported total. The Hubble results include heic0601a, opo0205c, opo0205d and opo9545a
— four of the five hand-curated M42 seeds, discovered automatically."
```

---

### Task 6: The position index

**Files:**
- Create: `src/autocontrast/db/discover/index.py`
- Test: `tests/test_gallery_index.py`

**Interfaces:**
- Consumes: `GalleryEntry` (Task 3), `separation_arcmin`/`cones_overlap` (Task 1).
- Produces: `IndexMatch` dataclass (`entry: GalleryEntry`, `separation_arcmin: float`);
  `GalleryIndex` with `upsert(entry)`, `get(gallery, entry_id)`, `has(gallery, entry_id)`,
  `cone_search(ra_deg, dec_deg, radius_arcmin) -> list[IndexMatch]`, `count()`,
  `get_sync_state(gallery) -> str | None`, `set_sync_state(gallery, iso_utc)`, `close()`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_gallery_index.py
"""The local position index — a genuine cone search over professional renders (§5.1)."""

from __future__ import annotations

import pytest

from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.discover.index import GalleryIndex


def entry(entry_id="x1", gallery="esa_hubble", ra=83.8, dec=-5.4, radius=20.0,
          palette="RGB", license_text="CC BY 4.0", attribution="NASA, ESA"):
    return GalleryEntry(
        id=entry_id, gallery=gallery,
        detail_url=f"https://example.test/images/{entry_id}/",
        image_url=f"https://cdn.example.test/images/large/{entry_id}.jpg",
        ra_deg=ra, dec_deg=dec, fov_w_arcmin=30.0, fov_h_arcmin=30.0,
        fov_radius_arcmin=radius, width_px=18000, height_px=18000,
        pixel_scale_arcsec=0.1001, object_name="Messier 42", category="Nebulae",
        entry_type="Observation", palette_class=palette, license=license_text,
        attribution=attribution, published_utc="2006-01-11T16:00:00Z", parsed_ok=True,
    )


@pytest.fixture
def index(tmp_path):
    idx = GalleryIndex(tmp_path / "gallery_index.sqlite")
    yield idx
    idx.close()


def test_roundtrip_preserves_every_field(index):
    original = entry()
    index.upsert(original)
    assert index.get("esa_hubble", "x1") == original


def test_upsert_is_idempotent_on_gallery_and_id(index):
    index.upsert(entry(ra=83.8))
    index.upsert(entry(ra=99.9))
    assert index.count() == 1
    assert index.get("esa_hubble", "x1").ra_deg == pytest.approx(99.9)


def test_same_id_in_two_galleries_are_distinct_rows(index):
    index.upsert(entry(entry_id="dup", gallery="esa_hubble"))
    index.upsert(entry(entry_id="dup", gallery="eso"))
    assert index.count() == 2


def test_has_reports_membership_so_the_crawler_can_skip(index):
    assert index.has("esa_hubble", "x1") is False
    index.upsert(entry())
    assert index.has("esa_hubble", "x1") is True


def test_cone_search_finds_an_overlapping_footprint(index):
    index.upsert(entry(ra=83.82, dec=-5.39, radius=24.6))
    matches = index.cone_search(83.82, -5.38, radius_arcmin=30.0)
    assert [m.entry.id for m in matches] == ["x1"]
    assert matches[0].separation_arcmin < 1.0


def test_cone_search_excludes_a_disjoint_footprint(index):
    index.upsert(entry(ra=83.8, dec=-5.4, radius=5.0))
    assert index.cone_search(200.0, 40.0, radius_arcmin=10.0) == []


def test_overlap_is_boundary_inclusive(index):
    """Footprints exactly touching count as overlapping, matching FingerprintStore."""
    index.upsert(entry(ra=10.0, dec=0.0, radius=6.0))
    assert len(index.cone_search(10.0, 10.0 / 60.0, radius_arcmin=4.0)) == 1


def test_entries_without_a_position_are_never_returned(index):
    """opo0205c-style NULL rows are remembered so the crawler skips them, but they can
    never be selected as a reference."""
    index.upsert(entry(entry_id="nopos", ra=None, dec=None, radius=None))
    assert index.has("esa_hubble", "nopos") is True
    assert index.cone_search(83.8, -5.4, radius_arcmin=60.0) == []


def test_cone_search_handles_ra_wrap(index):
    """The dec-band SQL prefilter cannot express RA wrap, so the precise haversine
    check must catch it: 359.9 and 0.1 are 12 arcmin apart, not 359 degrees."""
    index.upsert(entry(ra=359.9, dec=0.0, radius=5.0))
    assert len(index.cone_search(0.1, 0.0, radius_arcmin=5.0)) == 1


def test_cone_search_orders_nearest_first(index):
    index.upsert(entry(entry_id="far", ra=84.3, dec=-5.4, radius=20.0))
    index.upsert(entry(entry_id="near", ra=83.81, dec=-5.4, radius=20.0))
    assert [m.entry.id for m in index.cone_search(83.8, -5.4, 10.0)] == ["near", "far"]


def test_sync_state_roundtrips_per_gallery(index):
    assert index.get_sync_state("eso") is None
    index.set_sync_state("eso", "2026-07-26T00:00:00Z")
    index.set_sync_state("esa_hubble", "2026-01-01T00:00:00Z")
    assert index.get_sync_state("eso") == "2026-07-26T00:00:00Z"
    assert index.get_sync_state("esa_hubble") == "2026-01-01T00:00:00Z"


def test_index_is_a_separate_file_from_the_fingerprint_store(tmp_path):
    """The index is a regenerable external-derived cache; deleting it must never risk
    user fingerprints, so it lives in its own file."""
    path = tmp_path / "gallery_index.sqlite"
    idx = GalleryIndex(path)
    idx.upsert(entry())
    idx.close()
    assert path.exists()
    assert not (tmp_path / "fingerprints.sqlite").exists()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_gallery_index.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.db.discover.index'`

- [ ] **Step 3: Write the implementation**

```python
# src/autocontrast/db/discover/index.py
"""Local positional index over professional gallery renders (§5.2.1).

No cone-search API exists over press-release renders, so discovery works by indexing the
galleries' own published, AVM-derived positions and cone-searching that index locally.
This keeps §5.1's rule intact: matching is by position, never by object name.

Deliberately a SEPARATE SQLite file from ``fingerprints.sqlite``. This is a regenerable
cache derived from external sites; keeping it apart means "delete it and re-crawl" can
never endanger user fingerprints, and the two version on different cadences.
"""

from __future__ import annotations

import sqlite3
from dataclasses import dataclass, fields
from pathlib import Path

from ..skymath import cones_overlap, separation_arcmin
from .gallery import GalleryEntry

_SCHEMA = """
CREATE TABLE IF NOT EXISTS gallery_index (
    id                 TEXT NOT NULL,
    gallery            TEXT NOT NULL,
    detail_url         TEXT NOT NULL,
    image_url          TEXT,
    ra_deg             REAL,
    dec_deg            REAL,
    fov_w_arcmin       REAL,
    fov_h_arcmin       REAL,
    fov_radius_arcmin  REAL,
    width_px           INTEGER,
    height_px          INTEGER,
    pixel_scale_arcsec REAL,
    object_name        TEXT,
    category           TEXT,
    entry_type         TEXT,
    palette_class      TEXT NOT NULL,
    license            TEXT,
    attribution        TEXT,
    published_utc      TEXT,
    parsed_ok          INTEGER NOT NULL,
    PRIMARY KEY (gallery, id)
);
CREATE INDEX IF NOT EXISTS ix_gallery_dec ON gallery_index(dec_deg);

CREATE TABLE IF NOT EXISTS sync_state (
    gallery        TEXT PRIMARY KEY,
    last_synced_utc TEXT NOT NULL
);
"""

_COLUMNS = [f.name for f in fields(GalleryEntry)]


@dataclass
class IndexMatch:
    """An indexed render whose footprint overlaps the query cone."""

    entry: GalleryEntry
    separation_arcmin: float


class GalleryIndex:
    """SQLite index of gallery render positions."""

    def __init__(self, db_path: str | Path):
        self.db_path = str(db_path)
        self._conn = sqlite3.connect(self.db_path)
        self._conn.row_factory = sqlite3.Row
        self._conn.executescript(_SCHEMA)
        self._conn.commit()

    def upsert(self, entry: GalleryEntry) -> None:
        values = [getattr(entry, name) for name in _COLUMNS]
        values[_COLUMNS.index("parsed_ok")] = int(entry.parsed_ok)
        placeholders = ",".join("?" * len(_COLUMNS))
        self._conn.execute(
            f"INSERT OR REPLACE INTO gallery_index ({','.join(_COLUMNS)}) "
            f"VALUES ({placeholders})",
            values,
        )
        self._conn.commit()

    def _row_to_entry(self, row: sqlite3.Row) -> GalleryEntry:
        data = {name: row[name] for name in _COLUMNS}
        data["parsed_ok"] = bool(data["parsed_ok"])
        return GalleryEntry(**data)

    def get(self, gallery: str, entry_id: str) -> GalleryEntry | None:
        row = self._conn.execute(
            "SELECT * FROM gallery_index WHERE gallery = ? AND id = ?", (gallery, entry_id)
        ).fetchone()
        return self._row_to_entry(row) if row is not None else None

    def has(self, gallery: str, entry_id: str) -> bool:
        row = self._conn.execute(
            "SELECT 1 FROM gallery_index WHERE gallery = ? AND id = ?", (gallery, entry_id)
        ).fetchone()
        return row is not None

    def count(self) -> int:
        return int(self._conn.execute("SELECT COUNT(*) FROM gallery_index").fetchone()[0])

    def cone_search(
        self, ra_deg: float, dec_deg: float, radius_arcmin: float
    ) -> list[IndexMatch]:
        """Indexed renders whose footprint overlaps the query cone, nearest first.

        The SQL prefilter is a declination band — a safe superset, since separation is
        always at least |Δdec|. It cannot express RA wrap, so the precise haversine test
        below is what actually decides overlap.
        """
        sql = (
            "SELECT * FROM gallery_index WHERE ra_deg IS NOT NULL AND dec_deg IS NOT NULL "
            "AND fov_radius_arcmin IS NOT NULL "
            "AND ABS(dec_deg - ?) <= (? + fov_radius_arcmin) / 60.0"
        )
        matches: list[IndexMatch] = []
        for row in self._conn.execute(sql, (dec_deg, radius_arcmin)):
            sep = separation_arcmin(ra_deg, dec_deg, row["ra_deg"], row["dec_deg"])
            if not cones_overlap(sep, radius_arcmin, row["fov_radius_arcmin"]):
                continue
            matches.append(IndexMatch(entry=self._row_to_entry(row), separation_arcmin=sep))
        matches.sort(key=lambda m: m.separation_arcmin)
        return matches

    def get_sync_state(self, gallery: str) -> str | None:
        row = self._conn.execute(
            "SELECT last_synced_utc FROM sync_state WHERE gallery = ?", (gallery,)
        ).fetchone()
        return row["last_synced_utc"] if row is not None else None

    def set_sync_state(self, gallery: str, iso_utc: str) -> None:
        self._conn.execute(
            "INSERT OR REPLACE INTO sync_state (gallery, last_synced_utc) VALUES (?, ?)",
            (gallery, iso_utc),
        )
        self._conn.commit()

    def close(self) -> None:
        self._conn.close()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_gallery_index.py -v`
Expected: PASS, 12 tests.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/db/discover/index.py tests/test_gallery_index.py
git commit -m "discover: local position index with a real cone search

Turns discovery into position matching rather than name matching, keeping §5.1
intact. Separate SQLite file from fingerprints.sqlite because this is a regenerable
external-derived cache: 'delete it and re-crawl' must never risk user fingerprints.

Positionless entries (opo0205c) are stored as NULL rows so the crawler remembers to
skip them, but can never be returned as a reference. RA wrap is tested explicitly —
the dec-band SQL prefilter cannot express it, so the haversine check is what decides."
```

---

### Task 7: Polite fetcher, crawl, and incremental sync

**Files:**
- Create: `src/autocontrast/db/discover/crawl.py`
- Test: `tests/test_crawl.py`

**Interfaces:**
- Consumes: `GALLERIES`, `search_url`, `detail_url`, `parse_listing`, `parse_detail`,
  `parse_result_total` (Tasks 2/3/5); `GalleryIndex` (Task 6).
- Produces: `Fetcher` protocol (`get_text(url) -> str`, `get_bytes(url, max_bytes) -> bytes`);
  `PoliteFetcher(user_agent=..., min_interval_s=1.0, max_requests=8000)`;
  `CrawlReport` dataclass (`gallery`, `listed`, `indexed`, `skipped`, `failed`, `detail`);
  `crawl_gallery(index, cfg, fetcher, criteria, *, max_pages=None, refresh=False) -> CrawlReport`;
  `sync_gallery(index, cfg, fetcher, *, now_utc, safety_days=7) -> CrawlReport`;
  `DriftError` exception; constants `DEFAULT_USER_AGENT`, `MAX_IMAGE_BYTES = 256 * 1024 * 1024`,
  `DRIFT_THRESHOLD = 0.5`, `SYNC_SAFETY_DAYS = 7`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_crawl.py
"""Crawl orchestration. Network is behind the Fetcher protocol, so these tests are
offline and deterministic."""

from __future__ import annotations

from pathlib import Path

import pytest

from autocontrast.db.discover.crawl import (
    DRIFT_THRESHOLD,
    MAX_IMAGE_BYTES,
    DriftError,
    crawl_gallery,
    sync_gallery,
)
from autocontrast.db.discover.gallery import GALLERIES
from autocontrast.db.discover.index import GalleryIndex

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


def load(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8", errors="replace")


class FakeFetcher:
    """Serves fixtures by URL substring and records the request order."""

    def __init__(self, routes: dict[str, str], default: str | None = None):
        self.routes = routes
        self.default = default
        self.requested: list[str] = []

    def get_text(self, url: str) -> str:
        self.requested.append(url)
        for needle, body in self.routes.items():
            if needle in url:
                return body
        if self.default is not None:
            return self.default
        raise AssertionError(f"unrouted URL: {url}")

    def get_bytes(self, url: str, *, max_bytes: int) -> bytes:
        self.requested.append(url)
        return b"\xff\xd8\xff\xd9"


@pytest.fixture
def index(tmp_path):
    idx = GalleryIndex(tmp_path / "gallery_index.sqlite")
    yield idx
    idx.close()


def hubble_routes():
    return {
        "archive/search": load("hubble_listing_orion.html"),
        "/images/heic0601a/": load("hubble_detail_heic0601a.html"),
        "/images/opo0205c/": load("hubble_detail_opo0205c.html"),
        "/images/heic0211i/": load("hubble_detail_heic0211i.html"),
    }


def test_crawl_indexes_positioned_and_positionless_entries(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.listed == 35
    assert report.indexed == 35
    assert index.has("esa_hubble", "heic0601a")
    assert index.has("esa_hubble", "opo0205c")


def test_crawled_entry_carries_position_and_scale_from_published_metadata(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                  {"subject_name": "Orion Nebula"}, max_pages=1)
    entry = index.get("esa_hubble", "heic0601a")
    assert entry.ra_deg == pytest.approx(83.7905, abs=1e-3)
    assert entry.pixel_scale_arcsec == pytest.approx(0.1001, abs=1e-4)
    assert entry.width_px == 18000        # from the listing, not the download


def test_crawl_skips_already_indexed_ids(index):
    routes = hubble_routes()
    first = FakeFetcher(routes, default=load("hubble_detail_heic0211i.html"))
    crawl_gallery(index, GALLERIES["esa_hubble"], first,
                  {"subject_name": "Orion Nebula"}, max_pages=1)

    second = FakeFetcher(routes, default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], second,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.skipped == 35
    assert report.indexed == 0
    detail_requests = [u for u in second.requested if "archive/search" not in u]
    assert detail_requests == []          # resumable: no wasted requests


def test_refresh_re_fetches_indexed_ids(index):
    routes = hubble_routes()
    crawl_gallery(index, GALLERIES["esa_hubble"],
                  FakeFetcher(routes, default=load("hubble_detail_heic0211i.html")),
                  {"subject_name": "Orion Nebula"}, max_pages=1)
    fetcher = FakeFetcher(routes, default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1, refresh=True)
    assert report.indexed == 35
    assert report.skipped == 0


def test_site_drift_aborts_loudly_instead_of_indexing_an_empty_archive(index):
    """A Djangoplicity redesign must be a noisy failure, not a quietly-empty index (§12)."""
    fetcher = FakeFetcher({"archive/search": load("hubble_listing_orion.html")},
                          default="<html><body>503 Service Unavailable</body></html>")
    with pytest.raises(DriftError) as excinfo:
        crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                      {"subject_name": "Orion Nebula"}, max_pages=1)
    assert "parse" in str(excinfo.value).lower()


def test_legitimately_positionless_pages_do_not_trip_the_drift_guard(index):
    """Artwork and old photographic releases publish no position; that is normal data
    absence, not parse breakage, so a page full of them must NOT abort."""
    fetcher = FakeFetcher({"archive/search": load("hubble_listing_orion.html")},
                          default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.indexed == 35
    assert report.failed == 0


def test_eso_crawl_uses_the_unpaginated_results_path(index):
    fetcher = FakeFetcher({
        "archive/search": load("eso_listing_orion.html"),
        "/public/images/eso1103a/": load("eso_detail_eso1103a.html"),
    }, default=load("eso_detail_eso1103a.html"))
    report = crawl_gallery(index, GALLERIES["eso"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.listed == 26
    assert all("/page/" not in u for u in fetcher.requested if "archive/search" in u)


def test_sync_queries_from_the_last_sync_minus_the_safety_window(index):
    index.set_sync_state("esa_hubble", "2026-07-20T00:00:00Z")
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    sync_gallery(index, GALLERIES["esa_hubble"], fetcher, now_utc="2026-07-26T00:00:00Z")
    search = next(u for u in fetcher.requested if "archive/search" in u)
    # 2026-07-20 minus the 7-day safety window
    assert "published_since_year=2026" in search
    assert "published_since_month=7" in search
    assert "published_since_day=13" in search


def test_first_sync_with_no_state_crawls_from_the_epoch_year(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    sync_gallery(index, GALLERIES["esa_hubble"], fetcher, now_utc="2026-07-26T00:00:00Z")
    search = next(u for u in fetcher.requested if "archive/search" in u)
    assert "published_since_year=1990" in search


def test_sync_records_new_state_so_the_next_run_is_incremental(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    sync_gallery(index, GALLERIES["esa_hubble"], fetcher, now_utc="2026-07-26T00:00:00Z")
    assert index.get_sync_state("esa_hubble") == "2026-07-26T00:00:00Z"


def test_download_cap_is_a_quarter_gigabyte():
    """The 18000x18000 Hubble Orion mosaic is the largest known case and fits inside."""
    assert MAX_IMAGE_BYTES == 256 * 1024 * 1024
    assert DRIFT_THRESHOLD == 0.5
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_crawl.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.db.discover.crawl'`

- [ ] **Step 3: Write the implementation**

```python
# src/autocontrast/db/discover/crawl.py
"""Crawl and incremental sync for the gallery position index (§5.2.1).

All network access is behind the :class:`Fetcher` protocol so the orchestration is
testable offline. Politeness is not optional: these are free public archives being
enumerated, so requests are serialized at one per second with a descriptive
User-Agent, backoff, and a hard per-run cap.
"""

from __future__ import annotations

import random
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Protocol

from .gallery import (
    GalleryConfig,
    detail_url,
    parse_detail,
    parse_listing,
    parse_result_total,
    search_url,
)
from .index import GalleryIndex

DEFAULT_USER_AGENT = (
    "AutoContrast/0.1 "
    "(+https://github.com/scarter4work/autocontrast; scarter4work@yahoo.com)"
)
MAX_IMAGE_BYTES = 256 * 1024 * 1024   # the 18000x18000 Orion mosaic fits comfortably
MAX_REQUESTS_PER_RUN = 8000           # full cold crawl of both galleries, with headroom
MIN_REQUEST_INTERVAL_S = 1.0
DRIFT_THRESHOLD = 0.5                 # >50% unparseable detail pages on one listing page
SYNC_SAFETY_DAYS = 7
RESULTS_PER_PAGE = 50
EPOCH_YEAR = 1990                     # "everything": both archives postdate this


class DriftError(RuntimeError):
    """Raised when detail pages stop parsing — a site redesign, not missing data.

    Loud by design (§12): a quietly-empty index would read as "nothing to discover".
    """


class RequestBudgetExceeded(RuntimeError):
    """Raised when a run exceeds ``MAX_REQUESTS_PER_RUN``."""


class Fetcher(Protocol):
    def get_text(self, url: str) -> str: ...
    def get_bytes(self, url: str, *, max_bytes: int) -> bytes: ...


class PoliteFetcher:
    """Rate-limited, retrying, capped HTTP client built on ``urllib.request``."""

    def __init__(
        self,
        user_agent: str = DEFAULT_USER_AGENT,
        min_interval_s: float = MIN_REQUEST_INTERVAL_S,
        max_requests: int = MAX_REQUESTS_PER_RUN,
        max_retries: int = 3,
    ):
        self.user_agent = user_agent
        self.min_interval_s = min_interval_s
        self.max_requests = max_requests
        self.max_retries = max_retries
        self._requests = 0
        self._last_request_at = 0.0

    def _throttle(self) -> None:
        if self._requests >= self.max_requests:
            raise RequestBudgetExceeded(
                f"Request budget of {self.max_requests} exhausted; refusing to continue."
            )
        elapsed = time.monotonic() - self._last_request_at
        # Jitter avoids a metronomic request pattern.
        wait = self.min_interval_s + random.uniform(0.0, 0.25) - elapsed
        if wait > 0:
            time.sleep(wait)
        self._last_request_at = time.monotonic()
        self._requests += 1

    def _open(self, url: str):
        request = urllib.request.Request(url, headers={"User-Agent": self.user_agent})
        for attempt in range(self.max_retries + 1):
            self._throttle()
            try:
                return urllib.request.urlopen(request, timeout=60)
            except urllib.error.HTTPError as exc:
                retry_after = exc.headers.get("Retry-After") if exc.headers else None
                if exc.code == 429 and retry_after and attempt < self.max_retries:
                    time.sleep(min(float(retry_after), 120.0))
                    continue
                if 500 <= exc.code < 600 and attempt < self.max_retries:
                    time.sleep(2.0 ** attempt)
                    continue
                raise
            except (urllib.error.URLError, TimeoutError):
                if attempt < self.max_retries:
                    time.sleep(2.0 ** attempt)
                    continue
                raise
        raise RuntimeError(f"unreachable retry state for {url}")

    def get_text(self, url: str) -> str:
        with self._open(url) as response:
            return response.read().decode("utf-8", errors="replace")

    def get_bytes(self, url: str, *, max_bytes: int = MAX_IMAGE_BYTES) -> bytes:
        with self._open(url) as response:
            payload = response.read(max_bytes + 1)
        if len(payload) > max_bytes:
            raise ValueError(
                f"{url} exceeds the {max_bytes} byte cap; refusing to buffer it."
            )
        return payload


@dataclass
class CrawlReport:
    gallery: str
    listed: int
    indexed: int
    skipped: int
    failed: int
    detail: str


def crawl_gallery(
    index: GalleryIndex,
    cfg: GalleryConfig,
    fetcher: Fetcher,
    criteria: dict[str, str],
    *,
    max_pages: int | None = None,
    refresh: bool = False,
) -> CrawlReport:
    """Enumerate a gallery search and index every result's published metadata.

    Resumable: ids already present are skipped without a request unless ``refresh``.
    Aborts with :class:`DriftError` if detail pages stop parsing.
    """
    listed = indexed = skipped = failed = 0
    page = 1
    total: int | None = None

    while True:
        listing_doc = fetcher.get_text(search_url(cfg, criteria, page=page))
        if total is None:
            total = parse_result_total(listing_doc)
        items = parse_listing(listing_doc)
        if not items:
            break
        listed += len(items)

        unparseable = 0
        considered = 0
        for item in items:
            if not refresh and index.has(cfg.key, item.id):
                skipped += 1
                continue
            considered += 1
            url = detail_url(cfg, item.id)
            entry = parse_detail(
                fetcher.get_text(url),
                entry_id=item.id, gallery=cfg.key, detail_url=url,
                width_px=item.width_px, height_px=item.height_px,
            )
            if not entry.parsed_ok:
                unparseable += 1
                failed += 1
                continue
            index.upsert(entry)
            indexed += 1

        # Only genuine parse breakage counts. A page full of artwork parses fine and
        # simply publishes no position — that is data absence, not drift.
        if considered and unparseable / considered > DRIFT_THRESHOLD:
            raise DriftError(
                f"{unparseable}/{considered} detail pages on page {page} of {cfg.key} "
                f"failed to parse (>{DRIFT_THRESHOLD:.0%}). The gallery markup has most "
                "likely changed; refusing to index a partial archive. Re-run "
                "tools/capture_gallery_fixtures.sh and fix the parsers."
            )

        if not cfg.results_paginated:
            break
        if max_pages is not None and page >= max_pages:
            break
        if total is not None and page * RESULTS_PER_PAGE >= total:
            break
        page += 1

    return CrawlReport(
        gallery=cfg.key, listed=listed, indexed=indexed, skipped=skipped, failed=failed,
        detail=(f"{cfg.key}: listed {listed}, indexed {indexed}, skipped {skipped}, "
                f"failed {failed}"),
    )


def sync_gallery(
    index: GalleryIndex,
    cfg: GalleryConfig,
    fetcher: Fetcher,
    *,
    now_utc: str,
    safety_days: int = SYNC_SAFETY_DAYS,
    max_pages: int | None = None,
) -> CrawlReport:
    """Incremental sync using the galleries' ``published_since`` filter.

    The window starts at the last successful sync minus ``safety_days``, covering late
    edits and backdated publication. ``now_utc`` is passed in rather than read from the
    clock so the behavior is testable.
    """
    last = index.get_sync_state(cfg.key)
    if last is None:
        since = datetime(EPOCH_YEAR, 1, 1, tzinfo=timezone.utc)
    else:
        since = (datetime.fromisoformat(last.replace("Z", "+00:00"))
                 - timedelta(days=safety_days))

    report = crawl_gallery(
        index, cfg, fetcher,
        {
            "published_since_year": str(since.year),
            "published_since_month": str(since.month),
            "published_since_day": str(since.day),
        },
        max_pages=max_pages,
    )
    index.set_sync_state(cfg.key, now_utc)
    return report
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_crawl.py -v`
Expected: PASS, 11 tests.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/db/discover/crawl.py tests/test_crawl.py
git commit -m "discover: polite crawler + incremental sync

Network sits behind a Fetcher protocol, so orchestration is tested offline against
committed fixtures. Politeness is fixed, not configurable-by-accident: 1 req/s with
jitter, descriptive UA with contact, backoff on 5xx, honors 429 Retry-After, and a
hard 8000-request per-run budget that raises rather than grinding on.

Two behaviors worth noting. Resume is real — a second crawl issues ZERO detail
requests for already-indexed ids, asserted in a test. And the drift guard counts only
genuine parse BREAKAGE, not absent data: a listing page full of artwork parses fine
and publishes no position, so it must not abort, while a page of 503s must. Getting
that distinction wrong would either mask a redesign or fire constantly.

Sync uses published_since from last-sync minus a 7-day safety window; now_utc is
injected so the window is testable."
```

---

### Task 8: Candidate ranking

**Files:**
- Create: `src/autocontrast/db/discover/discover.py`
- Test: `tests/test_discover_rank.py`

**Interfaces:**
- Consumes: `IndexMatch` (Task 6), `GalleryEntry` (Task 3).
- Produces: `RankedCandidate` dataclass (`entry`, `separation_arcmin`, `palette_compatible`,
  `framing_ratio`, `centering`); `rank_candidates(matches, *, query_radius_arcmin, user_palette_class) -> list[RankedCandidate]`.

**AUTHOR DECISION.** The ordering policy is a domain judgment: which professional render
makes the best style target for a given field. The default is palette compatibility first
(§2.3 flags rather than filters, so incompatible entries remain as lower-ranked fallbacks),
then framing aptness `|log(entry_radius / query_radius)|`, then centering, then `id` for
determinism. Reasonable alternatives: rank by the galleries' own `ranking` field, or prefer
larger pixel dimensions. Change the key if you prefer; the tests encode the default.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_discover_rank.py
"""Candidate ranking for the miss path."""

from __future__ import annotations

from autocontrast.db.discover.discover import rank_candidates
from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.discover.index import IndexMatch


def make(entry_id, *, radius=20.0, palette="RGB", sep=0.0):
    entry = GalleryEntry(
        id=entry_id, gallery="esa_hubble",
        detail_url=f"https://example.test/images/{entry_id}/",
        image_url=f"https://cdn.example.test/images/large/{entry_id}.jpg",
        ra_deg=83.8, dec_deg=-5.4, fov_w_arcmin=radius, fov_h_arcmin=radius,
        fov_radius_arcmin=radius, width_px=4000, height_px=4000,
        pixel_scale_arcsec=0.5, object_name="M 42", category="Nebulae",
        entry_type="Observation", palette_class=palette, license="CC BY 4.0",
        attribution="NASA, ESA", published_utc="2006-01-11T16:00:00Z", parsed_ok=True,
    )
    return IndexMatch(entry=entry, separation_arcmin=sep)


def test_palette_compatible_candidates_rank_first():
    """§2.3 flags rather than filters, so a mismatch is demoted, never dropped — it can
    still contribute structure and tone."""
    ranked = rank_candidates(
        [make("mismatch", palette="SHO"), make("match", palette="HOO")],
        query_radius_arcmin=20.0, user_palette_class="HOO",
    )
    assert [c.entry.id for c in ranked] == ["match", "mismatch"]
    assert ranked[0].palette_compatible is True
    assert ranked[1].palette_compatible is False


def test_incompatible_candidates_are_retained_not_discarded():
    ranked = rank_candidates([make("only", palette="SHO")],
                             query_radius_arcmin=20.0, user_palette_class="HOO")
    assert len(ranked) == 1


def test_closest_framing_wins_among_compatible_candidates():
    """A 20-arcmin field is better served by a similarly framed render than by a
    3-arcmin close-up or a 5-degree wide field."""
    ranked = rank_candidates(
        [make("closeup", radius=3.0), make("apt", radius=22.0), make("wide", radius=300.0)],
        query_radius_arcmin=20.0, user_palette_class="RGB",
    )
    assert ranked[0].entry.id == "apt"


def test_framing_ratio_is_symmetric_in_log_space():
    """Half the field and twice the field are equally mismatched."""
    ranked = {c.entry.id: c for c in rank_candidates(
        [make("half", radius=10.0), make("double", radius=40.0)],
        query_radius_arcmin=20.0, user_palette_class="RGB",
    )}
    assert ranked["half"].framing_ratio == ranked["double"].framing_ratio


def test_centering_breaks_ties_between_equally_framed_candidates():
    ranked = rank_candidates(
        [make("offset", radius=20.0, sep=15.0), make("centered", radius=20.0, sep=1.0)],
        query_radius_arcmin=20.0, user_palette_class="RGB",
    )
    assert [c.entry.id for c in ranked] == ["centered", "offset"]


def test_ranking_is_deterministic_for_identical_candidates():
    """Identical candidates must order by id, so runs are reproducible and testable."""
    ranked = rank_candidates([make("bbb"), make("aaa"), make("ccc")],
                             query_radius_arcmin=20.0, user_palette_class="RGB")
    assert [c.entry.id for c in ranked] == ["aaa", "bbb", "ccc"]


def test_unknown_user_palette_treats_nothing_as_compatible():
    """'unknown' means undetermined, so it must not be claimed to match anything —
    that would let a mismatched reference push chroma (§2.1)."""
    ranked = rank_candidates([make("rgb", palette="RGB")],
                             query_radius_arcmin=20.0, user_palette_class="unknown")
    assert ranked[0].palette_compatible is False


def test_empty_input_yields_empty_output():
    assert rank_candidates([], query_radius_arcmin=20.0, user_palette_class="RGB") == []
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_discover_rank.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'autocontrast.db.discover.discover'`

- [ ] **Step 3: Write the implementation**

```python
# src/autocontrast/db/discover/discover.py
"""Miss-path orchestration: cone search -> rank -> fetch -> verify -> ingest (§5.2).

Ordering and gating policy live here. The verification gate is safety-critical: because
auto-discovered records enter the store as immediately consensus-eligible
``professional_render`` (§5.4), this gate is the only barrier between a parser regression
and a poisoned reference set. It fails closed.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from .gallery import GalleryEntry
from .index import IndexMatch


@dataclass
class RankedCandidate:
    entry: GalleryEntry
    separation_arcmin: float
    palette_compatible: bool
    framing_ratio: float   # |log(entry_radius / query_radius)|; 0 is a perfect match
    centering: float       # separation as a fraction of the entry footprint radius


def rank_candidates(
    matches: list[IndexMatch],
    *,
    query_radius_arcmin: float,
    user_palette_class: str,
) -> list[RankedCandidate]:
    """Order overlapping candidates best-first.

    Policy, in precedence order:

    1. **Palette compatibility.** §2.3 flags rather than filters, so an incompatible
       render is demoted, never dropped — it can still contribute structure and tone.
       ``unknown`` on either side is never treated as compatible: it means undetermined,
       and claiming a match would let a mismatched reference push chroma (§2.1).
    2. **Framing aptness.** ``|log(ratio)|`` so that half the field and twice the field
       are equally penalised. Band-limiting (§4.4) absorbs residual scale differences,
       but a similarly framed render is a more apt style target than a close-up.
    3. **Centering.** How centrally the query sits inside the reference footprint.
    4. **Id**, so identical candidates order reproducibly.
    """
    ranked: list[RankedCandidate] = []
    for match in matches:
        entry = match.entry
        compatible = (
            user_palette_class == entry.palette_class
            and user_palette_class != "unknown"
            and entry.palette_class != "unknown"
        )
        radius = entry.fov_radius_arcmin or 0.0
        framing = (
            abs(math.log(radius / query_radius_arcmin))
            if radius > 0 and query_radius_arcmin > 0 else math.inf
        )
        centering = match.separation_arcmin / radius if radius > 0 else math.inf
        ranked.append(RankedCandidate(
            entry=entry, separation_arcmin=match.separation_arcmin,
            palette_compatible=compatible, framing_ratio=framing, centering=centering,
        ))

    ranked.sort(key=lambda c: (
        not c.palette_compatible, round(c.framing_ratio, 6), round(c.centering, 6), c.entry.id
    ))
    return ranked
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_discover_rank.py -v`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/db/discover/discover.py tests/test_discover_rank.py
git commit -m "discover: deterministic candidate ranking

Palette compatibility first (§2.3 demotes rather than drops, so an incompatible
render can still contribute structure and tone), then framing aptness as
|log(radius ratio)| so half-field and double-field are penalised equally, then
centering, then id for reproducibility.

'unknown' is never treated as compatible on either side — it means undetermined, and
claiming a match would let a mismatched reference push channel ratios (§2.1)."
```

---

### Task 9: The verification gate

**Files:**
- Modify: `src/autocontrast/db/discover/discover.py`
- Test: `tests/test_discover_gate.py`

**Interfaces:**
- Consumes: `RankedCandidate` (Task 8), `acquire_wcs`/`WcsResult` from `autocontrast.db.wcs`,
  `separation_arcmin`/`cones_overlap` (Task 1).
- Produces: `VerifyResult` dataclass (`accepted: bool`, `reason: str`, `wcs: WcsResult | None`,
  `pixel_scale_arcsec: float | None`); `verify_candidate(candidate, image_path, *, query_ra_deg, query_dec_deg, query_radius_arcmin, blind_solver=None) -> VerifyResult`;
  `position_tolerance_arcmin(fov_radius_arcmin) -> float`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_discover_gate.py
"""The fail-closed verification gate (G1-G6).

Auto-discovered records enter the store as immediately consensus-eligible
professional_render, so this gate is the ONLY barrier between a parser regression and a
poisoned reference set. Each rule is tested in isolation, not merely end-to-end.
"""

from __future__ import annotations

import pytest

from autocontrast.db.discover.discover import (
    RankedCandidate,
    position_tolerance_arcmin,
    verify_candidate,
)
from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.wcs import WcsResult

QUERY = {"query_ra_deg": 83.82, "query_dec_deg": -5.39, "query_radius_arcmin": 30.0}


def candidate(**overrides):
    defaults = dict(
        id="eso1103a", gallery="eso",
        detail_url="https://www.eso.org/public/images/eso1103a/",
        image_url="https://cdn.eso.org/images/large/eso1103a.jpg",
        ra_deg=83.82217, dec_deg=-5.39099, fov_w_arcmin=35.49, fov_h_arcmin=34.10,
        fov_radius_arcmin=24.61, width_px=8948, height_px=8597,
        pixel_scale_arcsec=0.238, object_name="M 42", category="Nebulae",
        entry_type="Observation", palette_class="RGB", license="CC BY 4.0",
        attribution="ESO/Igor Chekalin", published_utc="2011-01-19T12:00:00Z",
        parsed_ok=True,
    )
    defaults.update(overrides)
    entry = GalleryEntry(**defaults)
    return RankedCandidate(entry=entry, separation_arcmin=0.5, palette_compatible=True,
                           framing_ratio=0.2, centering=0.02)


def solver_returning(result):
    return lambda path: result


AVM_AGREEING = WcsResult(83.82217, -5.39099, 24.61, "avm", True, "WCS from embedded AVM tag",
                         pixel_scale_arcsec=0.238)
# The corrupt values pyavm's force-parse would yield for eso1103a.
AVM_CORRUPT = WcsResult(81.97, -3.74, 24.61, "avm", True, "WCS from embedded AVM tag",
                        pixel_scale_arcsec=0.238)
MANUAL = WcsResult(83.82217, -5.39099, 24.61, "manual", True, "WCS from manual annotation",
                   pixel_scale_arcsec=0.238)
UNSOLVED = WcsResult(None, None, None, "unsolved", False, "no tier produced a WCS")


def test_agreeing_avm_is_accepted(tmp_path):
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is True
    assert result.wcs.wcs_source == "avm"


def test_g2_rejects_a_corrupt_avm_that_disagrees_with_published_position(tmp_path):
    """REGRESSION: force-parsing eso1103a's AVM yields (81.97, -3.74) against its
    published (83.82, -5.39) — 2.3 degrees apart, roughly 5x tolerance. Two independent
    sources disagreeing means one is wrong and we cannot tell which, so reject rather
    than prefer either."""
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_CORRUPT))
    assert result.accepted is False
    assert "disagree" in result.reason.lower()


def test_g2_accepts_manual_fallback_but_marks_it_unverified(tmp_path):
    """eso1103a's AVM is unparseable, so acquire_wcs falls back to the published
    annotation. That is legitimate, but no independent cross-check happened."""
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(MANUAL))
    assert result.accepted is True
    assert result.wcs.wcs_source == "manual"


def test_g2_rejects_an_unsolved_image(tmp_path):
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(UNSOLVED))
    assert result.accepted is False
    assert "unsolved" in result.reason.lower() or "no wcs" in result.reason.lower()


def test_g3_rejects_when_the_solved_position_leaves_the_query_cone(tmp_path):
    """The solve is authoritative: if it lands outside the cone, the indexed position
    was wrong, whatever the index said."""
    far = WcsResult(200.0, 40.0, 5.0, "avm", True, "WCS from embedded AVM tag",
                    pixel_scale_arcsec=0.238)
    result = verify_candidate(candidate(ra_deg=200.0, dec_deg=40.0),
                              tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(far))
    assert result.accepted is False
    assert "cone" in result.reason.lower()


def test_g4_rejects_when_no_pixel_scale_can_be_established(tmp_path):
    """§2.2 forbids comparing scales in pixels, so a reference with no angular scale is
    unusable — band-limiting (§4.4) would be meaningless."""
    no_scale = WcsResult(83.82217, -5.39099, 24.61, "manual", True, "manual",
                         pixel_scale_arcsec=None)
    result = verify_candidate(candidate(pixel_scale_arcsec=None, fov_w_arcmin=None),
                              tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(no_scale))
    assert result.accepted is False
    assert "scale" in result.reason.lower()


def test_g4_uses_published_scale_when_the_solve_supplies_none(tmp_path):
    no_scale = WcsResult(83.82217, -5.39099, 24.61, "manual", True, "manual",
                         pixel_scale_arcsec=None)
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(no_scale))
    assert result.accepted is True
    assert result.pixel_scale_arcsec == pytest.approx(0.238, abs=1e-3)


def test_g4_prefers_the_solved_scale_over_the_published_one(tmp_path):
    """§2.2: a real solve must never be overridden by a nominal value."""
    solved = WcsResult(83.82217, -5.39099, 24.61, "avm", True, "avm",
                       pixel_scale_arcsec=0.240)
    result = verify_candidate(candidate(), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(solved))
    assert result.pixel_scale_arcsec == pytest.approx(0.240, abs=1e-4)


def test_g5_rejects_a_missing_license(tmp_path):
    """A copyright-asserting credit leaves the license unestablished; ingesting it as
    CC BY 4.0 would attach a false license to the record forever (§5.5)."""
    result = verify_candidate(candidate(license=None), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is False
    assert "license" in result.reason.lower()


def test_g5_rejects_a_missing_attribution(tmp_path):
    """The galleries state that crediting with the full line is mandatory; we cannot
    honor an obligation we did not capture."""
    result = verify_candidate(candidate(attribution=None), tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is False
    assert "attribution" in result.reason.lower()


def test_g1_rejects_a_candidate_whose_indexed_footprint_misses_the_cone(tmp_path):
    result = verify_candidate(candidate(ra_deg=200.0, dec_deg=40.0, fov_radius_arcmin=1.0),
                              tmp_path / "img.jpg", **QUERY,
                              wcs_acquirer=solver_returning(AVM_AGREEING))
    assert result.accepted is False


def test_tolerance_scales_with_footprint_but_has_a_floor():
    """A quarter of the footprint radius, never below one arcminute."""
    assert position_tolerance_arcmin(24.61) == pytest.approx(6.15, abs=0.01)
    assert position_tolerance_arcmin(0.5) == pytest.approx(1.0, abs=1e-9)


def test_tolerance_is_far_smaller_than_the_known_corruption():
    """The eso1103a force-parse error is ~140 arcmin against a ~6 arcmin tolerance, so
    the regression test above has a wide margin rather than a marginal pass."""
    from autocontrast.db.skymath import separation_arcmin
    error = separation_arcmin(83.82217, -5.39099, 81.97, -3.74)
    assert error > 10 * position_tolerance_arcmin(24.61)


def test_gate_never_raises_on_a_bad_candidate(tmp_path):
    """§12: degraded paths report, they do not except."""
    for bad in (candidate(license=None), candidate(attribution=None),
                candidate(pixel_scale_arcsec=None, fov_w_arcmin=None)):
        result = verify_candidate(bad, tmp_path / "img.jpg", **QUERY,
                                  wcs_acquirer=solver_returning(AVM_AGREEING))
        assert result.accepted is False
        assert result.reason
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_discover_gate.py -v`
Expected: FAIL — `ImportError: cannot import name 'verify_candidate'`

- [ ] **Step 3: Write the implementation**

Append to `src/autocontrast/db/discover/discover.py`:

```python
from pathlib import Path
from typing import Callable

from ..skymath import cones_overlap, separation_arcmin
from ..wcs import BlindSolver, WcsResult, acquire_wcs

# G2 tolerance: a quarter of the footprint radius, floored at one arcminute so tiny
# fields still get a workable window. For eso1103a (24.61') that is ~6.2', while the
# known AVM force-parse corruption is ~140' out — an order of magnitude of margin.
_TOLERANCE_FRACTION = 0.25
_TOLERANCE_FLOOR_ARCMIN = 1.0

WcsAcquirer = Callable[[Path], WcsResult]


def position_tolerance_arcmin(fov_radius_arcmin: float | None) -> float:
    """Allowed disagreement between a parsed AVM and the published position."""
    if not fov_radius_arcmin:
        return _TOLERANCE_FLOOR_ARCMIN
    return max(_TOLERANCE_FRACTION * fov_radius_arcmin, _TOLERANCE_FLOOR_ARCMIN)


@dataclass
class VerifyResult:
    accepted: bool
    reason: str
    wcs: WcsResult | None
    pixel_scale_arcsec: float | None


def verify_candidate(
    candidate: RankedCandidate,
    image_path: str | Path,
    *,
    query_ra_deg: float,
    query_dec_deg: float,
    query_radius_arcmin: float,
    blind_solver: BlindSolver | None = None,
    wcs_acquirer: WcsAcquirer | None = None,
) -> VerifyResult:
    """Run gates G1-G6 against a fetched candidate. Fails closed; never raises.

    ``wcs_acquirer`` is injectable so the gate is testable without real image files; it
    defaults to :func:`acquire_wcs` with the published position as the manual annotation.
    """
    entry = candidate.entry
    path = Path(image_path)

    # ---- G5: license and attribution (§5.5). Checked first: it needs no I/O, and a
    # reference we cannot attribute is unusable however good its position.
    if not entry.license:
        return VerifyResult(
            False,
            f"{entry.id}: license could not be established (credit asserts copyright, or "
            "the gallery default does not apply). Refusing to ingest under a license we "
            "cannot support (§5.5).",
            None, None,
        )
    if not entry.attribution:
        return VerifyResult(
            False,
            f"{entry.id}: no attribution captured; the galleries require the full credit "
            "line, and §5.5 makes it travel with every record.",
            None, None,
        )

    # ---- G1: the indexed footprint must overlap the query cone.
    if not entry.has_position:
        return VerifyResult(False, f"{entry.id}: no indexed position.", None, None)
    indexed_sep = separation_arcmin(query_ra_deg, query_dec_deg, entry.ra_deg, entry.dec_deg)
    if not cones_overlap(indexed_sep, query_radius_arcmin, entry.fov_radius_arcmin):
        return VerifyResult(
            False,
            f"{entry.id}: indexed footprint does not overlap the query cone "
            f"({indexed_sep:.1f}' apart).",
            None, None,
        )

    # ---- G2: independent WCS cross-check.
    if wcs_acquirer is None:
        manual = {
            "ra_deg": entry.ra_deg,
            "dec_deg": entry.dec_deg,
            "fov_radius_arcmin": entry.fov_radius_arcmin,
            "pixel_scale_arcsec": entry.pixel_scale_arcsec,
        }

        def wcs_acquirer(p: Path) -> WcsResult:
            return acquire_wcs(p, blind_solver=blind_solver, manual=manual)

    wcs = wcs_acquirer(path)
    if not wcs.solved:
        return VerifyResult(False, f"{entry.id}: unsolved — {wcs.detail}", wcs, None)

    if wcs.wcs_source == "avm":
        tolerance = position_tolerance_arcmin(entry.fov_radius_arcmin)
        drift = separation_arcmin(wcs.ra_deg, wcs.dec_deg, entry.ra_deg, entry.dec_deg)
        if drift > tolerance:
            return VerifyResult(
                False,
                f"{entry.id}: parsed AVM and published position disagree by "
                f"{drift:.1f}' (tolerance {tolerance:.1f}'). One of them is wrong and "
                "which is unknowable here, so the candidate is rejected rather than "
                "trusting either.",
                wcs, None,
            )

    # ---- G3: the solved position is authoritative and must still be in the cone.
    solved_sep = separation_arcmin(query_ra_deg, query_dec_deg, wcs.ra_deg, wcs.dec_deg)
    solved_radius = wcs.fov_radius_arcmin or entry.fov_radius_arcmin
    if not cones_overlap(solved_sep, query_radius_arcmin, solved_radius):
        return VerifyResult(
            False,
            f"{entry.id}: solved position falls outside the query cone "
            f"({solved_sep:.1f}' apart); the indexed position was wrong.",
            wcs, None,
        )

    # ---- G4: an angular pixel scale is mandatory (§2.2). A real solve wins over the
    # published nominal value.
    scale = wcs.pixel_scale_arcsec or entry.pixel_scale_arcsec
    if scale is None:
        return VerifyResult(
            False,
            f"{entry.id}: no pixel scale from the solve or from published metadata; "
            "§2.2 forbids comparing scales in pixels.",
            wcs, None,
        )

    # ---- G6: palette is derived, never guessed. 'unknown' is permitted — it forfeits
    # chroma via the §2.3 gate, which is the honest outcome, not a failure.
    verified = "cross-checked against published position" if wcs.wcs_source == "avm" \
        else f"accepted via {wcs.wcs_source} (no independent cross-check)"
    return VerifyResult(True, f"{entry.id}: {verified}.", wcs, float(scale))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_discover_gate.py -v`
Expected: PASS, 15 tests.

- [ ] **Step 5: Commit**

```bash
git add src/autocontrast/db/discover/discover.py tests/test_discover_gate.py
git commit -m "discover: fail-closed verification gate G1-G6

Discovered records enter the store as immediately consensus-eligible
professional_render, so this gate is the ONLY barrier between a parser regression and
a poisoned reference set. Every rule is tested in isolation, not just end-to-end.

G2 is the important one: when a parsed AVM disagrees with the published position
beyond tolerance, REJECT rather than preferring either — one is wrong and which is
unknowable from here. Regression-tested with the real corruption from
never-force-parse-avm: eso1103a force-parses to (81.97, -3.74) against published
(83.82, -5.39), ~140 arcmin out against a ~6 arcmin tolerance.

G5 runs first because it needs no I/O and a reference we cannot attribute is unusable
however good its position. G3 treats the solve as authoritative over the index. G4
prefers a solved scale over the published nominal one, per §2.2."
```

---

### Task 10: End-to-end miss path

**Files:**
- Modify: `src/autocontrast/db/discover/discover.py`, `src/autocontrast/db/discover/__init__.py`
- Test: `tests/test_discover_e2e.py`

**Interfaces:**
- Consumes: everything above; `FingerprintStore.cone_search`, `ingest_reference_auto`.
- Produces: `CandidateReport` dataclass (`id`, `accepted`, `reason`);
  `DiscoveryOutcome` dataclass (`ingested`, `record`, `considered: list[CandidateReport]`, `detail`);
  `AcquisitionOutcome` dataclass (`matches`, `discovered`, `detail`);
  `discover_reference(store, index, *, ra_deg, dec_deg, search_radius_arcmin, palette_class, fetcher, cache_dir, blind_solver=None, top_k=1) -> DiscoveryOutcome`;
  `acquire_reference(...same signature...) -> AcquisitionOutcome`.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_discover_e2e.py
"""The Phase 1 exit criterion as an automated test:
solve -> look up -> miss -> fetch -> fingerprint -> store, unattended."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from autocontrast.db.discover.discover import acquire_reference, discover_reference
from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.discover.index import GalleryIndex
from autocontrast.db.store import FingerprintStore
from autocontrast.db.wcs import WcsResult

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


@pytest.fixture
def synthetic_render(tmp_path):
    """A small RGB raster standing in for a fetched gallery JPEG. Real press-release
    files are hundreds of megapixels; the fingerprint math does not care about content
    here, only that a real image is loadable."""
    rng = np.random.default_rng(42)
    data = (rng.random((128, 128, 3)) * 200 + 20).astype(np.uint8)
    path = tmp_path / "render.jpg"
    Image.fromarray(data).save(path, quality=92)
    return path


class StubFetcher:
    def __init__(self, image_bytes: bytes):
        self.image_bytes = image_bytes
        self.requested: list[str] = []

    def get_text(self, url: str) -> str:
        self.requested.append(url)
        return ""

    def get_bytes(self, url: str, *, max_bytes: int) -> bytes:
        self.requested.append(url)
        return self.image_bytes


def indexed_entry(entry_id="eso1103a", palette="RGB", license_text="CC BY 4.0"):
    return GalleryEntry(
        id=entry_id, gallery="eso",
        detail_url="https://www.eso.org/public/images/eso1103a/",
        image_url="https://cdn.eso.org/images/large/eso1103a.jpg",
        ra_deg=83.82217, dec_deg=-5.39099, fov_w_arcmin=35.49, fov_h_arcmin=34.10,
        fov_radius_arcmin=24.61, width_px=8948, height_px=8597,
        pixel_scale_arcsec=0.238, object_name="M 42", category="Nebulae",
        entry_type="Observation", palette_class=palette, license=license_text,
        attribution="ESO/Igor Chekalin", published_utc="2011-01-19T12:00:00Z",
        parsed_ok=True,
    )


@pytest.fixture
def wiring(tmp_path, synthetic_render):
    store = FingerprintStore(tmp_path / "fingerprints.sqlite")
    index = GalleryIndex(tmp_path / "gallery_index.sqlite")
    index.upsert(indexed_entry())
    fetcher = StubFetcher(synthetic_render.read_bytes())
    yield store, index, fetcher, tmp_path / "cache"
    store.close()
    index.close()


def manual_wcs(path):
    return WcsResult(83.82217, -5.39099, 24.61, "manual", True,
                     "WCS from manual annotation", pixel_scale_arcsec=0.238)


def test_discovery_ingests_a_reference_on_a_miss(wiring):
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.ingested is True
    assert outcome.record.source_type == "professional_render"
    assert store.get(outcome.record.id) is not None


def test_stored_record_carries_license_attribution_and_wcs_source(wiring):
    """§5.5 requires the attribution and source URL to travel with the record."""
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    provenance = outcome.record.provenance
    assert provenance["license"] == "CC BY 4.0"
    assert provenance["attribution"] == "ESO/Igor Chekalin"
    assert provenance["source_url"] == "https://www.eso.org/public/images/eso1103a/"
    assert provenance["wcs_source"] == "manual"


def test_stored_position_and_palette_come_from_the_render_not_the_query(wiring):
    """The user's palette is used only for ranking; a discovered record's palette is
    always that render's own."""
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="HOO", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.record.palette_class == "RGB"
    assert outcome.record.ra_deg == pytest.approx(83.82217, abs=1e-4)


def test_downloaded_image_is_cached_and_not_refetched(wiring):
    store, index, fetcher, cache = wiring
    for _ in range(2):
        discover_reference(
            store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
            palette_class="RGB", fetcher=fetcher, cache_dir=cache,
            wcs_acquirer=manual_wcs,
        )
    downloads = [u for u in fetcher.requested if u.endswith(".jpg")]
    assert len(downloads) == 1


def test_no_candidate_in_the_index_reports_cleanly(wiring):
    """An empty result is a first-class branch, not an error (§5.2)."""
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=200.0, dec_deg=40.0, search_radius_arcmin=10.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.ingested is False
    assert outcome.considered == []
    assert "no candidate" in outcome.detail.lower()


def test_rejected_candidates_are_reported_with_reasons(wiring):
    """§12: every degraded path surfaces.

    The unlicensed entry is given an id that sorts first AND identical framing to the
    good one, so ranking's id tie-break puts it first and it is guaranteed to be
    considered — the assertion can never pass vacuously.
    """
    store, index, fetcher, cache = wiring
    index.upsert(indexed_entry(entry_id="aaa_unlicensed", license_text=None))
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
        top_k=1,
    )
    reports = {r.id: r for r in outcome.considered}
    assert "aaa_unlicensed" in reports, f"expected it to be considered; got {list(reports)}"
    assert reports["aaa_unlicensed"].accepted is False
    assert "license" in reports["aaa_unlicensed"].reason.lower()


def test_discovery_falls_through_to_the_next_candidate_after_a_rejection(wiring):
    """A rejected candidate must not abort discovery — the next one gets its turn."""
    store, index, fetcher, cache = wiring
    index.upsert(indexed_entry(entry_id="aaa_broken", license_text=None))
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.ingested is True
    assert outcome.record.id == "eso:eso1103a"
    rejected = {r.id: r for r in outcome.considered if not r.accepted}
    assert "aaa_broken" in rejected      # it was tried first, and rejected


def test_acquire_reference_returns_existing_matches_without_discovering(wiring):
    store, index, fetcher, cache = wiring
    first = acquire_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert first.discovered is not None and first.discovered.ingested is True

    fetcher.requested.clear()
    second = acquire_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert second.discovered is None          # already had it; no discovery attempted
    assert second.matches
    assert fetcher.requested == []


def test_the_full_exit_criterion_runs_unattended(wiring):
    """solve -> look up -> miss -> fetch -> fingerprint -> store, with no prompts."""
    store, index, fetcher, cache = wiring
    outcome = acquire_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.discovered.ingested is True
    stored = store.cone_search(83.82, -5.39, 30.0, palette_class="RGB")
    assert len(stored) == 1
    assert stored[0].palette_compatible is True
    assert stored[0].record.fingerprint is not None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_discover_e2e.py -v`
Expected: FAIL — `ImportError: cannot import name 'acquire_reference'`

- [ ] **Step 3: Write the implementation**

Append to `src/autocontrast/db/discover/discover.py`:

```python
from ..ingest import ingest_reference_auto
from ..records import ReferenceRecord
from ..store import ConeMatch, FingerprintStore
from .crawl import MAX_IMAGE_BYTES, Fetcher
from .index import GalleryIndex


@dataclass
class CandidateReport:
    """Why one candidate was accepted or rejected — §12's surfacing requirement."""

    id: str
    accepted: bool
    reason: str


@dataclass
class DiscoveryOutcome:
    ingested: bool
    record: ReferenceRecord | None
    considered: list[CandidateReport]
    detail: str


@dataclass
class AcquisitionOutcome:
    """``discovered`` is ``None`` when the store already held a match, so no discovery
    was attempted."""

    matches: list[ConeMatch]
    discovered: DiscoveryOutcome | None
    detail: str


def _cached_image(fetcher: Fetcher, url: str, cache_dir: Path, entry_id: str) -> Path:
    """Download ``url`` into ``cache_dir`` unless already present."""
    cache_dir.mkdir(parents=True, exist_ok=True)
    suffix = Path(url).suffix or ".jpg"
    path = cache_dir / f"{entry_id}{suffix}"
    if not path.exists():
        path.write_bytes(fetcher.get_bytes(url, max_bytes=MAX_IMAGE_BYTES))
    return path


def discover_reference(
    store: FingerprintStore,
    index: GalleryIndex,
    *,
    ra_deg: float,
    dec_deg: float,
    search_radius_arcmin: float,
    palette_class: str,
    fetcher: Fetcher,
    cache_dir: str | Path,
    blind_solver: BlindSolver | None = None,
    wcs_acquirer: WcsAcquirer | None = None,
    top_k: int = 1,
    psf_fwhm_arcsec: float = 2.0,
    n_scales: int = 7,
    max_dim: int | None = 1600,
) -> DiscoveryOutcome:
    """Cone-search the index, then fetch, verify, and ingest the best candidate(s).

    ``palette_class`` is the *user image's* palette, used only for §2.3 ranking. A
    discovered record always stores the render's own derived palette.

    ``top_k`` defaults to 1, matching §5.4: seed a position with one best professional
    reference, and let consensus arrive as more accumulate.
    """
    cache = Path(cache_dir)
    matches = index.cone_search(ra_deg, dec_deg, search_radius_arcmin)
    if not matches:
        return DiscoveryOutcome(
            False, None, [],
            f"No candidate in the gallery index covers ({ra_deg:.4f}, {dec_deg:.4f}) "
            f"within {search_radius_arcmin:.1f}'. Sync the index, or add a curated "
            "catalog entry.",
        )

    ranked = rank_candidates(
        matches, query_radius_arcmin=search_radius_arcmin, user_palette_class=palette_class
    )

    reports: list[CandidateReport] = []
    ingested: ReferenceRecord | None = None

    for candidate in ranked:
        entry = candidate.entry
        if not entry.image_url:
            reports.append(CandidateReport(entry.id, False, "no CDN image URL published"))
            continue
        try:
            image_path = _cached_image(fetcher, entry.image_url, cache, entry.id)
        except Exception as exc:  # network/size failure for THIS candidate only
            reports.append(CandidateReport(entry.id, False, f"download failed: {exc}"))
            continue

        verdict = verify_candidate(
            candidate, image_path,
            query_ra_deg=ra_deg, query_dec_deg=dec_deg,
            query_radius_arcmin=search_radius_arcmin,
            blind_solver=blind_solver, wcs_acquirer=wcs_acquirer,
        )
        reports.append(CandidateReport(entry.id, verdict.accepted, verdict.reason))
        if not verdict.accepted:
            continue

        outcome = ingest_reference_auto(
            store, image_path,
            psf_fwhm_arcsec=psf_fwhm_arcsec,
            palette_class=entry.palette_class,
            source_type="professional_render",
            provenance={
                "source_url": entry.detail_url,
                "license": entry.license,
                "attribution": entry.attribution,
                "gallery": entry.gallery,
                "discovered": True,
            },
            manual={
                "ra_deg": entry.ra_deg,
                "dec_deg": entry.dec_deg,
                "fov_radius_arcmin": entry.fov_radius_arcmin,
                "pixel_scale_arcsec": verdict.pixel_scale_arcsec,
            },
            pixel_scale_arcsec=verdict.pixel_scale_arcsec,
            id=f"{entry.gallery}:{entry.id}",
            n_scales=n_scales, max_dim=max_dim,
        )
        if not outcome.ingested:
            reports[-1] = CandidateReport(entry.id, False, f"ingest declined: {outcome.detail}")
            continue

        ingested = outcome.record
        if sum(1 for r in reports if r.accepted) >= top_k:
            break

    if ingested is None:
        return DiscoveryOutcome(
            False, None, reports,
            f"{len(ranked)} candidate(s) considered, none usable: "
            + "; ".join(f"{r.id} ({r.reason})" for r in reports),
        )
    return DiscoveryOutcome(
        True, ingested, reports,
        f"Ingested {ingested.id} from the {ingested.provenance['gallery']} gallery.",
    )


def acquire_reference(
    store: FingerprintStore,
    index: GalleryIndex,
    *,
    ra_deg: float,
    dec_deg: float,
    search_radius_arcmin: float,
    palette_class: str,
    fetcher: Fetcher,
    cache_dir: str | Path,
    blind_solver: BlindSolver | None = None,
    wcs_acquirer: WcsAcquirer | None = None,
    top_k: int = 1,
    psf_fwhm_arcsec: float = 2.0,
    n_scales: int = 7,
    max_dim: int | None = 1600,
) -> AcquisitionOutcome:
    """The Phase 1 exit criterion in one call: look up, and on a miss discover.

    Returns existing matches untouched when the store already covers the position — no
    network, no discovery.
    """
    matches = store.cone_search(ra_deg, dec_deg, search_radius_arcmin, palette_class)
    if matches:
        return AcquisitionOutcome(
            matches, None,
            f"{len(matches)} reference(s) already stored for this position.",
        )

    discovered = discover_reference(
        store, index, ra_deg=ra_deg, dec_deg=dec_deg,
        search_radius_arcmin=search_radius_arcmin, palette_class=palette_class,
        fetcher=fetcher, cache_dir=cache_dir, blind_solver=blind_solver,
        wcs_acquirer=wcs_acquirer, top_k=top_k, psf_fwhm_arcsec=psf_fwhm_arcsec,
        n_scales=n_scales, max_dim=max_dim,
    )
    return AcquisitionOutcome(
        store.cone_search(ra_deg, dec_deg, search_radius_arcmin, palette_class),
        discovered,
        discovered.detail,
    )
```

Then export the public surface:

```python
# src/autocontrast/db/discover/__init__.py
"""Archive discovery (§5.2.1): local positional index over professional gallery renders."""

from __future__ import annotations

from .crawl import CrawlReport, DriftError, PoliteFetcher, crawl_gallery, sync_gallery
from .discover import (
    AcquisitionOutcome,
    DiscoveryOutcome,
    acquire_reference,
    discover_reference,
)
from .gallery import GALLERIES, GalleryEntry, GalleryConfig
from .index import GalleryIndex

__all__ = [
    "GALLERIES", "GalleryConfig", "GalleryEntry", "GalleryIndex",
    "PoliteFetcher", "CrawlReport", "DriftError", "crawl_gallery", "sync_gallery",
    "AcquisitionOutcome", "DiscoveryOutcome", "acquire_reference", "discover_reference",
]
```

- [ ] **Step 4: Run test to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_discover_e2e.py -v`
Expected: PASS, 9 tests.

- [ ] **Step 5: Run the full suite**

Run: `.venv/bin/python -m ruff check src tests && .venv/bin/python -m pytest -q`
Expected: no lint findings; all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/autocontrast/db/discover tests/test_discover_e2e.py
git commit -m "discover: end-to-end miss path — Phase 1 exit criterion automated

acquire_reference() is the criterion in one call: look up, and on a miss discover ->
fetch -> fingerprint -> store, unattended. When the store already covers the position
it returns existing matches and touches no network, asserted in a test.

Rejected candidates fall through to the next rather than aborting, and every
consideration is reported with a reason (§12). Records carry source_url, license,
attribution, and wcs_source so §5.5's obligation travels with the fingerprint.

A discovered record's palette is always the render's OWN derived palette; the caller's
palette_class is used only for §2.3 ranking."
```

---

### Task 11: CLI entry point and live drift check

**Files:**
- Create: `src/autocontrast/db/discover/__main__.py`, `tests/test_gallery_live.py`
- Modify: `pyproject.toml` (register the `live` marker)
- Test: as above

**Interfaces:**
- Consumes: everything above.
- Produces: `python -m autocontrast.db.discover sync [--gallery KEY] [--index PATH] [--max-pages N]`.

- [ ] **Step 1: Register the marker so `-m live` works and unmarked runs stay offline**

In `pyproject.toml`, replace the `[tool.pytest.ini_options]` block with:

```toml
[tool.pytest.ini_options]
testpaths = ["tests"]
addopts = "-ra -m 'not live'"
markers = [
    "live: hits the real ESA/Hubble and ESO sites; deselected by default. Run with -m live.",
]
```

- [ ] **Step 2: Write the live drift test**

```python
# tests/test_gallery_live.py
"""Opt-in drift detector. Deselected by default; run deliberately:

    .venv/bin/python -m pytest tests/test_gallery_live.py -m live -v

Its only job is to tell you whether the committed fixtures still reflect reality. If it
fails, re-run tools/capture_gallery_fixtures.sh and fix the parsers — do NOT relax the
assertions, which would hide the drift the crawler's DriftError exists to catch.
"""

from __future__ import annotations

import pytest

from autocontrast.db.discover.crawl import PoliteFetcher
from autocontrast.db.discover.gallery import GALLERIES, detail_url, parse_detail

pytestmark = pytest.mark.live


@pytest.mark.parametrize(
    "gallery, entry_id, ra, dec, radius",
    [
        ("esa_hubble", "heic0601a", 83.7905, -5.4140, 21.23),
        ("eso", "eso1103a", 83.82217, -5.39099, 24.61),
    ],
)
def test_live_detail_pages_still_publish_position_and_field_of_view(
    gallery, entry_id, ra, dec, radius
):
    cfg = GALLERIES[gallery]
    url = detail_url(cfg, entry_id)
    entry = parse_detail(
        PoliteFetcher().get_text(url), entry_id=entry_id, gallery=gallery, detail_url=url
    )
    assert entry.parsed_ok is True, f"{entry_id}: detail page no longer parses"
    assert entry.ra_deg == pytest.approx(ra, abs=1e-3)
    assert entry.dec_deg == pytest.approx(dec, abs=1e-3)
    assert entry.fov_radius_arcmin == pytest.approx(radius, abs=0.05)
    assert entry.attribution, f"{entry_id}: credit block no longer found (gate G5 blocker)"
    assert entry.image_url, f"{entry_id}: CDN image URL no longer found"
```

- [ ] **Step 3: Confirm the default run excludes it and the opt-in run includes it**

Run: `.venv/bin/python -m pytest -q`
Expected: PASS with the live tests **deselected** (the summary shows `2 deselected`).

Run: `.venv/bin/python -m pytest tests/test_gallery_live.py -m live -v`
Expected: PASS against the real sites (needs network; takes a few seconds due to the
1 req/s throttle).

- [ ] **Step 4: Write the CLI**

```python
# src/autocontrast/db/discover/__main__.py
"""Crawl or sync the gallery position index.

    python -m autocontrast.db.discover sync
    python -m autocontrast.db.discover sync --gallery eso --max-pages 3

A cold run enumerates thousands of detail pages at one request per second, so expect it
to take a while. It is resumable: re-running skips everything already indexed.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from pathlib import Path

from .crawl import DriftError, PoliteFetcher, sync_gallery
from .gallery import GALLERIES
from .index import GalleryIndex

DEFAULT_INDEX = Path("data/gallery_index.sqlite")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m autocontrast.db.discover")
    parser.add_argument("command", choices=["sync"])
    parser.add_argument("--gallery", choices=sorted(GALLERIES), action="append",
                        help="gallery to sync; repeatable. Default: all.")
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--max-pages", type=int, default=None,
                        help="stop after N listing pages (useful for a smoke run)")
    args = parser.parse_args(argv)

    args.index.parent.mkdir(parents=True, exist_ok=True)
    index = GalleryIndex(args.index)
    fetcher = PoliteFetcher()
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    status = 0
    try:
        for key in (args.gallery or sorted(GALLERIES)):
            try:
                report = sync_gallery(index, GALLERIES[key], fetcher, now_utc=now,
                                      max_pages=args.max_pages)
                print(report.detail)
            except DriftError as exc:
                # Loud, and non-zero exit: a redesign must not look like a quiet no-op.
                print(f"DRIFT: {exc}")
                status = 1
        print(f"index now holds {index.count()} entries at {args.index}")
    finally:
        index.close()
    return status


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 5: Smoke-test the CLI against the real sites, bounded to one page**

Run: `.venv/bin/python -m autocontrast.db.discover sync --gallery eso --max-pages 1 --index /tmp/smoke_index.sqlite`
Expected: prints an `eso: listed N, indexed N, ...` line and a final index count > 0.
This makes real requests at 1/s; one page is ~50 detail fetches, so allow ~1 minute.

- [ ] **Step 6: Add the index to `.gitignore`**

The index is a regenerable cache, not source. Append to `.gitignore`:

```
# Regenerable gallery position index (rebuild with `python -m autocontrast.db.discover sync`)
data/gallery_index.sqlite
```

- [ ] **Step 7: Final full-suite check and lint**

Run: `.venv/bin/python -m ruff check src tests && .venv/bin/python -m pytest -q`
Expected: no lint findings; all tests pass, live tests deselected.

- [ ] **Step 8: Commit**

```bash
git add src/autocontrast/db/discover/__main__.py tests/test_gallery_live.py \
        pyproject.toml .gitignore
git commit -m "discover: sync CLI + opt-in live drift check

'python -m autocontrast.db.discover sync' crawls or incrementally syncs the index,
resumable and polite. DriftError exits non-zero so a gallery redesign cannot look
like a quiet no-op.

The live test is marked and deselected by default (addopts now carries -m 'not live'),
so the suite stays offline and deterministic while drift remains detectable on demand.
It asserts the two known references still parse to their catalog positions.

data/gallery_index.sqlite is gitignored — regenerable cache, not source."
```

---

## Self-Review

**1. Spec coverage.**

| Spec section | Task |
|---|---|
| §4 architecture, four modules, pure/impure split | 2, 3, 5, 6, 7, 8–10 |
| §4 `skymath.py` refactor | 1 |
| §5 index schema, separate file, NULL rows | 6 |
| §5 palette from published filters | 4 |
| §5 crawl, resume, sync, politeness, drift abort | 7 |
| §6 miss path, local cone search, ranking, fetch/cache, ingest, outcome reporting | 8, 10 |
| §6 `top_k` default 1 | 10 |
| §7 gates G1–G6, tolerance, `eso1103a` regression | 9 |
| §7 raises vs. reports | 9, 10 |
| §8 six fixtures, index/gate/crawl/e2e tests, opt-in live test | 3, 6, 7, 9, 10, 11 |
| §8 RA-wrap coverage | 6 |

**2. Deviations from the spec, discovered while verifying against live pages.** All three
make the implementation stronger; the spec should be amended to match.

- **Listing is a JS data literal**, not markup, and it publishes `width`/`height`. The spec
  assumed dimensions came from the downloaded file. Consequence: the index gains
  `width_px`, `height_px`, `pixel_scale_arcsec`, and G4 is satisfiable without a download.
- **G5 gains a copyright sub-rule.** ESA/Hubble hosts third-party copyrighted renders
  (`opo0205c` is AAO's) and neither site publishes a per-image machine-readable license.
  The per-gallery CC BY 4.0 default therefore applies only when the credit does not assert
  copyright; otherwise the license is left unestablished and G5 rejects.
- **Sync is driven by a `sync_state` table**, not by the maximum indexed `published_utc`.
  More robust — it does not depend on parsing a release date — and `now_utc` is injected so
  the window is testable. `published_utc` is still captured.

Also: the spec's fixture list named `hubble_detail_illustration.html`; the concrete fixture
is `hubble_detail_heic0211i.html` (`Type: Artwork`).

**3. Placeholder scan.** No TBD/TODO. Every code step carries complete code. The two
AUTHOR DECISION callouts (Task 4 palette mapping, Task 8 ranking policy) ship working
defaults with tests, so the plan is executable as written; they mark where domain judgment
should be applied, not gaps.

**4. Type consistency.** `GalleryEntry` field names are used identically in `gallery.py`,
`index.py` (`_COLUMNS` is derived from `dataclasses.fields`, so they cannot drift),
`discover.py`, and every test. `separation_arcmin` is the public name everywhere after
Task 1. `wcs_acquirer` appears with the same signature in Tasks 9 and 10.
`fov_radius_arcmin` is the half-diagonal in `gallery.py`, `index.py`, `WcsResult`, and
`ReferenceRecord` alike.

**5. Task ordering caveat.** Task 3's `parse_detail` imports
`palette_class_from_gallery_bands` from Task 4. **Execute Task 4 before Task 3's Step 5**,
or add a temporary stub as noted in Task 3 Step 4.
