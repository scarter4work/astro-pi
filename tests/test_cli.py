import sqlite3
import stat
from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from astrometa import cli, manifest
from astrometa.config import Config


def _write_light(path, object_name="M 42", ra=83.8, dec=-5.4, seed=3):
    rng = np.random.default_rng(seed)
    hdu = fits.PrimaryHDU(rng.normal(100, 5, (64, 64)).astype(np.float32))
    hdu.header["OBJECT"] = object_name
    hdu.header["RA"] = ra
    hdu.header["DEC"] = dec
    hdu.header["IMAGETYP"] = "Light Frame"
    hdu.writeto(path, overwrite=True)


def _stub(tmp_path, source, name):
    p = tmp_path / name
    p.write_text(source)
    mode = p.stat().st_mode
    p.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return p


def _use_roots(monkeypatch, tmp_path, archive, live):
    """Point cli.Config's archive/live roots at test directories.

    A scan that passes no --root walks exactly these two, which is the
    condition under which the whole-database missing sweep is allowed to
    run (see cli.main).
    """
    fake = Config(archive_root=archive, live_root=live,
                  scratch_dir=tmp_path / "scratch")
    monkeypatch.setattr(cli, "Config", lambda: fake)
    return fake

# Stub astap_cli for -wcs solving: writes a solved .ini next to whatever
# path it was given via -f, ignoring every other argument. Good enough to
# exercise the CLI's wiring to solve.solve_fields without depending on a
# real star database.
_SOLVE_STUB = '''#!/usr/bin/env python3
import sys
from pathlib import Path
argv = sys.argv[1:]
f_path = Path(argv[argv.index("-f") + 1])
f_path.with_suffix(".ini").write_text(
    "PLTSOLVD=T\\nCRVAL1=10.0\\nCRVAL2=20.0\\nCDELT1=0.0003\\nCROTA1=1.5\\n")
sys.exit(0)
'''

# Stub astap_cli for -analyse: always reports a clean measurement.
_MEASURE_STUB = '''#!/usr/bin/env python3
print("HFD_MEDIAN=2.5")
print("STARS=40")
'''


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


def test_missing_db_flag_returns_nonzero(tmp_path):
    assert cli.main(["status"]) != 0


def test_scan_missing_root_raises_loudly(tmp_path):
    # inventory.scan raises FileNotFoundError for a missing root -- the
    # CLI must let this propagate, not swallow it into a tidy message or
    # a zero exit code.
    dbp = tmp_path / "store.sqlite"
    with pytest.raises(FileNotFoundError):
        cli.main(["scan", "--db", str(dbp),
                  "--root", str(tmp_path / "does_not_exist")])


def test_rescan_against_empty_configured_roots_raises_loudly(tmp_path,
                                                              monkeypatch):
    # A rescan whose walk finds nothing, while the db still holds
    # 'present' frames, must not silently mark everything missing --
    # disposition.mark_missing refuses with ValueError, and the CLI must
    # surface that, not catch it.
    #
    # This is driven through the CONFIGURED roots rather than --root: a
    # --root scan is a deliberately narrowed one and no longer runs the
    # whole-database missing sweep at all (see
    # test_narrowed_root_scan_does_not_mark_unscanned_frames_missing),
    # so the guard now lives on the path that still performs the sweep.
    archive = tmp_path / "astro_data"
    archive.mkdir()
    live = tmp_path / "live"
    live.mkdir()
    _write_light(archive / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"
    _use_roots(monkeypatch, tmp_path, archive, live)
    assert cli.main(["scan", "--db", str(dbp)]) == 0

    empty_a = tmp_path / "empty_a"
    empty_a.mkdir()
    empty_b = tmp_path / "empty_b"
    empty_b.mkdir()
    _use_roots(monkeypatch, tmp_path, empty_a, empty_b)
    with pytest.raises(ValueError):
        cli.main(["scan", "--db", str(dbp)])


def test_scan_reports_failed_overlapping_added_not_disjoint(tmp_path, capsys):
    # inventory.scan's `failed` count overlaps `added`/`updated` -- a
    # frame that fails a read is still inventoried. The CLI's scan output
    # must not read as if the three were disjoint counts.
    root = tmp_path / "astro_data"
    root.mkdir()
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    bad = root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220100_0deg_0002.fit"
    bad.write_bytes(b"not a real fits file")
    dbp = tmp_path / "store.sqlite"

    assert cli.main(["scan", "--db", str(dbp), "--root", str(root)]) == 0
    out = capsys.readouterr().out
    assert "added=2" in out
    assert "failed=1" in out
    assert "overlap" in out


def test_cluster_labels_newly_created_fields(tmp_path, capsys):
    # cluster.assign_fields returns fields created THIS call, not the
    # total -- a re-run must read as "0 new", not "no fields exist".
    root = tmp_path / "astro_data"
    root.mkdir()
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"
    assert cli.main(["scan", "--db", str(dbp), "--root", str(root)]) == 0

    assert cli.main(["cluster", "--db", str(dbp)]) == 0
    assert "fields_created=1" in capsys.readouterr().out

    assert cli.main(["cluster", "--db", str(dbp)]) == 0
    assert "fields_created=0" in capsys.readouterr().out


def test_group_reports_project_count(tmp_path, capsys):
    root = tmp_path / "astro_data"
    root.mkdir()
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"
    assert cli.main(["scan", "--db", str(dbp), "--root", str(root)]) == 0

    assert cli.main(["group", "--db", str(dbp)]) == 0
    assert "projects=1" in capsys.readouterr().out


def test_manifest_out_dir_mirrors_leaf_structure(tmp_path, capsys):
    # export_all(out_dir=...) matters operationally: the deployment
    # mounts the archive read-only, so an out-dir mirror is the working
    # path, not a nice-to-have.
    root = tmp_path / "astro_data" / "2026-09-08" / "M 42"
    root.mkdir(parents=True)
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"
    out_dir = tmp_path / "manifests"
    out_dir.mkdir()

    assert cli.main(["scan", "--db", str(dbp),
                     "--root", str(tmp_path / "astro_data")]) == 0
    assert cli.main(["manifest", "--db", str(dbp),
                     "--out-dir", str(out_dir)]) == 0
    assert "manifests=1" in capsys.readouterr().out

    found = list(out_dir.rglob(manifest.MANIFEST_NAME))
    assert len(found) == 1
    # nothing written beside the (read-only, in production) archive frames
    assert not (root / manifest.MANIFEST_NAME).exists()


def test_solve_reports_solved_count(tmp_path, capsys, monkeypatch):
    root = tmp_path / "astro_data"
    root.mkdir()
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"
    assert cli.main(["scan", "--db", str(dbp), "--root", str(root)]) == 0
    assert cli.main(["cluster", "--db", str(dbp)]) == 0

    astap_db = tmp_path / "astap_db"
    astap_db.mkdir()
    stub = _stub(tmp_path, _SOLVE_STUB, "stub_solve.py")
    fake_cfg = Config(scratch_dir=tmp_path / "scratch", astap_bin=stub,
                       astap_db_dir=astap_db)
    monkeypatch.setattr(cli, "Config", lambda: fake_cfg)

    assert cli.main(["solve", "--db", str(dbp)]) == 0
    assert "solved=1" in capsys.readouterr().out


def test_measure_reports_measured_and_failed(tmp_path, capsys, monkeypatch):
    root = tmp_path / "astro_data"
    root.mkdir()
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"
    assert cli.main(["scan", "--db", str(dbp), "--root", str(root)]) == 0

    astap_db = tmp_path / "astap_db"
    astap_db.mkdir()
    stub = _stub(tmp_path, _MEASURE_STUB, "stub_measure.py")
    fake_cfg = Config(scratch_dir=tmp_path / "scratch", astap_bin=stub,
                       astap_db_dir=astap_db)
    monkeypatch.setattr(cli, "Config", lambda: fake_cfg)

    assert cli.main(["measure", "--db", str(dbp)]) == 0
    assert "measured=1 failed=0" in capsys.readouterr().out


def test_solve_missing_astap_raises_loudly(tmp_path, monkeypatch):
    dbp = tmp_path / "store.sqlite"
    fake_cfg = Config(scratch_dir=tmp_path / "scratch",
                       astap_bin=tmp_path / "no_such_binary",
                       astap_db_dir=tmp_path / "no_such_db")
    monkeypatch.setattr(cli, "Config", lambda: fake_cfg)
    with pytest.raises(RuntimeError):
        cli.main(["solve", "--db", str(dbp)])


def test_measure_missing_astap_raises_loudly(tmp_path, monkeypatch):
    dbp = tmp_path / "store.sqlite"
    fake_cfg = Config(scratch_dir=tmp_path / "scratch",
                       astap_bin=tmp_path / "no_such_binary",
                       astap_db_dir=tmp_path / "no_such_db")
    monkeypatch.setattr(cli, "Config", lambda: fake_cfg)
    with pytest.raises(RuntimeError):
        cli.main(["measure", "--db", str(dbp)])


# --- FIX 2: a narrowed scan must not run the whole-DB missing sweep ---


def test_narrowed_root_scan_does_not_mark_unscanned_frames_missing(tmp_path):
    # `astrometa scan --root /archive/astro_data/2026-09-08` walked one
    # night but then ran mark_missing across the WHOLE database, flipping
    # every frame outside that night to 'missing'. mark_missing's own
    # guard only trips on a completely empty seen_hashes, so it cannot
    # catch this. `missing` is how a deliberate cull is recorded -- the
    # single signal this project exists to make trustworthy.
    archive = tmp_path / "astro_data"
    night_a = archive / "2026-09-07" / "IC 59"; night_a.mkdir(parents=True)
    night_b = archive / "2026-09-08" / "M 42"; night_b.mkdir(parents=True)
    _write_light(night_a / "Light_IC 59_120.0s_HaO3_20260907-220000_0deg_0001.fit",
                 object_name="IC 59", seed=1)
    _write_light(night_b / "Light_M 42_120.0s_HaO3_20260908-220000_0deg_0001.fit",
                 object_name="M 42", seed=2)
    dbp = tmp_path / "store.sqlite"

    assert cli.main(["scan", "--db", str(dbp), "--root", str(archive)]) == 0

    # Rescan only the second night.
    assert cli.main(["scan", "--db", str(dbp), "--root", str(night_b)]) == 0

    conn = sqlite3.connect(dbp)
    dispositions = {r[0] for r in conn.execute(
        "SELECT disposition FROM frames")}
    assert dispositions == {"present"}


def test_narrowed_root_scan_says_the_missing_sweep_was_skipped(tmp_path, capsys):
    # Skipping the sweep must be visible in the output, never silent --
    # an operator has to be able to tell that `missing` was not refreshed.
    root = tmp_path / "astro_data"; root.mkdir()
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    dbp = tmp_path / "store.sqlite"

    assert cli.main(["scan", "--db", str(dbp), "--root", str(root)]) == 0
    out = capsys.readouterr().out
    assert "missing=skipped" in out
    assert "--root" in out


def test_full_configured_root_scan_still_runs_the_missing_sweep(
        tmp_path, capsys, monkeypatch):
    # The sweep is not disabled -- a scan covering the configured roots
    # still marks a vanished frame missing, which is the whole point of
    # the disposition column.
    archive = tmp_path / "archive"; archive.mkdir()
    live = tmp_path / "live"; live.mkdir()
    kept = archive / "Light_M 42_120.0s_HaO3_20260908-220000_0deg_0001.fit"
    gone = archive / "Light_M 42_120.0s_HaO3_20260908-220100_0deg_0002.fit"
    _write_light(kept, seed=1)
    _write_light(gone, seed=2)
    dbp = tmp_path / "store.sqlite"
    _use_roots(monkeypatch, tmp_path, archive, live)

    assert cli.main(["scan", "--db", str(dbp)]) == 0
    gone.unlink()
    assert cli.main(["scan", "--db", str(dbp)]) == 0
    out = capsys.readouterr().out
    assert "missing=1" in out

    conn = sqlite3.connect(dbp)
    rows = dict(conn.execute("SELECT filename, disposition FROM frames"))
    assert rows[kept.name] == "present"
    assert rows[gone.name] == "missing"


def test_group_reports_frames_skipped_for_lack_of_a_capture_instant(
        tmp_path, capsys):
    root = tmp_path / "astro_data" / "M 42"
    root.mkdir(parents=True)
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    _write_light(root / "Autosave001.fit", seed=9)
    dbp = tmp_path / "store.sqlite"
    assert cli.main(["scan", "--db", str(dbp), "--root", str(root)]) == 0

    assert cli.main(["group", "--db", str(dbp)]) == 0
    out = capsys.readouterr().out
    assert "projects=1" in out
    assert "skipped_no_capture_instant=1" in out
