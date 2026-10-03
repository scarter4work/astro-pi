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

