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

