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

