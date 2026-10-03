### Task 1: Project scaffolding, config, and database schema

**Files:**
- Create: `pyproject.toml`
- Create: `src/astrometa/__init__.py`
- Create: `src/astrometa/config.py`
- Create: `src/astrometa/db.py`
- Test: `tests/test_db.py`

**Interfaces:**
- Consumes: nothing
- Produces: `db.connect(path: Path) -> sqlite3.Connection`, `db.init_schema(conn: sqlite3.Connection) -> None`, `config.Config` dataclass with fields `archive_root: Path`, `live_root: Path`, `db_path: Path`, `scratch_dir: Path`, `astap_bin: Path`, `astap_db_dir: Path`

- [ ] **Step 1: Write the failing test**

```python
# tests/test_db.py
import sqlite3
from astrometa import db

def test_init_schema_creates_expected_tables(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite")
    db.init_schema(conn)
    names = {r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"frames", "fields", "objects", "aliases",
            "identity_assertions", "quality", "projects",
            "frame_projects"} <= names

def test_init_schema_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite")
    db.init_schema(conn)
    db.init_schema(conn)          # must not raise
    conn.execute("INSERT INTO frames (content_hash, path, filename, size, mtime, "
                 "frame_type, disposition) VALUES ('h1','/a/b.fit','b.fit',1,1.0,"
                 "'light','present')")
    db.init_schema(conn)
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 1

def test_frames_primary_key_is_content_hash(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite")
    db.init_schema(conn)
    conn.execute("INSERT INTO frames (content_hash, path, filename, size, mtime, "
                 "frame_type, disposition) VALUES ('h1','/a/b.fit','b.fit',1,1.0,"
                 "'light','present')")
    try:
        conn.execute("INSERT INTO frames (content_hash, path, filename, size, mtime, "
                     "frame_type, disposition) VALUES ('h1','/c/d.fit','d.fit',1,1.0,"
                     "'light','present')")
        raise AssertionError("duplicate content_hash must be rejected")
    except sqlite3.IntegrityError:
        pass
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_db.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa'`

- [ ] **Step 3: Write minimal implementation**

```toml
# pyproject.toml
[project]
name = "astrometa"
version = "0.1.0"
requires-python = ">=3.13"
dependencies = ["numpy>=2.0", "astropy>=7.0", "astroquery>=0.4.7"]

[build-system]
requires = ["setuptools>=68"]
build-backend = "setuptools.build_meta"

[tool.setuptools.packages.find]
where = ["src"]

[tool.pytest.ini_options]
testpaths = ["tests"]
```

```python
# src/astrometa/config.py
from dataclasses import dataclass
from pathlib import Path

@dataclass(frozen=True)
class Config:
    archive_root: Path = Path("/archive/astro_data")
    live_root: Path = Path("/live/astro_data")
    db_path: Path = Path("/data/astro-metadata/store.sqlite")
    scratch_dir: Path = Path("/var/tmp/astrometa")
    astap_bin: Path = Path("/opt/astap/astap_cli")
    astap_db_dir: Path = Path("/opt/astap")

EXCLUDED_PATH_MARKERS = ("_dedup_quarantine", "@Recycle")
FINGERPRINT_SIZE = 16          # 16x16 grid -> 240-bit dHash
```

```python
# src/astrometa/db.py
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pip install -e . && pytest tests/test_db.py -v`
Expected: PASS, 3 tests

- [ ] **Step 5: Commit**

```bash
git add pyproject.toml src/astrometa/ tests/test_db.py
git commit -m "feat: project scaffolding, config, and database schema"
```

---

