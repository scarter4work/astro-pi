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

