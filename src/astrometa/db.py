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
  header_ra REAL, header_dec REAL, focallen REAL, xpixsz REAL,
  naxis1 INTEGER, naxis2 INTEGER,
  fingerprint TEXT, bg_median REAL, saturated_frac REAL,
  field_id INTEGER REFERENCES fields(id),
  disposition TEXT NOT NULL,
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

CREATE TABLE IF NOT EXISTS quality (
  content_hash TEXT PRIMARY KEY REFERENCES frames(content_hash),
  star_count INTEGER, hfd_median REAL, sky_background REAL, measured_at TEXT
);

CREATE TABLE IF NOT EXISTS projects (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  object_id INTEGER REFERENCES objects(id),
  filter TEXT, kind TEXT NOT NULL,
  started_at TEXT, ended_at TEXT
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
