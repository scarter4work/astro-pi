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

