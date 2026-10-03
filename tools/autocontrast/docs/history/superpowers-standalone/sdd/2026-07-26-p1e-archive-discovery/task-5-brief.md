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

