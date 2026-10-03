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

