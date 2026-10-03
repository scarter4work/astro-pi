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

