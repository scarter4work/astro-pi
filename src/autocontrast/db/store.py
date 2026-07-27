"""SQLite fingerprint store (design §5, §9.3 — SQLite locally).

Stores §4.1 reference records. The full record is kept as a JSON blob; the fields
needed to query (position, footprint radius, palette, source type) are mirrored
into indexed columns so cone search (§5.1) never has to parse JSON to filter.

Provenance is enforced at ingest: ``tool_output`` is rejected outright (§5.3, the
autophagy guard) — never stored, never consensused.
"""

from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from autocontrast.fingerprint.palette import palette_chroma_compatible

from .records import SOURCE_TYPES, ReferenceRecord
from .skymath import separation_arcmin


@dataclass
class ConeMatch:
    """A reference whose footprint overlaps the query cone (§5.1)."""

    record: ReferenceRecord
    separation_arcmin: float
    palette_compatible: bool  # False => contributes structural+tonal only (§2.3)

_SCHEMA = """
CREATE TABLE IF NOT EXISTS fingerprints (
    id                 TEXT PRIMARY KEY,
    ra_deg             REAL NOT NULL,
    dec_deg            REAL NOT NULL,
    fov_radius_arcmin  REAL NOT NULL,
    palette_class      TEXT NOT NULL,
    source_type        TEXT NOT NULL,
    record_json        TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fp_dec ON fingerprints(dec_deg);
CREATE INDEX IF NOT EXISTS idx_fp_palette ON fingerprints(palette_class);
"""


class FingerprintStore:
    """A local SQLite cache of reference fingerprints."""

    def __init__(self, db_path: str | Path):
        self.db_path = str(db_path)
        self._conn = sqlite3.connect(self.db_path)
        self._conn.row_factory = sqlite3.Row
        self._conn.executescript(_SCHEMA)
        self._conn.commit()

    def store(self, record: ReferenceRecord) -> None:
        """Insert or replace a reference record.

        Rejects ``tool_output`` and any unknown source type before touching the DB
        (§5.3) — the guard is loud, never a silent skip (§12)."""
        if record.source_type not in SOURCE_TYPES:
            raise ValueError(f"Unknown source_type {record.source_type!r}; expected {SOURCE_TYPES}")
        if record.source_type == "tool_output":
            raise ValueError(
                "Refusing to store source_type='tool_output' (§5.3 autophagy guard): "
                "the tool's own output must never enter the reference set."
            )
        self._conn.execute(
            "INSERT OR REPLACE INTO fingerprints "
            "(id, ra_deg, dec_deg, fov_radius_arcmin, palette_class, source_type, record_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (
                record.id, record.ra_deg, record.dec_deg, record.fov_radius_arcmin,
                record.palette_class, record.source_type, json.dumps(record.to_dict()),
            ),
        )
        self._conn.commit()

    def get(self, record_id: str) -> ReferenceRecord | None:
        row = self._conn.execute(
            "SELECT record_json FROM fingerprints WHERE id = ?", (record_id,)
        ).fetchone()
        if row is None:
            return None
        return ReferenceRecord.from_dict(json.loads(row["record_json"]))

    def cone_search(
        self,
        ra_deg: float,
        dec_deg: float,
        radius_arcmin: float,
        palette_class: str | None = None,
        source_types: set[str] | None = None,
    ) -> list[ConeMatch]:
        """Reference records whose footprint cone overlaps the query cone (§5.1).

        Two cones overlap when their center separation is at most the sum of their
        radii: ``sep <= radius_arcmin + record.fov_radius_arcmin``. Results are
        ordered nearest-first.

        A palette mismatch does **not** exclude a record — §2.3 lets it contribute
        structural + tonal components, so the mismatch is reported via
        ``palette_compatible`` rather than filtered out. ``source_types`` optionally
        restricts to e.g. consensus-eligible professional renders.
        """
        # Necessary condition for overlap: |Δdec| <= (radius + fov). Since
        # separation >= |Δdec|, this dec-band prefilter is a safe superset.
        clauses = ["ABS(dec_deg - ?) <= (? + fov_radius_arcmin) / 60.0"]
        params: list = [dec_deg, radius_arcmin]
        if source_types is not None:
            clauses.append(f"source_type IN ({','.join('?' * len(source_types))})")
            params.extend(sorted(source_types))
        sql = "SELECT record_json, ra_deg, dec_deg, fov_radius_arcmin FROM fingerprints WHERE " \
              + " AND ".join(clauses)

        matches: list[ConeMatch] = []
        for row in self._conn.execute(sql, params):
            sep = separation_arcmin(ra_deg, dec_deg, row["ra_deg"], row["dec_deg"])
            if sep > radius_arcmin + row["fov_radius_arcmin"]:
                continue  # precise reject after the dec-band prefilter
            record = ReferenceRecord.from_dict(json.loads(row["record_json"]))
            compatible = palette_class is None or palette_chroma_compatible(
                record.palette_class, palette_class
            )
            matches.append(ConeMatch(record=record, separation_arcmin=sep,
                                     palette_compatible=compatible))

        matches.sort(key=lambda m: m.separation_arcmin)
        return matches

    def close(self) -> None:
        self._conn.close()
