import stat
from pathlib import Path

import pytest

from astrometa import db, quality
from astrometa.config import Config

# A stub "astap_cli" used in place of the real binary. It mimics only the
# -analyse behaviours these tests care about, all measured 2026-09-09
# against the real binary:
#   - a genuine measurement prints "HFD_MEDIAN=<f>\nSTARS=<n>\n" to stdout
#     and exits 0, WITHOUT writing any .ini sidecar at all (unlike a
#     solve, -analyse writes no sidecar on success)
#   - even a bare "-analyse" (no snr_min) works -- it is not a required arg
#   - a file-access failure (bad/missing/corrupt input) exits 1 and DOES
#     write an .ini next to the input, with an ERROR= line and no metrics
#     on stdout
# Controlled entirely through env vars (inherited from the test process),
# so one stub script covers every scenario below:
#   STUB_MODE=solved (default) -> prints STUB_STDOUT (or a canned
#                                   HFD_MEDIAN/STARS pair) to stdout, exits
#                                   STUB_EXIT_CODE (default 0), no sidecar
#   STUB_MODE=no_metrics         -> prints unrelated text to stdout, exits
#                                   0, no sidecar (simulates a run that
#                                   didn't crash but produced nothing
#                                   parseable)
#   STUB_MODE=error_ini          -> writes an .ini with ERROR=<STUB_ERROR>
#                                   next to the -f path, exits 1, no
#                                   metrics on stdout
#   STUB_MODE=crash               -> prints to stdout/stderr, writes no
#                                   .ini, exits 139
#   STUB_MODE=sleep                -> sleeps STUB_SLEEP seconds, writes
#                                   nothing (exercises the timeout path
#                                   without waiting on the real 60s)
_STUB_SOURCE = '''#!/usr/bin/env python3
import os
import sys
import time
from pathlib import Path

def main():
    argv = sys.argv[1:]
    mode = os.environ.get("STUB_MODE", "solved")
    if mode == "sleep":
        time.sleep(float(os.environ.get("STUB_SLEEP", "5")))
        return
    if mode == "crash":
        print("simulated astap_cli crash: assertion failed at foo.pas:123")
        print("segmentation fault detected", file=sys.stderr)
        sys.exit(139)
    if mode == "no_metrics":
        print("Using star database D50")
        print("Done")
        return

    f_path = Path(argv[argv.index("-f") + 1])
    if mode == "error_ini":
        err = os.environ.get("STUB_ERROR", "Error reading image file.")
        f_path.with_suffix(".ini").write_text(
            f"\\nPLTSOLVD=F\\nCMDLINE=x\\nERROR={err}\\n")
        sys.exit(1)

    stdout = os.environ.get("STUB_STDOUT", "HFD_MEDIAN=3.9\\nSTARS=240\\n")
    sys.stdout.write(stdout)
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
                  astap_bin=astap_bin, astap_db_dir=db_dir)


# --- parse_analyse -----------------------------------------------------

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


# --- measure_frame -------------------------------------------------------

def test_measure_frame_stub_solver_leaves_source_directory_clean(tmp_path, monkeypatch):
    # astap_cli writes sidecars next to whatever -f path it is given, and
    # the archive mount is read-only by design, so measure_frame must
    # copy the frame to scratch before invoking astap_cli.
    source_dir = tmp_path / "read_only_source"
    source_dir.mkdir()
    frame = source_dir / "frame.fit"
    frame.write_bytes(b"pretend this is FITS data, the stub never reads it")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")

    cfg = _cfg(tmp_path, stub)
    result = quality.measure_frame(cfg, frame)

    assert result["ok"] is True
    assert result["hfd_median"] == 3.9
    assert result["star_count"] == 240
    assert [p.name for p in source_dir.iterdir()] == ["frame.fit"]


def test_measure_frame_zero_stars_is_still_a_genuine_measurement(tmp_path, monkeypatch):
    # Measured 2026-09-09 against the real binary on an actual bias
    # frame: HFD_MEDIAN=21.5 STARS=0, exit 0, no .ini. A field with no
    # detectable stars is a legitimate result, not a failure -- it must
    # still be written.
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    monkeypatch.setenv("STUB_STDOUT", "HFD_MEDIAN=21.5\nSTARS=0\n")

    cfg = _cfg(tmp_path, stub)
    result = quality.measure_frame(cfg, frame)

    assert result["ok"] is True
    assert result["hfd_median"] == 21.5
    assert result["star_count"] == 0


def test_measure_frame_unparseable_output_is_a_failure_not_a_measurement(tmp_path, monkeypatch):
    # Exit 0 but nothing parseable on stdout: must not be silently
    # reported as a zero-star measurement -- that would fabricate data
    # astap_cli never actually reported.
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "no_metrics")

    cfg = _cfg(tmp_path, stub)
    result = quality.measure_frame(cfg, frame)

    assert result["ok"] is False
    assert result["hfd_median"] is None
    assert result["star_count"] is None
    assert "exit 0" in result["error"]


def test_measure_frame_crash_carries_exit_code_and_output(tmp_path, monkeypatch):
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "crash")

    cfg = _cfg(tmp_path, stub)
    result = quality.measure_frame(cfg, frame)

    assert result["ok"] is False
    assert "exit 139" in result["error"]
    assert "simulated astap_cli crash" in result["error"]


def test_measure_frame_environment_failure_carries_ini_error(tmp_path, monkeypatch):
    # Reproduces the shape measured against the real binary for a bad
    # input file: an .ini IS written even though a successful -analyse
    # writes none at all, with an ERROR field and exit code 1.
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "error_ini")
    monkeypatch.setenv("STUB_ERROR", "Error reading image file.")

    cfg = _cfg(tmp_path, stub)
    result = quality.measure_frame(cfg, frame)

    assert result["ok"] is False
    assert "Error reading image file." in result["error"]
    assert "exit 1" in result["error"]


def test_measure_frame_records_timeout_loudly(tmp_path, monkeypatch):
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "sleep")
    monkeypatch.setenv("STUB_SLEEP", "2")
    monkeypatch.setattr(quality, "TIMEOUT_S", 0.2)

    cfg = _cfg(tmp_path, stub)
    result = quality.measure_frame(cfg, frame)

    assert result["ok"] is False
    assert "timeout" in result["error"]


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
def test_measure_frame_against_real_astap(tmp_path):
    frame = _find_real_frame()
    if frame is None:
        pytest.skip("no archive frame reachable on this host")

    cfg = Config(scratch_dir=tmp_path / "scratch",
                 astap_bin=REAL_ASTAP, astap_db_dir=REAL_ASTAP.parent)

    result = quality.measure_frame(cfg, frame)

    assert result["ok"] is True
    assert result["hfd_median"] is not None
    assert result["star_count"] is not None


# --- measure_frames --------------------------------------------------------

def _insert_frame(conn, content_hash: str, path: Path, frame_type="light",
                   read_error=None, bg_median=None) -> None:
    conn.execute("""
        INSERT INTO frames (content_hash, path, filename, size, mtime,
            frame_type, disposition, read_error, bg_median)
        VALUES (?,?,?,1,1.0,?,'present',?,?)""",
        (content_hash, str(path), path.name, frame_type, read_error, bg_median))


def test_measure_frames_writes_quality_row_for_a_genuine_measurement(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_frame(conn, "h1", frame, bg_median=123.4)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    cfg = _cfg(tmp_path, stub)

    res = quality.measure_frames(conn, cfg)
    assert res.measured == 1
    assert res.failed == 0

    row = conn.execute(
        "SELECT star_count, hfd_median, sky_background, measured_at "
        "FROM quality WHERE content_hash='h1'").fetchone()
    assert row[0] == 240
    assert row[1] == 3.9
    assert row[2] == 123.4          # copied straight from frames.bg_median
    assert row[3] is not None

    # source frame is never touched
    assert [p.name for p in source_dir.iterdir()] == ["frame.fit"]


def test_measure_frames_only_considers_light_frames_without_read_error(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    dark = source_dir / "dark.fit"; dark.write_bytes(b"x")
    broken = source_dir / "broken.fit"; broken.write_bytes(b"x")
    _insert_frame(conn, "dark1", dark, frame_type="dark")
    _insert_frame(conn, "broken1", broken, frame_type="light",
                  read_error="pixels: OSError: truncated")

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    cfg = _cfg(tmp_path, stub)

    res = quality.measure_frames(conn, cfg)
    assert res.measured == 0
    assert res.failed == 0
    assert conn.execute("SELECT COUNT(*) FROM quality").fetchone()[0] == 0


def test_measure_frames_is_idempotent(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_frame(conn, "h1", frame)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    cfg = _cfg(tmp_path, stub)

    assert quality.measure_frames(conn, cfg).measured == 1
    # re-run finds nothing new: the frame already has a quality row
    assert quality.measure_frames(conn, cfg).measured == 0
    assert conn.execute("SELECT COUNT(*) FROM quality").fetchone()[0] == 1


def test_measure_frames_failure_writes_no_row_and_stays_a_candidate(tmp_path, monkeypatch):
    # The core correction this task exists to enforce: a NULL-metric row
    # would permanently mark the frame measured-with-no-data (the
    # candidate query is a LEFT JOIN ... WHERE quality.content_hash IS
    # NULL) and it would never be retried. A failed measurement must
    # leave no row at all.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_frame(conn, "h1", frame)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "no_metrics")
    cfg = _cfg(tmp_path, stub)

    res = quality.measure_frames(conn, cfg)
    assert res.measured == 0
    assert res.failed == 1
    assert conn.execute("SELECT COUNT(*) FROM quality").fetchone()[0] == 0

    # a later run (e.g. once the environment is fixed) retries it
    monkeypatch.setenv("STUB_MODE", "solved")
    res2 = quality.measure_frames(conn, cfg)
    assert res2.measured == 1
    assert res2.failed == 0
    assert conn.execute("SELECT COUNT(*) FROM quality").fetchone()[0] == 1


def test_measure_frames_respects_limit(tmp_path, monkeypatch):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame1 = source_dir / "frame1.fit"; frame1.write_bytes(b"x")
    frame2 = source_dir / "frame2.fit"; frame2.write_bytes(b"x")
    _insert_frame(conn, "h1", frame1)
    _insert_frame(conn, "h2", frame2)

    stub = _make_stub(tmp_path)
    monkeypatch.setenv("STUB_MODE", "solved")
    cfg = _cfg(tmp_path, stub)

    res = quality.measure_frames(conn, cfg, limit=1)
    assert res.measured == 1
    assert conn.execute("SELECT COUNT(*) FROM quality").fetchone()[0] == 1


def test_measure_frames_raises_on_missing_astap_binary(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_frame(conn, "h1", frame)

    missing_bin = tmp_path / "does_not_exist_astap_cli"
    cfg = _cfg(tmp_path, missing_bin)

    with pytest.raises(RuntimeError, match="astap_bin"):
        quality.measure_frames(conn, cfg)

    assert conn.execute("SELECT COUNT(*) FROM quality").fetchone()[0] == 0


def test_measure_frames_raises_on_missing_astap_db_dir(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    source_dir = tmp_path / "source"; source_dir.mkdir()
    frame = source_dir / "frame.fit"; frame.write_bytes(b"x")
    _insert_frame(conn, "h1", frame)

    stub = _make_stub(tmp_path)
    cfg = Config(scratch_dir=tmp_path / "scratch", astap_bin=stub,
                 astap_db_dir=tmp_path / "does_not_exist_db_dir")

    with pytest.raises(RuntimeError, match="astap_db_dir"):
        quality.measure_frames(conn, cfg)

    assert conn.execute("SELECT COUNT(*) FROM quality").fetchone()[0] == 0
