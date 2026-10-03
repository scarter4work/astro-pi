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

