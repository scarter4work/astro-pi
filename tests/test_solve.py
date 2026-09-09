import stat
from pathlib import Path

import pytest

from astrometa import db, solve
from astrometa.config import Config

FIXTURE = Path(__file__).parent / "fixtures" / "solved.ini"

# A stub "astap_cli" used in place of the real binary. It never touches
# star catalogs -- it only mimics the behaviours these tests care about:
# astap_cli writes its .ini sidecar NEXT TO whatever file it was given via
# -f, and (measured 2026-09-09 against the real binary) returns exit code
# 1 for BOTH a genuinely unsolvable frame and an environment problem
# (missing star database, unreadable input) while still writing an .ini
# with an ERROR field for the latter. Controlled entirely through env
# vars (inherited from the test process, since solve_frame's
# subprocess.run call passes no explicit env) so one stub script covers
# every scenario below:
#   STUB_MODE=solved (default)  -> prints a line to stdout, writes
#                                   STUB_INI_TEXT (or a canned PLTSOLVD=T
#                                   block) next to the -f path, exits
#                                   STUB_EXIT_CODE (default 0)
#   STUB_MODE=no_ini             -> writes nothing, exits 0 (simulates a
#                                   crash-free run that still produced no
#                                   sidecar)
#   STUB_MODE=crash              -> prints to stdout/stderr, writes no
#                                   .ini, exits 139 (simulates a hard
#                                   crash -- the case _diagnostics exists
#                                   for)
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
    if mode == "crash":
        print("simulated astap_cli crash: assertion failed at foo.pas:123")
        print("segmentation fault detected", file=sys.stderr)
        sys.exit(139)

    f_path = Path(argv[argv.index("-f") + 1])
    print(f"stub astap_cli solving {f_path.name}")
    ini_text = os.environ.get(
        "STUB_INI_TEXT",
        "PLTSOLVD=T\\nCRVAL1=10.0\\nCRVAL2=20.0\\nCDELT1=0.0003\\nCROTA1=1.5\\n",
    )
    f_path.with_suffix(".ini").write_text(ini_text)
    sys.exit(int(os.environ.get("STUB_EXIT_CODE", "0")))

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
    db_dir = tmp_path / "astap_db"
    db_dir.mkdir(exist_ok=True)
    return Config(scratch_dir=tmp_path / "scratch",
                  astap_bin=astap_bin,
                  astap_db_dir=db_dir)


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


def test_parse_ini_surfaces_error_field_on_failure():
    # Measured 2026-09-09 against the real binary: `-d /nonexistent-dir`
    # produces exactly this shape -- PLTSOLVD=F plus an ERROR field, no
    # WARNING. ERROR is astap_cli's own diagnosis of an environment
    # problem and must not be dropped.
    r = solve.parse_ini("PLTSOLVD=F\nCMDLINE=x\nERROR=No star database found.\n")
    assert r["solved"] is False
    assert r["warning"] == "No star database found."


def test_parse_ini_prefers_error_over_warning():
    r = solve.parse_ini("PLTSOLVD=F\nERROR=db missing\nWARNING=some other text\n")
    assert r["warning"] == "db missing"


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
    # Defensive fallback path: astap_cli exits cleanly without ever
    # writing an .ini at all. The real binary was measured (2026-09-09)
    # to still write an .ini even for a genuinely unsolvable Moon frame
    # (see test_solve_frame_genuine_no_solution_carries_only_diagnostics
    # below), so this covers the code path's handling of the case where
    # even that doesn't happen -- this must still be reported as a
    # normal (not exceptional) failure, with its exit code recorded.
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "no_ini")

    cfg = _cfg(tmp_path, stub)
    result = solve.solve_frame(cfg, frame, hint=None)

    assert result["solved"] is False
    assert result["warning"] == "no .ini produced (exit 0)"


def test_solve_frame_no_ini_produced_includes_exit_code_and_output(tmp_path, monkeypatch):
    # A hard crash (missing shared library, segfault, ...): no .ini, but
    # astap_cli's stdout/stderr before it died is exactly the diagnostic
    # an operator needs, and it must not be discarded.
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "crash")

    cfg = _cfg(tmp_path, stub)
    result = solve.solve_frame(cfg, frame, hint=None)

    assert result["solved"] is False
    assert "no .ini produced" in result["warning"]
    assert "exit 139" in result["warning"]
    assert "simulated astap_cli crash" in result["warning"]


def test_solve_frame_environment_failure_carries_ini_error_and_diagnostics(tmp_path, monkeypatch):
    # Reproduces the exact shape measured against the real binary for a
    # bad database directory: an .ini IS produced, PLTSOLVD=F, with an
    # ERROR field and exit code 1 -- the same exit code a genuinely
    # unsolvable frame produces, so the ERROR text plus diagnostics is
    # what must make it into solve_error, not a bare "solve failed".
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    monkeypatch.setenv("STUB_EXIT_CODE", "1")
    monkeypatch.setenv("STUB_INI_TEXT",
                        "PLTSOLVD=F\nERROR=No star database found.\n")

    cfg = _cfg(tmp_path, stub)
    result = solve.solve_frame(cfg, frame, hint=None)

    assert result["solved"] is False
    assert "No star database found." in result["warning"]
    assert "exit 1" in result["warning"]


def test_solve_frame_genuine_no_solution_carries_only_diagnostics(tmp_path, monkeypatch):
    # Reproduces the exact .ini shape measured against the real binary
    # for a genuinely unsolvable Moon frame: PLTSOLVD=F, exit 1, but NO
    # ERROR or WARNING field -- astap_cli has nothing more specific to
    # say. This must still carry the exit code/output diagnostics (never
    # a bare, unhelpful "solve failed"), which is also what makes it
    # distinguishable from the ERROR case above: no structured reason
    # here, just the raw diagnostic tail.
    source_dir = tmp_path / "source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    monkeypatch.setenv("STUB_EXIT_CODE", "1")
    monkeypatch.setenv("STUB_INI_TEXT", "PLTSOLVD=F\n")

    cfg = _cfg(tmp_path, stub)
    result = solve.solve_frame(cfg, frame, hint=None)

    assert result["solved"] is False
    assert result["warning"].startswith("exit 1")


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
    assert row[2] == "no .ini produced (exit 0)"


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


def test_solve_fields_raises_on_missing_astap_binary(tmp_path):
    # The poisoning scenario this guards against: a misconfigured
    # install would otherwise fail every field identically to a genuine
    # unsolvable frame, burning the whole attempt cap across the archive
    # on the very first run. Preflighting must catch this BEFORE
    # touching a single field.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_field_with_rep(conn, frame)

    missing_bin = tmp_path / "does_not_exist_astap_cli"
    cfg = _cfg(tmp_path, missing_bin)

    with pytest.raises(RuntimeError, match="astap_bin"):
        solve.solve_fields(conn, cfg)

    row = conn.execute(
        "SELECT solve_attempts, solve_source FROM fields").fetchone()
    assert row[0] == 0
    assert row[1] == "none"


def test_solve_fields_raises_on_missing_astap_db_dir(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_field_with_rep(conn, frame)

    stub = _make_stub(tmp_path)
    cfg = Config(scratch_dir=tmp_path / "scratch", astap_bin=stub,
                 astap_db_dir=tmp_path / "does_not_exist_db_dir")

    with pytest.raises(RuntimeError, match="astap_db_dir"):
        solve.solve_fields(conn, cfg)

    row = conn.execute(
        "SELECT solve_attempts, solve_source FROM fields").fetchone()
    assert row[0] == 0
    assert row[1] == "none"
