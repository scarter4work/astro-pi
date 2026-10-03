import sqlite3
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS frames (
  content_hash TEXT PRIMARY KEY,
  path TEXT NOT NULL, filename TEXT NOT NULL,
  size INTEGER NOT NULL, mtime REAL NOT NULL,
  first_seen TEXT, last_seen TEXT,
  frame_type TEXT NOT NULL,
  camera TEXT, filter TEXT, exptime REAL, binning INTEGER,
  gain REAL, ccd_temp REAL, captured_at TEXT,
  -- The OBJECT card as written in the header, and the name of the
  -- directory the frame sits in. Neither is an identity claim: both
  -- are the operator's own labelling, kept verbatim so spec 6.7 can
  -- cross-check a solved identity against them without a second
  -- 34,000-file header read, and so grouping has a per-frame target
  -- name to fall back on while object_id is still unresolved.
  object_card TEXT, leaf_dir TEXT,
  header_ra REAL, header_dec REAL, focallen REAL, xpixsz REAL,
  naxis1 INTEGER, naxis2 INTEGER,
  fingerprint TEXT, bg_median REAL, saturated_frac REAL,
  field_id INTEGER REFERENCES fields(id),
  -- These three values are a contract across inventory, disposition,
  -- manifest and quality, and inventory's rescan guard compares a
  -- bare literal (CASE WHEN disposition='quarantined'). A typo
  -- anywhere would silently defeat that guard rather than fail.
  disposition TEXT NOT NULL
    CHECK (disposition IN ('present', 'missing', 'quarantined')),
  disposition_reason TEXT, disposition_at TEXT, disposition_source TEXT,
  read_error TEXT
);
CREATE INDEX IF NOT EXISTS idx_frames_path ON frames(path);
CREATE INDEX IF NOT EXISTS idx_frames_field ON frames(field_id);
CREATE INDEX IF NOT EXISTS idx_frames_type ON frames(frame_type);

CREATE TABLE IF NOT EXISTS fields (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  solved_ra REAL, solved_dec REAL,
  fov_w REAL, fov_h REAL, rotation REAL, scale_arcsec_px REAL,
  solve_source TEXT NOT NULL DEFAULT 'none',
  solve_at TEXT, quads_matched INTEGER, solve_attempts INTEGER DEFAULT 0,
  solve_error TEXT,
  object_id INTEGER REFERENCES objects(id)
);

CREATE TABLE IF NOT EXISTS objects (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  canonical_name TEXT NOT NULL UNIQUE,
  object_type TEXT NOT NULL,
  simbad_id TEXT, ra REAL, dec REAL
);

CREATE TABLE IF NOT EXISTS aliases (
  object_id INTEGER NOT NULL REFERENCES objects(id),
  alias TEXT NOT NULL, source TEXT NOT NULL,
  PRIMARY KEY (object_id, alias)
);

CREATE TABLE IF NOT EXISTS identity_assertions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  field_id INTEGER NOT NULL REFERENCES fields(id),
  object_id INTEGER NOT NULL REFERENCES objects(id),
  source TEXT NOT NULL, confidence TEXT NOT NULL, asserted_at TEXT NOT NULL
);

-- Historical culls recorded independently of any frame row.
--
-- The 39 IC 59 frames culled on 2026-09-07 exist on neither the live
-- NAS nor the ZFS backup (verified 2026-09-09: both hold 91 frames,
-- HaO3 indices stopping at 0018 while the culled range is 0019-0040).
-- They survive only on the camera, which is not one of the store's
-- roots, so there is no frames row to carry their disposition and there
-- may never be one. Fabricating a frames row with an invented
-- content_hash would corrupt the store's central identity claim -- the
-- primary key means "the bytes of this file" -- so the knowledge lives
-- here instead, keyed on the cull pattern.
CREATE TABLE IF NOT EXISTS known_culls (
  pattern TEXT PRIMARY KEY,
  reason TEXT NOT NULL,
  recorded_at TEXT NOT NULL,
  matched INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS quality (
  content_hash TEXT PRIMARY KEY REFERENCES frames(content_hash),
  star_count INTEGER, hfd_median REAL, sky_background REAL, measured_at TEXT
);

CREATE TABLE IF NOT EXISTS projects (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  object_id INTEGER REFERENCES objects(id),
  filter TEXT, kind TEXT NOT NULL,
  started_at TEXT, ended_at TEXT,
  -- How this project's target was identified, and under what name.
  -- object_id is authoritative when set; until it is, grouping falls
  -- back to a per-frame identity and records which one it used, so a
  -- fallback label is never mistaken for a solved one. Mirrors the
  -- confidence model in spec 7: 'object' (authoritative), 'object_card'
  -- (medium), 'dirname' (low), 'unknown'. identity_name is NULL when
  -- object_id carries the identity, and when there was none to find.
  identity_name TEXT, identity_source TEXT
);

CREATE TABLE IF NOT EXISTS frame_projects (
  content_hash TEXT NOT NULL REFERENCES frames(content_hash),
  project_id INTEGER NOT NULL REFERENCES projects(id),
  panel TEXT,
  PRIMARY KEY (content_hash, project_id)
);
"""

def connect(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(path)
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA journal_mode = WAL")
    return conn

def init_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(SCHEMA)
    conn.commit()
