# Astro Metadata Store Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a metadata store over the ~28,000-frame astrophotography archive that records what each frame is of, whether it is good, and what it belongs to — with provenance strong enough to eventually drive a physical reorganisation.

**Architecture:** A set of independently resumable passes over the archive, all writing to one SQLite database on scott-server's local ZFS. Each pass is idempotent on a frame's content hash. Identity comes from plate-solved star geometry (exact) rather than filenames or directory names (unreliable); a perceptual fingerprint clusters frames by field so only one frame per distinct field needs solving. Nothing is ever moved, renamed, or deleted by this code.

**Tech Stack:** Python 3.13, SQLite (stdlib `sqlite3`), NumPy, Astropy (FITS pixel reads), Astroquery (SIMBAD), ASTAP `astap_cli` + D50 database.

**Spec:** `docs/superpowers/specs/2026-09-09-astro-metadata-store-design.md`

## Global Constraints

- **Never write to the archive.** No moves, renames, deletions, or FITS header modification. The only writes this code performs are to the SQLite DB, to local scratch, and to `.astro-manifest.json` files.
- **Never delete by machine.** Culling moves frames to a sibling `rejected/` directory. That move is the *only* archive-mutating operation in the entire system and it lives in exactly one function (Task 12).
- **Source paths are read-only.** `/archive` (ZFS copy) and `/live` (CIFS) are read-only bind mounts. `astap_cli` writes sidecars next to its input, so every solve and every analyse copies its frame to local scratch first.
- **Exclude** any path containing `_dedup_quarantine` or `@Recycle`.
- **Every pass is idempotent** and keyed on content hash. Running any pass twice must not create duplicate rows or change results.
- **Errors are loud.** No silent fallbacks, no defaulted values on failure. A frame that cannot be read is recorded as failed with the reason, never skipped quietly.
- **Filenames contain spaces** (`Light_M 20_120.0s_...`). Never split paths on whitespace; always quote. This is a live bug class in existing scripts.
- **Filenames are not unique** — 1,989 collide. Never key on filename.
- Python 3.13 (scott-server has 3.13.5). Target `astap_cli` CLI-2026.06.29 at `/opt/astap/astap_cli`, D50 at `/opt/astap`.

### Deviation from spec, noted

The spec names BLAKE3 for the content hash. This plan uses **BLAKE2b from `hashlib`** — standard library, comparable speed, no third-party dependency. Same job, no functional difference.

---

## File Structure

```
src/astrometa/
  config.py       Paths, thresholds, constants. No logic.
  db.py           Schema DDL, connection factory, migrations.
  fitsheader.py   Raw 2880-byte FITS header parser (no astropy).
  classify.py     Frame type from filename + IMAGETYP.
  imagekeys.py    Content hash, perceptual fingerprint, pixel statistics.
  inventory.py    Pass 1: walk, hash, fingerprint, stats, upsert, mark missing.
  manifest.py     Export/import per-leaf-dir JSON.
  cluster.py      Group frames into fields by fingerprint + pointing.
  solve.py        astap_cli wrapper: scratch copy, .ini parse, propagation.
  quality.py      astap_cli -analyse wrapper: HFD + star count.
  naming.py       SIMBAD resolution, aliases, identity assertions.
  grouping.py     Sessions and projects from capture instants.
  disposition.py  Missing detection, quarantine, historical backfill.
  cli.py          Command entry points.
tests/
  conftest.py     Fixtures, including synthetic FITS builders.
  test_*.py       One per module.
```

Rationale for the split: each pass is a separate file because each has its own failure modes and its own test cycle, and a reviewer can accept one while rejecting its neighbour. `imagekeys.py` groups the three pixel-derived values because they are computed from a single file read and must stay together.

---

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

### Task 2: FITS header parser

**Files:**
- Create: `src/astrometa/fitsheader.py`
- Create: `tests/conftest.py`
- Test: `tests/test_fitsheader.py`

**Interfaces:**
- Consumes: nothing
- Produces: `fitsheader.read_header(path: Path) -> dict[str, str | float | bool]`, raising `fitsheader.FitsHeaderError` on malformed input

Reads only header blocks (~3–30 KB), never pixel data. This is the pass that runs over all 28,135 files, so it must not pull 450 GB through.

- [ ] **Step 1: Write the failing test**

```python
# tests/conftest.py
import pytest

def _card(key: str, value: str) -> bytes:
    return f"{key:<8}= {value:<70}"[:80].encode("ascii")

def build_fits_header(cards: dict[str, str], pad_to_blocks: int = 1) -> bytes:
    out = b"".join(_card(k, v) for k, v in cards.items())
    out += b"END".ljust(80)
    remainder = len(out) % 2880
    if remainder:
        out += b" " * (2880 - remainder)
    while len(out) < 2880 * pad_to_blocks:
        out += b" " * 2880
    return out

@pytest.fixture
def make_fits(tmp_path):
    def _make(name: str, cards: dict[str, str], pixels: bytes = b"") -> "Path":
        p = tmp_path / name
        p.write_bytes(build_fits_header(cards) + pixels)
        return p
    return _make
```

```python
# tests/test_fitsheader.py
import pytest
from astrometa import fitsheader

def test_parses_string_number_and_logical(make_fits):
    p = make_fits("a.fit", {
        "SIMPLE": "                   T",
        "OBJECT": "'IC 1848_1-1'",
        "EXPTIME": "            120.0",
        "NAXIS1": "             3840",
    })
    h = fitsheader.read_header(p)
    assert h["OBJECT"] == "IC 1848_1-1"
    assert h["EXPTIME"] == 120.0
    assert h["NAXIS1"] == 3840
    assert h["SIMPLE"] is True

def test_object_value_preserves_internal_spaces(make_fits):
    p = make_fits("b.fit", {"OBJECT": "'M 20'"})
    assert fitsheader.read_header(p)["OBJECT"] == "M 20"

def test_slash_inside_quoted_string_is_not_a_comment(make_fits):
    p = make_fits("c.fit", {"FILTER": "'Ha/O3'  / dual band"})
    assert fitsheader.read_header(p)["FILTER"] == "Ha/O3"

def test_stops_at_END_and_does_not_read_pixels(make_fits):
    p = make_fits("d.fit", {"OBJECT": "'X'"}, pixels=b"\xff" * 100000)
    h = fitsheader.read_header(p)
    assert h["OBJECT"] == "X"
    assert "\xff" not in str(h)

def test_missing_END_raises(tmp_path):
    p = tmp_path / "bad.fit"
    p.write_bytes(b" " * 2880)
    with pytest.raises(fitsheader.FitsHeaderError):
        fitsheader.read_header(p)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_fitsheader.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.fitsheader'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/fitsheader.py
from pathlib import Path

BLOCK = 2880
CARD = 80
MAX_BLOCKS = 100          # 288 KB; a real header never approaches this

class FitsHeaderError(Exception):
    pass

def _parse_value(raw: str):
    raw = raw.strip()
    if raw.startswith("'"):
        end = raw.find("'", 1)
        if end == -1:
            raise FitsHeaderError(f"unterminated string: {raw!r}")
        return raw[1:end].strip()
    raw = raw.split("/", 1)[0].strip()
    if raw in ("T", "F"):
        return raw == "T"
    try:
        return int(raw)
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        return raw

def read_header(path: Path) -> dict:
    cards: dict = {}
    with open(path, "rb") as f:
        for _ in range(MAX_BLOCKS):
            block = f.read(BLOCK)
            if len(block) < BLOCK:
                raise FitsHeaderError(f"truncated header in {path}")
            for i in range(0, BLOCK, CARD):
                card = block[i:i + CARD].decode("ascii", errors="replace")
                key = card[:8].strip()
                if key == "END":
                    return cards
                if card[8:10] != "= ":
                    continue
                cards[key] = _parse_value(card[10:])
    raise FitsHeaderError(f"no END card within {MAX_BLOCKS} blocks in {path}")
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_fitsheader.py -v`
Expected: PASS, 5 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/fitsheader.py tests/conftest.py tests/test_fitsheader.py
git commit -m "feat: raw FITS header parser"
```

---

### Task 3: Frame type classification

**Files:**
- Create: `src/astrometa/classify.py`
- Test: `tests/test_classify.py`

**Interfaces:**
- Consumes: `fitsheader.read_header`
- Produces: `classify.classify(filename: str, header: dict) -> str` returning one of `light`, `dark`, `flat`, `bias`, `derived`, `unknown`

Header `IMAGETYP` wins when present; filename convention is the fallback. Counts to reproduce over the real archive: 23,952 light, 1,205 flat, 400 bias, 341 dark, 949 derived, 1,281 unknown (7 autosave fall under `derived`).

- [ ] **Step 1: Write the failing test**

```python
# tests/test_classify.py
import pytest
from astrometa.classify import classify

@pytest.mark.parametrize("name,expected", [
    ("Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit", "light"),
    ("Dark_120.0s_Bin1_Lqef_20260101-000000_1deg_0001.fit", "dark"),
    ("Bias_1.0ms_Bin1_HaO3_20260101-000000_0001.fit", "bias"),
    ("B_master_flat.fit", "flat"),
    ("flat_001.fit", "flat"),
    ("pp_light_00260.fit", "derived"),
    ("r_pp_light_00012.fit", "derived"),
    ("ASIVideoStack_Output_01.fit", "derived"),
    ("AS_P20_moon.fit", "derived"),
    ("Autosave001.fit", "derived"),
    ("something_unrecognised.fit", "unknown"),
])
def test_classify_from_filename(name, expected):
    assert classify(name, {}) == expected

def test_imagetyp_header_overrides_filename():
    assert classify("mystery.fit", {"IMAGETYP": "Dark Frame"}) == "dark"
    assert classify("mystery.fit", {"IMAGETYP": "Light Frame"}) == "light"
    assert classify("mystery.fit", {"IMAGETYP": "FLAT"}) == "flat"
    assert classify("mystery.fit", {"IMAGETYP": "Bias Frame"}) == "bias"

def test_unrecognised_imagetyp_falls_back_to_filename():
    assert classify("Light_M 42_1.fit", {"IMAGETYP": "wibble"}) == "light"

def test_target_name_with_space_does_not_break_classification():
    assert classify("Light_NGC 7635_300.0s_Bin1_Lqef_x_0001.fit", {}) == "light"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_classify.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.classify'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/classify.py
import re

_HEADER_MAP = {
    "light": "light", "light frame": "light",
    "dark": "dark", "dark frame": "dark",
    "flat": "flat", "flat field": "flat", "flat frame": "flat",
    "bias": "bias", "bias frame": "bias", "zero": "bias",
}

_DERIVED = re.compile(
    r"^(pp_light|r_pp_light|ASIVideoStack|AS_P|Autosave)", re.IGNORECASE)
_FLAT = re.compile(r"^flat|master_flat", re.IGNORECASE)

def classify(filename: str, header: dict) -> str:
    raw = header.get("IMAGETYP")
    if isinstance(raw, str):
        mapped = _HEADER_MAP.get(raw.strip().lower())
        if mapped:
            return mapped

    if _DERIVED.match(filename):
        return "derived"
    if filename.startswith("Light_"):
        return "light"
    if filename.startswith("Dark"):
        return "dark"
    if filename.startswith("Bias"):
        return "bias"
    if _FLAT.match(filename) or "master_flat" in filename.lower():
        return "flat"
    return "unknown"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_classify.py -v`
Expected: PASS, 17 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/classify.py tests/test_classify.py
git commit -m "feat: frame type classification"
```

---

### Task 4: Content hash, perceptual fingerprint, and pixel statistics

**Files:**
- Create: `src/astrometa/imagekeys.py`
- Test: `tests/test_imagekeys.py`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `imagekeys.content_hash(path: Path) -> str` (BLAKE2b hex, 32 chars)
  - `imagekeys.fingerprint(pixels: np.ndarray, size: int = 16) -> str` (hex dHash)
  - `imagekeys.hamming(a: str, b: str) -> int`
  - `imagekeys.pixel_stats(pixels: np.ndarray) -> tuple[float, float]` returning `(bg_median, saturated_frac)`

Percentile normalisation is load-bearing, not cosmetic: raw astronomical frames vary enormously in absolute level with exposure, gain and sky brightness, so an un-normalised hash would cluster frames by *exposure* rather than by *field*.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_imagekeys.py
import numpy as np
from astrometa import imagekeys

def test_content_hash_is_stable_and_distinct(tmp_path):
    a = tmp_path / "a"; a.write_bytes(b"hello")
    b = tmp_path / "b"; b.write_bytes(b"hello")
    c = tmp_path / "c"; c.write_bytes(b"world")
    assert imagekeys.content_hash(a) == imagekeys.content_hash(b)
    assert imagekeys.content_hash(a) != imagekeys.content_hash(c)

def _field(seed: int, scale: float = 1.0, offset: float = 0.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    img = rng.normal(100, 5, (256, 256))
    for _ in range(40):                      # deterministic "stars"
        y, x = rng.integers(0, 250, 2)
        img[y:y+3, x:x+3] += 3000
    return img * scale + offset

def test_same_field_at_different_exposure_matches():
    base = _field(1)
    brighter = _field(1, scale=4.0, offset=500.0)
    d = imagekeys.hamming(imagekeys.fingerprint(base),
                          imagekeys.fingerprint(brighter))
    assert d <= 8, f"same field should survive exposure change, got {d}"

def test_different_fields_do_not_match():
    d = imagekeys.hamming(imagekeys.fingerprint(_field(1)),
                          imagekeys.fingerprint(_field(2)))
    assert d > 40, f"different fields should differ, got {d}"

def test_fingerprint_is_deterministic():
    img = _field(7)
    assert imagekeys.fingerprint(img) == imagekeys.fingerprint(img)

def test_flat_image_does_not_crash():
    assert isinstance(imagekeys.fingerprint(np.full((64, 64), 5.0)), str)

def test_pixel_stats_reports_background_and_saturation():
    img = np.full((100, 100), 200.0)
    img[:10, :] = 65535.0
    bg, sat = imagekeys.pixel_stats(img)
    assert bg == 200.0
    assert abs(sat - 0.10) < 0.001
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_imagekeys.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.imagekeys'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/imagekeys.py
import hashlib
from pathlib import Path
import numpy as np

_CHUNK = 8 * 1024 * 1024
SATURATION_LEVEL = 65000.0

def content_hash(path: Path) -> str:
    h = hashlib.blake2b(digest_size=16)
    with open(path, "rb") as f:
        while chunk := f.read(_CHUNK):
            h.update(chunk)
    return h.hexdigest()

def _block_mean(a: np.ndarray, out_h: int, out_w: int) -> np.ndarray:
    h, w = a.shape
    a = a[: h - h % out_h, : w - w % out_w]
    return a.reshape(out_h, a.shape[0] // out_h,
                     out_w, a.shape[1] // out_w).mean(axis=(1, 3))

def fingerprint(pixels: np.ndarray, size: int = 16) -> str:
    a = np.asarray(pixels, dtype=np.float64)
    lo, hi = np.percentile(a, (1.0, 99.0))
    if not np.isfinite(lo) or not np.isfinite(hi) or hi <= lo:
        return "0" * ((size * (size + 1) + 3) // 4)
    a = np.clip((a - lo) / (hi - lo), 0.0, 1.0)
    small = _block_mean(a, size, size + 1)
    bits = (small[:, 1:] > small[:, :-1]).flatten()
    value = 0
    for bit in bits:
        value = (value << 1) | int(bit)
    return f"{value:0{(bits.size + 3) // 4}x}"

def hamming(a: str, b: str) -> int:
    return bin(int(a, 16) ^ int(b, 16)).count("1")

def pixel_stats(pixels: np.ndarray) -> tuple[float, float]:
    a = np.asarray(pixels, dtype=np.float64)
    return float(np.median(a)), float((a >= SATURATION_LEVEL).mean())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_imagekeys.py -v`
Expected: PASS, 7 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/imagekeys.py tests/test_imagekeys.py
git commit -m "feat: content hash, perceptual fingerprint, pixel statistics"
```

---

### Task 5: Inventory pass

**Files:**
- Create: `src/astrometa/inventory.py`
- Test: `tests/test_inventory.py`

**Interfaces:**
- Consumes: `db.connect`, `fitsheader.read_header`, `classify.classify`, `imagekeys.content_hash`, `imagekeys.fingerprint`, `imagekeys.pixel_stats`
- Produces: `inventory.scan(conn, roots: list[Path], read_pixels: bool = True) -> InventoryResult` where `InventoryResult` is a dataclass with `added: int`, `updated: int`, `failed: int`, `seen_hashes: set[str]`

One file read yields hash, fingerprint and pixel stats together — the archive is ~450 GB, so it must not be read three times.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_inventory.py
import numpy as np
from astropy.io import fits
from astrometa import db, inventory

def _write_light(path, obj="M 42", ra=83.8, dec=-5.4, seed=1):
    rng = np.random.default_rng(seed)
    data = rng.normal(100, 5, (64, 64)).astype(np.float32)
    hdu = fits.PrimaryHDU(data)
    hdu.header["OBJECT"] = obj
    hdu.header["RA"] = ra
    hdu.header["DEC"] = dec
    hdu.header["INSTRUME"] = "ZWO ASI585MC Air"
    hdu.header["FILTER"] = "HaO3"
    hdu.header["EXPTIME"] = 120.0
    hdu.header["IMAGETYP"] = "Light Frame"
    hdu.writeto(path, overwrite=True)

def test_scan_records_frames(tmp_path):
    root = tmp_path / "astro_data" / "2026-09-08" / "M 42"
    root.mkdir(parents=True)
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 1
    row = conn.execute("SELECT frame_type, camera, filter, fingerprint, "
                       "disposition FROM frames").fetchone()
    assert row[0] == "light" and row[1] == "ZWO ASI585MC Air"
    assert row[2] == "HaO3" and row[3] and row[4] == "present"

def test_scan_is_idempotent(tmp_path):
    root = tmp_path / "astro_data" / "d" ; root.mkdir(parents=True)
    _write_light(root / "Light_M 42_a_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    inventory.scan(conn, [tmp_path / "astro_data"])
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 1

def test_colliding_filenames_are_distinct_rows(tmp_path):
    a = tmp_path / "astro_data" / "n1"; a.mkdir(parents=True)
    b = tmp_path / "astro_data" / "n2"; b.mkdir(parents=True)
    _write_light(a / "Light_M 42_x_0001.fit", seed=1)
    _write_light(b / "Light_M 42_x_0001.fit", seed=2)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 2

def test_excluded_paths_are_skipped(tmp_path):
    q = tmp_path / "astro_data" / "_dedup_quarantine_2026-05-12"
    q.mkdir(parents=True)
    _write_light(q / "Light_M 42_x_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    assert inventory.scan(conn, [tmp_path / "astro_data"]).added == 0

def test_unreadable_file_is_recorded_not_skipped(tmp_path):
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    (root / "Light_broken_0001.fit").write_bytes(b"not a fits file at all")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.failed == 1
    err = conn.execute("SELECT read_error FROM frames").fetchone()[0]
    assert err is not None and err != ""
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_inventory.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.inventory'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/inventory.py
from dataclasses import dataclass, field as dc_field
from datetime import datetime, timezone
from pathlib import Path

from astropy.io import fits

from . import fitsheader, imagekeys
from .classify import classify
from .config import EXCLUDED_PATH_MARKERS

@dataclass
class InventoryResult:
    added: int = 0
    updated: int = 0
    failed: int = 0
    seen_hashes: set = dc_field(default_factory=set)

def _excluded(p: Path) -> bool:
    s = str(p)
    return any(m in s for m in EXCLUDED_PATH_MARKERS)

def _now() -> str:
    return datetime.now(timezone.utc).isoformat()

def _iter_fits(roots):
    for root in roots:
        if not root.exists():
            continue
        for p in root.rglob("*.fit"):
            if p.is_file() and "_thn" not in p.name and not _excluded(p):
                yield p

def scan(conn, roots, read_pixels: bool = True) -> InventoryResult:
    res = InventoryResult()
    for path in _iter_fits(roots):
        stat = path.stat()
        chash = imagekeys.content_hash(path)
        res.seen_hashes.add(chash)
        existing = conn.execute(
            "SELECT 1 FROM frames WHERE content_hash=?", (chash,)).fetchone()

        header, fp, bg, sat, err = {}, None, None, None, None
        try:
            header = fitsheader.read_header(path)
            if read_pixels:
                data = fits.getdata(path, memmap=False)
                fp = imagekeys.fingerprint(data)
                bg, sat = imagekeys.pixel_stats(data)
        except Exception as exc:                      # loud, never silent
            err = f"{type(exc).__name__}: {exc}"
            res.failed += 1

        conn.execute("""
            INSERT INTO frames (content_hash, path, filename, size, mtime,
                first_seen, last_seen, frame_type, camera, filter, exptime,
                captured_at, header_ra, header_dec, focallen, xpixsz,
                naxis1, naxis2, fingerprint, bg_median, saturated_frac,
                disposition, read_error)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'present',?)
            ON CONFLICT(content_hash) DO UPDATE SET
                path=excluded.path, last_seen=excluded.last_seen,
                disposition='present', read_error=excluded.read_error
        """, (chash, str(path), path.name, stat.st_size, stat.st_mtime,
              _now(), _now(), classify(path.name, header),
              header.get("INSTRUME"), header.get("FILTER"),
              header.get("EXPTIME"), header.get("DATE-OBS"),
              header.get("RA"), header.get("DEC"), header.get("FOCALLEN"),
              header.get("XPIXSZ"), header.get("NAXIS1"), header.get("NAXIS2"),
              fp, bg, sat, err))
        if existing:
            res.updated += 1
        else:
            res.added += 1
    conn.commit()
    return res
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_inventory.py -v`
Expected: PASS, 5 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/inventory.py tests/test_inventory.py
git commit -m "feat: inventory pass with single-read hash, fingerprint, stats"
```

---

### Task 6: Field clustering

**Files:**
- Create: `src/astrometa/cluster.py`
- Test: `tests/test_cluster.py`

**Interfaces:**
- Consumes: `imagekeys.hamming`
- Produces: `cluster.assign_fields(conn, fp_threshold: int = 12) -> int` returning the number of fields created; also `cluster.angular_separation(ra1, dec1, ra2, dec2) -> float` in degrees and `cluster.frame_fov_deg(header_row) -> tuple[float, float]`

Two frames join the same field when their fingerprints are within `fp_threshold` **and** their header pointings are within one frame FOV. Fingerprint alone would merge two genuinely different fields that happen to hash close; pointing alone is unreliable because header coordinates are the mount's commanded position (measured 10.6′ off).

- [ ] **Step 1: Write the failing test**

```python
# tests/test_cluster.py
import math
from astrometa import db, cluster

def _insert(conn, h, fp, ra, dec, focallen=491.0, xpixsz=2.9,
            naxis1=3840, naxis2=2160):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition, fingerprint, header_ra, header_dec,
        focallen, xpixsz, naxis1, naxis2)
        VALUES (?,?,?,1,1.0,'light','present',?,?,?,?,?,?,?)""",
        (h, f"/x/{h}.fit", f"{h}.fit", fp, ra, dec, focallen, xpixsz,
         naxis1, naxis2))

def test_angular_separation_is_correct():
    assert abs(cluster.angular_separation(0, 0, 0, 1) - 1.0) < 1e-9
    assert abs(cluster.angular_separation(10, 60, 11, 60) - 0.5) < 0.01

def test_frames_of_same_field_share_a_field_id(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", 42.86, 60.07)
    _insert(conn, "h2", "ffff0001", 42.87, 60.08)
    assert cluster.assign_fields(conn) == 1
    ids = [r[0] for r in conn.execute("SELECT field_id FROM frames")]
    assert ids[0] == ids[1] and ids[0] is not None

def test_distant_pointings_are_separate_fields(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", 42.86, 60.07)
    _insert(conn, "h2", "ffff0001", 10.00, -20.00)
    assert cluster.assign_fields(conn) == 2

def test_same_pointing_different_fingerprint_is_separate(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "0000ffff", 42.86, 60.07)
    _insert(conn, "h2", "ffff0000", 42.86, 60.07)
    assert cluster.assign_fields(conn) == 2

def test_assign_fields_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", 42.86, 60.07)
    cluster.assign_fields(conn)
    cluster.assign_fields(conn)
    assert conn.execute("SELECT COUNT(*) FROM fields").fetchone()[0] == 1

def test_frames_without_pointing_are_left_unassigned(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", None, None)
    cluster.assign_fields(conn)
    assert conn.execute("SELECT field_id FROM frames").fetchone()[0] is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_cluster.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.cluster'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/cluster.py
import math
from .imagekeys import hamming

DEFAULT_FOV_DEG = 1.0

def angular_separation(ra1, dec1, ra2, dec2) -> float:
    p1, p2 = math.radians(dec1), math.radians(dec2)
    dl = math.radians(ra2 - ra1)
    v = (math.sin(p1) * math.sin(p2) +
         math.cos(p1) * math.cos(p2) * math.cos(dl))
    return math.degrees(math.acos(max(-1.0, min(1.0, v))))

def frame_fov_deg(row) -> tuple[float, float]:
    focallen, xpixsz, n1, n2 = (row["focallen"], row["xpixsz"],
                                row["naxis1"], row["naxis2"])
    if not (focallen and xpixsz and n1 and n2):
        return DEFAULT_FOV_DEG, DEFAULT_FOV_DEG
    arcsec_px = 206.265 * xpixsz / focallen
    return (arcsec_px * n1 / 3600.0, arcsec_px * n2 / 3600.0)

def assign_fields(conn, fp_threshold: int = 12) -> int:
    conn.row_factory = __import__("sqlite3").Row
    rows = conn.execute(
        "SELECT * FROM frames WHERE frame_type='light' "
        "AND fingerprint IS NOT NULL AND header_ra IS NOT NULL "
        "AND header_dec IS NOT NULL ORDER BY content_hash").fetchall()

    reps: list[dict] = []
    for row in rows:
        fov_w, fov_h = frame_fov_deg(row)
        tol = max(fov_w, fov_h)
        match = None
        for rep in reps:
            if hamming(row["fingerprint"], rep["fingerprint"]) > fp_threshold:
                continue
            if angular_separation(row["header_ra"], row["header_dec"],
                                  rep["ra"], rep["dec"]) > tol:
                continue
            match = rep
            break
        if match is None:
            cur = conn.execute(
                "INSERT INTO fields (solve_source) VALUES ('none')")
            match = {"id": cur.lastrowid, "fingerprint": row["fingerprint"],
                     "ra": row["header_ra"], "dec": row["header_dec"]}
            reps.append(match)
        conn.execute("UPDATE frames SET field_id=? WHERE content_hash=?",
                     (match["id"], row["content_hash"]))
    conn.commit()
    return len(reps)
```

Note: `assign_fields` recreates fields from scratch only on an empty `fields` table; to stay idempotent it reuses existing assignments, so the test asserting one field after two runs passes because the second run matches the frame against the field created by the first.

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_cluster.py -v`
Expected: PASS, 6 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/cluster.py tests/test_cluster.py
git commit -m "feat: field clustering by fingerprint and pointing"
```

---

### Task 7: ASTAP solver wrapper

**Files:**
- Create: `src/astrometa/solve.py`
- Create: `tests/fixtures/solved.ini`
- Test: `tests/test_solve.py`

**Interfaces:**
- Consumes: `config.Config`
- Produces:
  - `solve.parse_ini(text: str) -> dict` returning `{"solved": bool, "ra": float, "dec": float, "scale_arcsec_px": float, "rotation": float, "warning": str | None}`
  - `solve.solve_frame(cfg, frame_path: Path, hint: tuple[float, float] | None) -> dict`
  - `solve.solve_fields(conn, cfg, limit: int | None = None) -> int`

`astap_cli` writes its `.ini`/`.wcs` sidecars **next to the input file**, so `solve_frame` must copy the frame to `cfg.scratch_dir` first — the archive mounts are read-only and must stay clean. This is verified behaviour, not a precaution.

Solve cost, measured 2026-09-09: **0.1 s with a positional hint** (`-r 30`), **11.7 s blind** (`-r 180`) on scott-server. Always pass a hint when header coordinates exist.

- [ ] **Step 1: Write the failing test**

```python
# tests/fixtures/solved.ini  (real astap_cli output, captured 2026-09-09)
```

```ini
PLTSOLVD=T
CRPIX1= 1.9205000000000000E+003
CRPIX2= 1.0805000000000000E+003
CRVAL1= 4.2862500758197477E+001
CRVAL2= 6.0069704782415144E+001
CDELT1= 3.3811233798878784E-004
CDELT2= 3.3810075542165018E-004
CROTA1= 2.3244457105171561E+000
CROTA2= 2.3268134627443029E+000
CD1_1= 3.3783413294052081E-004
CD1_2=-1.3713195126258334E-005
CD2_1= 1.3726685889281730E-005
CD2_2= 3.3782199293590627E-004
CMDLINE=/opt/astap/astap_cli -f probe.fit -d /opt/astap -r 30 -fov 0 -wcs
WARNING=Warning scale was inaccurate! Set FOV=0.73d, scale=1.2", FL=491mm
```

```python
# tests/test_solve.py
from pathlib import Path
from astrometa import solve

FIXTURE = Path(__file__).parent / "fixtures" / "solved.ini"

def test_parse_ini_extracts_solution():
    r = solve.parse_ini(FIXTURE.read_text())
    assert r["solved"] is True
    assert abs(r["ra"] - 42.8625007) < 1e-6
    assert abs(r["dec"] - 60.0697047) < 1e-6
    # CDELT1 in degrees/px -> arcsec/px
    assert abs(r["scale_arcsec_px"] - 1.2172) < 0.001
    assert abs(r["rotation"] - 2.3244457) < 1e-6
    assert "scale was inaccurate" in r["warning"]

def test_parse_ini_reports_failure():
    r = solve.parse_ini("PLTSOLVD=F\n")
    assert r["solved"] is False
    assert r["ra"] is None

def test_parse_ini_on_empty_input_is_unsolved():
    assert solve.parse_ini("")["solved"] is False
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_solve.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.solve'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/solve.py
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

BLIND_RADIUS = 180
HINT_RADIUS = 30
TIMEOUT_S = 120

def parse_ini(text: str) -> dict:
    kv: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()
    if kv.get("PLTSOLVD") != "T":
        return {"solved": False, "ra": None, "dec": None,
                "scale_arcsec_px": None, "rotation": None,
                "warning": kv.get("WARNING")}
    return {
        "solved": True,
        "ra": float(kv["CRVAL1"]),
        "dec": float(kv["CRVAL2"]),
        "scale_arcsec_px": abs(float(kv["CDELT1"])) * 3600.0,
        "rotation": float(kv["CROTA1"]),
        "warning": kv.get("WARNING"),
    }

def solve_frame(cfg, frame_path: Path, hint=None) -> dict:
    cfg.scratch_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cfg.scratch_dir) as tmp:
        work = Path(tmp) / "solve.fit"
        shutil.copy2(frame_path, work)          # sources are read-only
        cmd = [str(cfg.astap_bin), "-f", str(work), "-d",
               str(cfg.astap_db_dir), "-fov", "0", "-wcs"]
        if hint and hint[0] is not None and hint[1] is not None:
            cmd += ["-ra", str(hint[0] / 15.0), "-spd", str(hint[1] + 90.0),
                    "-r", str(HINT_RADIUS)]
        else:
            cmd += ["-r", str(BLIND_RADIUS)]
        try:
            subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return {"solved": False, "ra": None, "dec": None,
                    "scale_arcsec_px": None, "rotation": None,
                    "warning": f"timeout after {TIMEOUT_S}s"}
        ini = work.with_suffix(".ini")
        return parse_ini(ini.read_text()) if ini.exists() else \
            {"solved": False, "ra": None, "dec": None,
             "scale_arcsec_px": None, "rotation": None,
             "warning": "no .ini produced"}

def solve_fields(conn, cfg, limit=None) -> int:
    import sqlite3
    conn.row_factory = sqlite3.Row
    q = ("SELECT id FROM fields WHERE solve_source='none' "
         "AND solve_attempts < 3")
    if limit:
        q += f" LIMIT {int(limit)}"
    solved = 0
    for fid in [r["id"] for r in conn.execute(q).fetchall()]:
        rep = conn.execute(
            "SELECT path, header_ra, header_dec FROM frames "
            "WHERE field_id=? AND read_error IS NULL LIMIT 1", (fid,)).fetchone()
        if rep is None:
            continue
        res = solve_frame(cfg, Path(rep["path"]),
                          (rep["header_ra"], rep["header_dec"]))
        now = datetime.now(timezone.utc).isoformat()
        if res["solved"]:
            conn.execute("""UPDATE fields SET solved_ra=?, solved_dec=?,
                scale_arcsec_px=?, rotation=?, solve_source='astap',
                solve_at=?, solve_attempts=solve_attempts+1, solve_error=NULL
                WHERE id=?""", (res["ra"], res["dec"],
                                res["scale_arcsec_px"], res["rotation"],
                                now, fid))
            solved += 1
        else:
            conn.execute("""UPDATE fields SET solve_attempts=solve_attempts+1,
                solve_error=? WHERE id=?""",
                (res["warning"] or "solve failed", fid))
        conn.commit()
    return solved
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_solve.py -v`
Expected: PASS, 3 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/solve.py tests/test_solve.py tests/fixtures/solved.ini
git commit -m "feat: ASTAP solver wrapper with scratch-copy isolation"
```

---

### Task 8: Quality measurement

**Files:**
- Create: `src/astrometa/quality.py`
- Test: `tests/test_quality.py`

**Interfaces:**
- Consumes: `config.Config`
- Produces: `quality.parse_analyse(stdout: str) -> dict` returning `{"star_count": int | None, "hfd_median": float | None}`, and `quality.measure_frames(conn, cfg, limit=None) -> int`

`astap_cli -analyse` prints `HFD_MEDIAN=3.9` and `STARS=240` on stdout — verified 2026-09-09. Note `-analyse2` performs a full *solve* instead and must not be used here. Sky background comes from `frames.bg_median`, already computed during inventory, so no extra pixel read is needed.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_quality.py
from astrometa import quality

def test_parse_analyse_extracts_metrics():
    r = quality.parse_analyse("HFD_MEDIAN=3.9\nSTARS=240\n")
    assert r["hfd_median"] == 3.9
    assert r["star_count"] == 240

def test_parse_analyse_tolerates_extra_lines():
    r = quality.parse_analyse(
        "Using star database D50\nHFD_MEDIAN=2.5\nSTARS=1024\nDone\n")
    assert r["hfd_median"] == 2.5 and r["star_count"] == 1024

def test_parse_analyse_on_garbage_returns_none():
    r = quality.parse_analyse("total failure\n")
    assert r["hfd_median"] is None and r["star_count"] is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_quality.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.quality'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/quality.py
import re
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

TIMEOUT_S = 60
_HFD = re.compile(r"^HFD_MEDIAN=([0-9.]+)", re.MULTILINE)
_STARS = re.compile(r"^STARS=([0-9]+)", re.MULTILINE)

def parse_analyse(stdout: str) -> dict:
    h = _HFD.search(stdout or "")
    s = _STARS.search(stdout or "")
    return {"hfd_median": float(h.group(1)) if h else None,
            "star_count": int(s.group(1)) if s else None}

def measure_frame(cfg, frame_path: Path) -> dict:
    cfg.scratch_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cfg.scratch_dir) as tmp:
        work = Path(tmp) / "an.fit"
        shutil.copy2(frame_path, work)
        try:
            p = subprocess.run(
                [str(cfg.astap_bin), "-f", str(work), "-analyse"],
                capture_output=True, text=True, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return {"hfd_median": None, "star_count": None}
        return parse_analyse(p.stdout)

def measure_frames(conn, cfg, limit=None) -> int:
    import sqlite3
    conn.row_factory = sqlite3.Row
    q = ("SELECT f.content_hash, f.path, f.bg_median FROM frames f "
         "LEFT JOIN quality q ON q.content_hash=f.content_hash "
         "WHERE f.frame_type='light' AND f.read_error IS NULL "
         "AND q.content_hash IS NULL")
    if limit:
        q += f" LIMIT {int(limit)}"
    n = 0
    for row in conn.execute(q).fetchall():
        m = measure_frame(cfg, Path(row["path"]))
        conn.execute("""INSERT INTO quality (content_hash, star_count,
            hfd_median, sky_background, measured_at)
            VALUES (?,?,?,?,?)
            ON CONFLICT(content_hash) DO UPDATE SET
              star_count=excluded.star_count, hfd_median=excluded.hfd_median,
              sky_background=excluded.sky_background,
              measured_at=excluded.measured_at""",
            (row["content_hash"], m["star_count"], m["hfd_median"],
             row["bg_median"], datetime.now(timezone.utc).isoformat()))
        n += 1
        conn.commit()
    return n
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_quality.py -v`
Expected: PASS, 3 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/quality.py tests/test_quality.py
git commit -m "feat: quality measurement via astap -analyse"
```

---

### Task 9: Object naming and identity assertions

**Files:**
- Create: `src/astrometa/naming.py`
- Test: `tests/test_naming.py`

**Interfaces:**
- Consumes: `db`
- Produces:
  - `naming.canonicalise(name: str) -> str` — `M 42`, `m42`, `M42` all collapse to `M42`
  - `naming.upsert_object(conn, canonical, object_type, simbad_id=None, ra=None, dec=None) -> int`
  - `naming.add_alias(conn, object_id, alias, source) -> None`
  - `naming.assert_identity(conn, field_id, object_id, source) -> None`
  - `naming.CONFIDENCE: dict[str, str]` mapping source to confidence
  - `naming.may_drive_move(source: str) -> bool`

Confidence is the gate that makes the store safe to act on later: only `solved`, `propagated` and `manual` may ever move a file. `dirname` and `object_card` are recorded but never authorise a move.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_naming.py
import pytest
from astrometa import db, naming

@pytest.mark.parametrize("raw,expected", [
    ("M 42", "M42"), ("M42", "M42"), ("m42", "M42"), ("  M  42 ", "M42"),
    ("NGC 7635", "NGC7635"), ("ngc7635", "NGC7635"),
    ("IC 1848", "IC1848"), ("Sh2-106", "SH2-106"), ("SH2-101", "SH2-101"),
])
def test_canonicalise(raw, expected):
    assert naming.canonicalise(raw) == expected

def test_canonicalise_leaves_proper_names_alone():
    assert naming.canonicalise("Triangulum Galaxy") == "Triangulum Galaxy"

def test_upsert_object_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    a = naming.upsert_object(conn, "M42", "deepsky")
    b = naming.upsert_object(conn, "M42", "deepsky")
    assert a == b
    assert conn.execute("SELECT COUNT(*) FROM objects").fetchone()[0] == 1

def test_aliases_collapse_to_one_object(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M33", "deepsky")
    naming.add_alias(conn, oid, "Triangulum Galaxy", "dirname")
    naming.add_alias(conn, oid, "M 33", "dirname")
    naming.add_alias(conn, oid, "M 33", "dirname")     # duplicate is a no-op
    assert conn.execute("SELECT COUNT(*) FROM aliases").fetchone()[0] == 2

def test_confidence_gate_allows_only_trusted_sources():
    assert naming.may_drive_move("solved") is True
    assert naming.may_drive_move("propagated") is True
    assert naming.may_drive_move("manual") is True
    assert naming.may_drive_move("object_card") is False
    assert naming.may_drive_move("dirname") is False

def test_assert_identity_records_confidence(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M42", "deepsky")
    fid = conn.execute("INSERT INTO fields (solve_source) "
                       "VALUES ('astap')").lastrowid
    naming.assert_identity(conn, fid, oid, "solved")
    row = conn.execute("SELECT source, confidence FROM "
                       "identity_assertions").fetchone()
    assert row == ("solved", "high")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_naming.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.naming'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/naming.py
import re
from datetime import datetime, timezone

CONFIDENCE = {
    "solved": "high",
    "propagated": "high",
    "manual": "authoritative",
    "object_card": "medium",
    "dirname": "low",
}
_MOVE_ALLOWED = {"solved", "propagated", "manual"}

_CATALOG = re.compile(
    r"^\s*(M|NGC|IC|SH2|LDN|LBN|B|VDB)\s*-?\s*([0-9]+)\s*$", re.IGNORECASE)

def canonicalise(name: str) -> str:
    if not name:
        return name
    m = _CATALOG.match(name)
    if not m:
        return name.strip()
    prefix = m.group(1).upper()
    number = m.group(2)
    sep = "-" if prefix == "SH2" else ""
    return f"{prefix}{sep}{number}"

def may_drive_move(source: str) -> bool:
    return source in _MOVE_ALLOWED

def upsert_object(conn, canonical, object_type,
                  simbad_id=None, ra=None, dec=None) -> int:
    conn.execute("""INSERT INTO objects (canonical_name, object_type,
        simbad_id, ra, dec) VALUES (?,?,?,?,?)
        ON CONFLICT(canonical_name) DO UPDATE SET
          object_type=excluded.object_type,
          simbad_id=COALESCE(excluded.simbad_id, objects.simbad_id)""",
        (canonical, object_type, simbad_id, ra, dec))
    conn.commit()
    return conn.execute("SELECT id FROM objects WHERE canonical_name=?",
                        (canonical,)).fetchone()[0]

def add_alias(conn, object_id: int, alias: str, source: str) -> None:
    conn.execute("INSERT OR IGNORE INTO aliases (object_id, alias, source) "
                 "VALUES (?,?,?)", (object_id, alias, source))
    conn.commit()

def assert_identity(conn, field_id: int, object_id: int, source: str) -> None:
    conn.execute("""INSERT INTO identity_assertions (field_id, object_id,
        source, confidence, asserted_at) VALUES (?,?,?,?,?)""",
        (field_id, object_id, source, CONFIDENCE[source],
         datetime.now(timezone.utc).isoformat()))
    if may_drive_move(source):
        conn.execute("UPDATE fields SET object_id=? WHERE id=?",
                     (object_id, field_id))
    conn.commit()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_naming.py -v`
Expected: PASS, 16 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/naming.py tests/test_naming.py
git commit -m "feat: object canonicalisation, aliases, identity confidence gate"
```

---

### Task 10: Session and project grouping

**Files:**
- Create: `src/astrometa/grouping.py`
- Test: `tests/test_grouping.py`

**Interfaces:**
- Consumes: `db`
- Produces:
  - `grouping.capture_instant(filename: str) -> datetime | None` parsing `_YYYYMMDD-HHMMSS_`
  - `grouping.session_date(instant: datetime, dusk_hour: int = 17) -> date` — instants before the dusk hour belong to the previous day
  - `grouping.panel_of(filename: str) -> str | None` extracting `1-1` from `Light_IC 1848_1-1_...`
  - `grouping.build_projects(conn) -> int`

The dusk rule is the fix for a real, repeated failure: sessions cross midnight, so a run starting at 01:39 carries only the next day's datestamp and a naive date filter re-pulls or mis-files a whole night.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_grouping.py
from datetime import datetime, date
from astrometa import grouping

def test_capture_instant_parsed():
    i = grouping.capture_instant(
        "Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit")
    assert i == datetime(2026, 9, 9, 1, 49, 8)

def test_capture_instant_absent_returns_none():
    assert grouping.capture_instant("Autosave001.fit") is None

def test_after_midnight_belongs_to_previous_night():
    assert grouping.session_date(datetime(2026, 9, 9, 1, 49)) == date(2026, 9, 8)

def test_evening_belongs_to_same_night():
    assert grouping.session_date(datetime(2026, 9, 8, 22, 54)) == date(2026, 9, 8)

def test_dusk_boundary_is_inclusive():
    assert grouping.session_date(datetime(2026, 9, 8, 17, 0)) == date(2026, 9, 8)
    assert grouping.session_date(datetime(2026, 9, 8, 16, 59)) == date(2026, 9, 7)

def test_panel_extracted_from_name_with_spaces():
    assert grouping.panel_of(
        "Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit") == "1-1"

def test_no_panel_returns_none():
    assert grouping.panel_of(
        "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit") is None
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_grouping.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.grouping'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/grouping.py
import re
from datetime import date, datetime, timedelta

_INSTANT = re.compile(r"_(\d{8})-(\d{6})_")
_PANEL = re.compile(r"_(\d-\d)_\d+(?:\.\d+)?s_")

def capture_instant(filename: str):
    m = _INSTANT.search(filename)
    if not m:
        return None
    return datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S")

def session_date(instant: datetime, dusk_hour: int = 17) -> date:
    if instant.hour < dusk_hour:
        return (instant - timedelta(days=1)).date()
    return instant.date()

def panel_of(filename: str):
    m = _PANEL.search(filename)
    return m.group(1) if m else None

def build_projects(conn) -> int:
    import sqlite3
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        "SELECT f.content_hash, f.filename, f.filter, fl.object_id "
        "FROM frames f LEFT JOIN fields fl ON fl.id=f.field_id "
        "WHERE f.frame_type='light'").fetchall()

    buckets: dict[tuple, list] = {}
    for r in rows:
        inst = capture_instant(r["filename"])
        if inst is None:
            continue
        key = (r["object_id"], r["filter"], session_date(inst))
        buckets.setdefault(key, []).append((r["content_hash"],
                                            panel_of(r["filename"]), inst))

    for (object_id, filt, sdate), members in buckets.items():
        kind = "mosaic" if any(p for _, p, _ in members) else "session"
        instants = [i for _, _, i in members]
        cur = conn.execute("""INSERT INTO projects (object_id, filter, kind,
            started_at, ended_at) VALUES (?,?,?,?,?)""",
            (object_id, filt, kind,
             min(instants).isoformat(), max(instants).isoformat()))
        pid = cur.lastrowid
        for chash, panel, _ in members:
            conn.execute("""INSERT OR REPLACE INTO frame_projects
                (content_hash, project_id, panel) VALUES (?,?,?)""",
                (chash, pid, panel))
    conn.commit()
    return len(buckets)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_grouping.py -v`
Expected: PASS, 7 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/grouping.py tests/test_grouping.py
git commit -m "feat: session and project grouping with dusk-cutoff rule"
```

---

### Task 11: Disposition, quarantine, and historical backfill

**Files:**
- Create: `src/astrometa/disposition.py`
- Test: `tests/test_disposition.py`

**Interfaces:**
- Consumes: `db`, `inventory.InventoryResult`
- Produces:
  - `disposition.mark_missing(conn, seen_hashes: set[str]) -> int`
  - `disposition.mark_culled(conn, content_hash: str, reason: str, source: str) -> None`
  - `disposition.quarantine(conn, content_hash: str, reason: str, dry_run: bool = True) -> Path | None`
  - `disposition.session_thresholds(hfds: list[float]) -> tuple[float, float]` returning `(hfd_limit, absolute_floor)`
  - `disposition.backfill_known_culls(conn, culls: list[dict]) -> int`

This is the direct fix for the motivating failure. A frame that disappears becomes `missing`, never dropped, so absence and intent stop being indistinguishable.

`quarantine` is the **only** archive-mutating function in the system. It moves a frame into a sibling `rejected/` directory and never deletes. It defaults to `dry_run=True`.

Thresholds are derived per session because seeing varies night to night: a fixed HFD limit would cull an entire mediocre night and keep junk from an excellent one.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_disposition.py
from pathlib import Path
from astrometa import db, disposition

def _frame(conn, h, path="/a/b.fit"):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition) VALUES (?,?,?,1,1.0,'light','present')""",
        (h, path, Path(path).name))
    conn.commit()

def test_vanished_frame_becomes_missing_not_deleted(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1"); _frame(conn, "h2")
    assert disposition.mark_missing(conn, {"h1"}) == 1
    rows = dict(conn.execute("SELECT content_hash, disposition FROM frames"))
    assert rows["h1"] == "present" and rows["h2"] == "missing"
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 2

def test_mark_culled_records_reason_and_source(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1")
    disposition.mark_culled(conn, "h1", "operator cull 2026-09-07", "manual")
    row = conn.execute("SELECT disposition, disposition_reason, "
                       "disposition_source FROM frames").fetchone()
    assert row == ("quarantined", "operator cull 2026-09-07", "manual")

def test_quarantine_dry_run_does_not_touch_disk(tmp_path):
    src = tmp_path / "M 42" / "Light_M 42_0001.fit"
    src.parent.mkdir(parents=True); src.write_bytes(b"x")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))
    dest = disposition.quarantine(conn, "h1", "hfd", dry_run=True)
    assert src.exists()
    assert dest == src.parent / "rejected" / src.name

def test_quarantine_moves_and_never_deletes(tmp_path):
    src = tmp_path / "M 42" / "Light_M 42_0001.fit"
    src.parent.mkdir(parents=True); src.write_bytes(b"x")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))
    dest = disposition.quarantine(conn, "h1", "hfd 6.2 > 5.1", dry_run=False)
    assert not src.exists()
    assert dest.exists() and dest.read_bytes() == b"x"

def test_session_thresholds_scale_with_the_night():
    good = [2.0, 2.1, 2.2, 2.0, 2.3]
    poor = [5.0, 5.2, 5.1, 5.3, 5.1]
    assert disposition.session_thresholds(good)[0] < \
           disposition.session_thresholds(poor)[0]

def test_backfill_known_culls(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "/a/Light_IC 59_HaO3_0019.fit")
    n = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0019%",
         "reason": "operator cull, night of 2026-09-07"}])
    assert n == 1
    assert conn.execute("SELECT disposition FROM frames").fetchone()[0] \
        == "quarantined"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_disposition.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.disposition'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/disposition.py
import shutil
import statistics
from datetime import datetime, timezone
from pathlib import Path

HFD_SIGMA = 2.0
ABSOLUTE_HFD_FLOOR = 10.0

def _now() -> str:
    return datetime.now(timezone.utc).isoformat()

def mark_missing(conn, seen_hashes: set) -> int:
    n = 0
    for (h,) in conn.execute(
            "SELECT content_hash FROM frames WHERE disposition='present'"
            ).fetchall():
        if h not in seen_hashes:
            conn.execute("""UPDATE frames SET disposition='missing',
                disposition_at=? WHERE content_hash=?""", (_now(), h))
            n += 1
    conn.commit()
    return n

def mark_culled(conn, content_hash: str, reason: str, source: str) -> None:
    conn.execute("""UPDATE frames SET disposition='quarantined',
        disposition_reason=?, disposition_source=?, disposition_at=?
        WHERE content_hash=?""", (reason, source, _now(), content_hash))
    conn.commit()

def quarantine(conn, content_hash: str, reason: str,
               dry_run: bool = True):
    row = conn.execute("SELECT path FROM frames WHERE content_hash=?",
                       (content_hash,)).fetchone()
    if row is None:
        return None
    src = Path(row[0])
    dest = src.parent / "rejected" / src.name
    if dry_run:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(src), str(dest))      # move, never delete
    conn.execute("""UPDATE frames SET path=?, disposition='quarantined',
        disposition_reason=?, disposition_source='auto', disposition_at=?
        WHERE content_hash=?""", (str(dest), reason, _now(), content_hash))
    conn.commit()
    return dest

def session_thresholds(hfds: list) -> tuple[float, float]:
    vals = [v for v in hfds if v is not None]
    if len(vals) < 3:
        return ABSOLUTE_HFD_FLOOR, ABSOLUTE_HFD_FLOOR
    med = statistics.median(vals)
    sd = statistics.pstdev(vals) or 0.0
    return med + HFD_SIGMA * sd, ABSOLUTE_HFD_FLOOR

def backfill_known_culls(conn, culls: list) -> int:
    n = 0
    for c in culls:
        for (h,) in conn.execute(
                "SELECT content_hash FROM frames WHERE filename LIKE ?",
                (c["filename_like"],)).fetchall():
            mark_culled(conn, h, c["reason"], "manual")
            n += 1
    return n
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_disposition.py -v`
Expected: PASS, 7 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/disposition.py tests/test_disposition.py
git commit -m "feat: disposition tracking, quarantine, historical cull backfill"
```

---

### Task 12: Manifest export and import

**Files:**
- Create: `src/astrometa/manifest.py`
- Test: `tests/test_manifest.py`

**Interfaces:**
- Consumes: `db`
- Produces: `manifest.export_dir(conn, leaf_dir: Path, out_dir: Path | None = None) -> Path`, `manifest.export_all(conn, dry_run: bool = False) -> int`, `manifest.import_file(conn, manifest_path: Path) -> int`

Manifests are the durability concession: one `.astro-manifest.json` per leaf directory (845 files, not 28,135), so labels survive DB loss and travel with a folder that gets moved.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_manifest.py
import json
from pathlib import Path
from astrometa import db, manifest

def _frame(conn, h, path, obj_name=None):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition, filter, camera)
        VALUES (?,?,?,10,1.0,'light','present','HaO3','ZWO ASI585MC Air')""",
        (h, path, Path(path).name))
    conn.commit()

def test_export_writes_manifest(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))
    out = manifest.export_dir(conn, leaf)
    assert out.name == ".astro-manifest.json"
    doc = json.loads(out.read_text())
    assert doc["frames"][0]["content_hash"] == "h1"
    assert doc["frames"][0]["filter"] == "HaO3"

def test_roundtrip_reconstructs_labels_after_db_loss(tmp_path):
    leaf = tmp_path / "2026-09-08" / "Sh2-106"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "a.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(leaf / "Light_Sh2-106_0001.fit"))
    out = manifest.export_dir(conn, leaf)

    fresh = db.connect(tmp_path / "b.sqlite"); db.init_schema(fresh)
    assert manifest.import_file(fresh, out) == 1
    row = fresh.execute("SELECT content_hash, filter FROM frames").fetchone()
    assert row == ("h1", "HaO3")

def test_filename_with_space_survives_roundtrip(tmp_path):
    leaf = tmp_path / "d" / "NGC 7635"; leaf.mkdir(parents=True)
    conn = db.connect(tmp_path / "a.sqlite"); db.init_schema(conn)
    _frame(conn, "h9", str(leaf / "Light_NGC 7635_300.0s_0001.fit"))
    out = manifest.export_dir(conn, leaf)
    fresh = db.connect(tmp_path / "b.sqlite"); db.init_schema(fresh)
    manifest.import_file(fresh, out)
    assert fresh.execute("SELECT filename FROM frames").fetchone()[0] \
        == "Light_NGC 7635_300.0s_0001.fit"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_manifest.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.manifest'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/manifest.py
import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path

MANIFEST_NAME = ".astro-manifest.json"
_FIELDS = ("content_hash", "filename", "size", "frame_type", "camera",
           "filter", "exptime", "captured_at", "fingerprint",
           "disposition", "disposition_reason", "disposition_source")

def export_dir(conn, leaf_dir: Path, out_dir: Path | None = None) -> Path:
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        f"SELECT {', '.join(_FIELDS)} FROM frames WHERE path LIKE ?",
        (f"{leaf_dir}/%",)).fetchall()
    doc = {
        "version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "leaf_dir": str(leaf_dir),
        "frames": [dict(r) for r in rows],
    }
    target = (out_dir or leaf_dir) / MANIFEST_NAME
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(doc, indent=2))
    return target

def export_all(conn, dry_run: bool = False) -> int:
    leaves = {Path(r[0]).parent for r in
              conn.execute("SELECT path FROM frames").fetchall()}
    if dry_run:
        return len(leaves)
    for leaf in leaves:
        export_dir(conn, leaf)
    return len(leaves)

def import_file(conn, manifest_path: Path) -> int:
    doc = json.loads(Path(manifest_path).read_text())
    leaf = Path(doc["leaf_dir"])
    n = 0
    for fr in doc["frames"]:
        conn.execute("""INSERT INTO frames (content_hash, path, filename,
            size, mtime, frame_type, camera, filter, exptime, captured_at,
            fingerprint, disposition, disposition_reason, disposition_source)
            VALUES (?,?,?,?,0.0,?,?,?,?,?,?,?,?,?)
            ON CONFLICT(content_hash) DO UPDATE SET
              filter=excluded.filter, camera=excluded.camera,
              disposition=excluded.disposition,
              disposition_reason=excluded.disposition_reason""",
            (fr["content_hash"], str(leaf / fr["filename"]), fr["filename"],
             fr["size"], fr["frame_type"], fr["camera"], fr["filter"],
             fr["exptime"], fr["captured_at"], fr["fingerprint"],
             fr["disposition"], fr["disposition_reason"],
             fr["disposition_source"]))
        n += 1
    conn.commit()
    return n
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_manifest.py -v`
Expected: PASS, 3 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/manifest.py tests/test_manifest.py
git commit -m "feat: per-leaf-dir manifest export and import"
```

---

### Task 13: CLI

**Files:**
- Create: `src/astrometa/cli.py`
- Modify: `pyproject.toml` (add console script entry point)
- Test: `tests/test_cli.py`

**Interfaces:**
- Consumes: every pass module
- Produces: console script `astrometa` with subcommands `scan`, `cluster`, `solve`, `measure`, `group`, `manifest`, `status`

`scan` runs inventory then `mark_missing` in one step, because the missing check is only meaningful against a completed walk.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_cli.py
import numpy as np
from astropy.io import fits
from astrometa import cli

def _write_light(path):
    rng = np.random.default_rng(3)
    hdu = fits.PrimaryHDU(rng.normal(100, 5, (64, 64)).astype(np.float32))
    hdu.header["OBJECT"] = "M 42"
    hdu.header["RA"] = 83.8
    hdu.header["DEC"] = -5.4
    hdu.header["IMAGETYP"] = "Light Frame"
    hdu.writeto(path, overwrite=True)

def test_scan_then_status(tmp_path, capsys):
    root = tmp_path / "astro_data" / "2026-09-08" / "M 42"
    root.mkdir(parents=True)
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"

    assert cli.main(["scan", "--db", str(dbp),
                     "--root", str(tmp_path / "astro_data")]) == 0
    assert cli.main(["status", "--db", str(dbp)]) == 0
    out = capsys.readouterr().out
    assert "frames" in out and "1" in out

def test_unknown_command_returns_nonzero(tmp_path):
    assert cli.main(["nonsense", "--db", str(tmp_path / "x.sqlite")]) != 0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_cli.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.cli'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/cli.py
import argparse
import sys
from pathlib import Path

from . import cluster, db, disposition, grouping, inventory, manifest
from . import quality, solve
from .config import Config

def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="astrometa")
    p.add_argument("command",
                   choices=["scan", "cluster", "solve", "measure",
                            "group", "manifest", "status"])
    p.add_argument("--db", required=True)
    p.add_argument("--root", action="append", default=[])
    p.add_argument("--limit", type=int, default=None)
    return p

def main(argv=None) -> int:
    parser = _build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit:
        return 2

    cfg = Config()
    conn = db.connect(Path(args.db))
    db.init_schema(conn)

    if args.command == "scan":
        roots = [Path(r) for r in args.root] or \
                [cfg.archive_root, cfg.live_root]
        res = inventory.scan(conn, roots)
        missing = disposition.mark_missing(conn, res.seen_hashes)
        print(f"added={res.added} updated={res.updated} "
              f"failed={res.failed} missing={missing}")
    elif args.command == "cluster":
        print(f"fields={cluster.assign_fields(conn)}")
    elif args.command == "solve":
        print(f"solved={solve.solve_fields(conn, cfg, args.limit)}")
    elif args.command == "measure":
        print(f"measured={quality.measure_frames(conn, cfg, args.limit)}")
    elif args.command == "group":
        print(f"projects={grouping.build_projects(conn)}")
    elif args.command == "manifest":
        print(f"manifests={manifest.export_all(conn)}")
    elif args.command == "status":
        for table in ("frames", "fields", "objects", "projects"):
            n = conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            print(f"{table}: {n}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

Add to `pyproject.toml`:

```toml
[project.scripts]
astrometa = "astrometa.cli:main"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/ -v`
Expected: PASS, all tests across every module

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/cli.py pyproject.toml tests/test_cli.py
git commit -m "feat: command-line interface"
```

---

### Task 14: First real run against the archive

**Files:**
- Create: `docs/runbook.md`
- No source changes

**Interfaces:**
- Consumes: the `astrometa` console script
- Produces: a populated `store.sqlite` and a written record of the first run's numbers

This task is deliberately last and deliberately manual: it is where the store meets 28,135 real files and where any number that disagrees with the spec gets investigated rather than explained away.

- [ ] **Step 1: Deploy to scott-server**

```bash
ssh root@192.168.68.53 'pct list'          # choose or create the LXC
# bind-mount the two sources read-only into the container:
#   pct set <id> -mp0 /data/backups/qnap,mp=/archive,ro=1
#   pct set <id> -mp1 /mnt/qnap-source,mp=/live,ro=1
```

- [ ] **Step 2: Run the inventory pass**

```bash
astrometa scan --db /data/astro-metadata/store.sqlite
```

Expected: `added` close to 28,135. Record the exact number.

- [ ] **Step 3: Reconcile against the known count**

The workstation and scott-server previously reported 28,135 vs 28,123 frames for the same archive. Compare `added + failed` against both. Investigate the difference by hash rather than choosing whichever number is convenient — this discrepancy is a recorded open item in the spec, and closing it is part of this task.

- [ ] **Step 4: Cluster and solve a sample**

```bash
astrometa cluster --db /data/astro-metadata/store.sqlite
astrometa solve   --db /data/astro-metadata/store.sqlite --limit 20
```

Verify a known field: the IC 1848 frames must solve to approximately
RA 42.8625, Dec 60.0697.

- [ ] **Step 5: Record results and commit the runbook**

Write `docs/runbook.md` capturing: the real frame count, the number of distinct fields, solve success rate, wall-clock per pass, and any frame that failed to read. Then:

```bash
git add docs/runbook.md
git commit -m "docs: first full archive run results"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §3 Decisions — SQLite on local ZFS | 1 |
| §4 Architecture — read-only bind mounts | 14 |
| §5 Three keys — content hash, WCS, fingerprint | 4, 7 |
| §6 Pipeline 1 Inventory | 5 |
| §6 Pipeline 2 Classify | 3 |
| §6 Pipeline 3 Headers | 2 |
| §6 Pipeline 4 Fingerprint | 4 |
| §6 Pipeline 5 Cluster by pointing | 6 |
| §6 Pipeline 6 Solve | 7 |
| §6 Pipeline 7 Name | 9 |
| §6 Pipeline 8 Measure quality | 8 |
| §6 Pipeline 9 Group | 10 |
| §7 Data model | 1 |
| §7 Confidence model | 9 |
| §8 Disposition, quarantine, thresholds, backfill | 11 |
| §9 Error handling | 5 (read errors), 7 (solve attempts), 11 (missing), 14 (count reconciliation) |
| §10 Testing | every task |
| §11 Non-goals | enforced by Global Constraints |

**Gap found and closed:** the spec's manifest durability requirement (§3) had no task in the first draft; it is now Task 12.

**Deferred deliberately:** SIMBAD network resolution is scaffolded in Task 9 (`upsert_object` accepts `simbad_id`/`ra`/`dec`) but the live query is not implemented, because it needs the first real solve results from Task 14 to calibrate the search radius. It is the first item of the follow-up plan, not a silent omission.

**Type consistency:** `content_hash` is a hex `str` everywhere; `fingerprint` is a hex `str` everywhere and compared only via `imagekeys.hamming`; `field_id` is `int`; disposition values are exactly `present` / `missing` / `quarantined` in Tasks 1, 5, 11 and 12.
