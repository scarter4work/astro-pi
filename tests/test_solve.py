import stat
from pathlib import Path

import pytest

from astrometa import db, solve
from astrometa.config import Config

FIXTURE = Path(__file__).parent / "fixtures" / "solved.ini"

# A stub "astap_cli" used in place of the real binary. It never touches
# star catalogs -- it only mimics the one behaviour these tests care about:
# astap_cli writes its .ini sidecar NEXT TO whatever file it was given via
# -f. Controlled entirely through env vars (inherited from the test
# process, since solve_frame's subprocess.run call passes no explicit env)
# so one stub script covers every scenario below:
#   STUB_MODE=solved (default)  -> writes STUB_INI_TEXT (or a canned
#                                   PLTSOLVD=T block) next to the -f path
#   STUB_MODE=no_ini             -> writes nothing (simulates an
#                                   unsolvable frame -- no star field to
#                                   match, e.g. Moon/planet frames)
#   STUB_MODE=sleep              -> sleeps STUB_SLEEP seconds, writes
#                                   nothing (used to exercise the timeout
#                                   path without waiting on the real 120s)
#   STUB_ARGV_LOG=<path>         -> if set, records the argv it was
#                                   invoked with, so tests can assert on
#                                   the -ra/-spd/-r conversion
_STUB_SOURCE = '''#!/usr/bin/env python3
import os
import sys
import time
from pathlib import Path

def main():
    argv = sys.argv[1:]
    log = os.environ.get("STUB_ARGV_LOG")
    if log:
        Path(log).write_text(" ".join(argv))

    mode = os.environ.get("STUB_MODE", "solved")
    if mode == "sleep":
        time.sleep(float(os.environ.get("STUB_SLEEP", "5")))
        return
    if mode == "no_ini":
        return

    f_path = Path(argv[argv.index("-f") + 1])
    ini_text = os.environ.get(
        "STUB_INI_TEXT",
        "PLTSOLVD=T\\nCRVAL1=10.0\\nCRVAL2=20.0\\nCDELT1=0.0003\\nCROTA1=1.5\\n",
    )
    f_path.with_suffix(".ini").write_text(ini_text)

if __name__ == "__main__":
    main()
'''


def _make_stub(tmp_path: Path) -> Path:
    stub = tmp_path / "stub_astap.py"
    stub.write_text(_STUB_SOURCE)
    mode = stub.stat().st_mode
    stub.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return stub


def _cfg(tmp_path: Path, astap_bin: Path) -> Config:
    return Config(scratch_dir=tmp_path / "scratch",
                  astap_bin=astap_bin,
                  astap_db_dir=tmp_path / "astap_db")


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


def test_solve_frame_stub_solver_leaves_source_directory_clean(tmp_path, monkeypatch):
    # The constraint that matters: the archive mount is read-only by
    # design, so solve_frame must copy the frame to scratch before
    # invoking astap_cli, never hand astap_cli the original source path.
    source_dir = tmp_path / "read_only_source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"pretend this is FITS data, the stub never reads it")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    monkeypatch.setenv("STUB_INI_TEXT", FIXTURE.read_text())

    cfg = _cfg(tmp_path, stub)
    result = solve.solve_frame(cfg, frame, hint=(42.8625007, 60.0697047))

    assert result["solved"] is True
    assert abs(result["ra"] - 42.8625007) < 1e-6
    assert abs(result["dec"] - 60.0697047) < 1e-6

    # no .ini/.wcs sidecar (or anything else) ever appeared beside the
    # original source file
    assert [p.name for p in source_dir.iterdir()] == ["frame.fit"]


def test_solve_frame_hint_converts_to_hours_and_spd(tmp_path, monkeypatch):
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    log = tmp_path / "argv.log"
    monkeypatch.setenv("STUB_ARGV_LOG", str(log))
    monkeypatch.setenv("STUB_MODE", "solved")

    cfg = _cfg(tmp_path, stub)
    solve.solve_frame(cfg, frame, hint=(180.0, 45.0))

    argv = log.read_text().split()
    assert argv[argv.index("-ra") + 1] == str(180.0 / 15.0)
    assert argv[argv.index("-spd") + 1] == str(45.0 + 90.0)
    assert argv[argv.index("-r") + 1] == "30"


def test_solve_frame_blind_search_uses_wide_radius_when_no_hint(tmp_path, monkeypatch):
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    log = tmp_path / "argv.log"
    monkeypatch.setenv("STUB_ARGV_LOG", str(log))
    monkeypatch.setenv("STUB_MODE", "solved")

    cfg = _cfg(tmp_path, stub)
    solve.solve_frame(cfg, frame, hint=None)

    argv = log.read_text().split()
    assert "-ra" not in argv
    assert "-spd" not in argv
    assert argv[argv.index("-r") + 1] == "180"


def test_solve_frame_no_ini_produced_is_recorded_as_unsolved(tmp_path, monkeypatch):
    # Simulates a genuinely unsolvable frame (Moon/planet/bright star --
    # no star field to match): astap_cli exits without ever writing an
    # .ini, so this must be reported as a normal (not exceptional) failure.
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "no_ini")

    cfg = _cfg(tmp_path, stub)
    result = solve.solve_frame(cfg, frame, hint=None)

    assert result["solved"] is False
    assert result["warning"] == "no .ini produced"


def test_solve_frame_records_timeout_loudly(tmp_path, monkeypatch):
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "sleep")
    monkeypatch.setenv("STUB_SLEEP", "2")
    monkeypatch.setattr(solve, "TIMEOUT_S", 0.2)

    cfg = _cfg(tmp_path, stub)
    result = solve.solve_frame(cfg, frame, hint=None)

    assert result["solved"] is False
    assert "timeout" in result["warning"]


REAL_ASTAP = Path("/opt/astap/astap_cli")


def _find_real_frame() -> Path | None:
    root = Path("/mnt/qnap/astro_data")
    if not root.exists():
        return None
    for p in root.rglob("Light_*.fit"):
        return p
    return None


@pytest.mark.skipif(not REAL_ASTAP.exists(),
                     reason="astap_cli not installed on this host")
def test_solve_frame_against_real_astap(tmp_path):
    frame = _find_real_frame()
    if frame is None:
        pytest.skip("no archive frame reachable on this host")

    from astrometa import fitsheader

    header = fitsheader.read_header(frame)
    hint = (header.get("RA"), header.get("DEC"))
    cfg = Config(scratch_dir=tmp_path / "scratch",
                 astap_bin=REAL_ASTAP, astap_db_dir=REAL_ASTAP.parent)

    result = solve.solve_frame(cfg, frame, hint=hint)

    assert result["solved"] is True
    assert result["ra"] is not None
    assert result["dec"] is not None


def _insert_field_with_rep(conn, frame_path: Path, header_ra=42.8625007,
                            header_dec=60.0697047) -> int:
    cur = conn.execute("INSERT INTO fields (solve_source) VALUES ('none')")
    fid = cur.lastrowid
    conn.execute("""
        INSERT INTO frames (content_hash, path, filename, size, mtime,
            frame_type, disposition, header_ra, header_dec, field_id)
        VALUES (?,?,?,1,1.0,'light','present',?,?,?)""",
        (f"h{fid}", str(frame_path), frame_path.name, header_ra, header_dec,
         fid))
    return fid


def test_solve_fields_writes_solution_and_marks_solved(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    fid = _insert_field_with_rep(conn, frame)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    monkeypatch.setenv("STUB_INI_TEXT", FIXTURE.read_text())
    cfg = _cfg(tmp_path, stub)

    assert solve.solve_fields(conn, cfg) == 1
    row = conn.execute(
        "SELECT solved_ra, solved_dec, scale_arcsec_px, rotation, "
        "solve_source, solve_at, solve_attempts, solve_error "
        "FROM fields WHERE id=?", (fid,)).fetchone()
    assert row[0] == pytest.approx(42.8625007, abs=1e-6)
    assert row[1] == pytest.approx(60.0697047, abs=1e-6)
    assert row[4] == "astap"
    assert row[5] is not None
    assert row[6] == 1
    assert row[7] is None

    # the source frame is never touched
    assert [p.name for p in source_dir.iterdir()] == ["frame.fit"]


def test_solve_fields_is_idempotent(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_field_with_rep(conn, frame)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    monkeypatch.setenv("STUB_INI_TEXT", FIXTURE.read_text())
    cfg = _cfg(tmp_path, stub)

    assert solve.solve_fields(conn, cfg) == 1
    # re-run finds nothing new: the field is already solved
    assert solve.solve_fields(conn, cfg) == 0


def test_solve_fields_records_failure_without_marking_solved(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    fid = _insert_field_with_rep(conn, frame)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "no_ini")
    cfg = _cfg(tmp_path, stub)

    assert solve.solve_fields(conn, cfg) == 0
    row = conn.execute(
        "SELECT solve_source, solve_attempts, solve_error "
        "FROM fields WHERE id=?", (fid,)).fetchone()
    assert row[0] == "none"
    assert row[1] == 1
    assert row[2] == "no .ini produced"


def test_solve_fields_stops_retrying_after_max_attempts(tmp_path, monkeypatch):
    # A genuinely unsolvable frame (e.g. Moon) must not be retried
    # forever, and must be distinguishable from a frame that simply
    # hasn't been processed yet.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    fid = _insert_field_with_rep(conn, frame)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "no_ini")
    cfg = _cfg(tmp_path, stub)

    for _ in range(3):
        solve.solve_fields(conn, cfg)
    attempts = conn.execute(
        "SELECT solve_attempts FROM fields WHERE id=?", (fid,)).fetchone()[0]
    assert attempts == 3

    # a 4th run must not touch it again
    assert solve.solve_fields(conn, cfg) == 0
    assert conn.execute(
        "SELECT solve_attempts FROM fields WHERE id=?",
        (fid,)).fetchone()[0] == 3


def test_solve_fields_respects_limit(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame1 = source_dir / "frame1.fit"; frame1.write_bytes(b"x")
    frame2 = source_dir / "frame2.fit"; frame2.write_bytes(b"x")
    _insert_field_with_rep(conn, frame1)
    _insert_field_with_rep(conn, frame2)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    monkeypatch.setenv("STUB_INI_TEXT", FIXTURE.read_text())
    cfg = _cfg(tmp_path, stub)

    assert solve.solve_fields(conn, cfg, limit=1) == 1
    solved_count = conn.execute(
        "SELECT COUNT(*) FROM fields WHERE solve_source='astap'").fetchone()[0]
    assert solved_count == 1
