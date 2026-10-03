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
