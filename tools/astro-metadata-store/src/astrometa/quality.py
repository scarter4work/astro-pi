"""
Per-frame image quality via astap_cli -analyse: star count and median HFD
(a focus/seeing proxy), feeding automatic culling decisions downstream.

-analyse only detects and characterizes stars in the frame; it does not
match against the star catalog, so unlike solve.py's full solve it needs
no -d database argument at all (verified 2026-09-09 against the real
binary: -analyse with -d pointed at a nonexistent directory still
succeeds). A successful -analyse run also writes no .ini sidecar --
HFD_MEDIAN and STARS print straight to stdout, e.g. "HFD_MEDIAN=3.9" /
"STARS=240". Note -analyse2 performs a full plate solve instead and must
not be used here -- this task only wants star-detection stats, not a
solve (Task 7 already owns solving, once per field, which is a much more
expensive operation).

Sky background is NOT re-derived here: frames.bg_median was already
computed from the same pixel read during Task 5's inventory pass, so
measure_frames reuses that column and this module never decodes pixel
data itself.

astap_cli returns exit code 1 for a genuine access/read failure just as
it does for other environment problems (see solve.py's module
docstring), and -- measured 2026-09-09 against a corrupt/missing input --
it still writes an .ini with an ERROR field in that case, even though a
successful -analyse run writes no sidecar at all. So a genuine
measurement (including a legitimate "zero stars found" -- measured
against a real bias frame: HFD_MEDIAN=21.5 STARS=0, exit 0) is
distinguished from a failure by exit code 0 together with both metrics
present in stdout; parse_analyse alone can't make that call reliably,
since a process could exit 0 with corrupted/unexpected stdout containing
no metrics, so measure_frame folds the exit code in too.

measure_frames only ever writes a quality row for a genuine measurement.
A subprocess failure (timeout, crash, unparseable output) writes nothing
-- see the module-level correction this encodes: measure_frames selects
candidates via a LEFT JOIN against quality on content_hash, so a
NULL-metric row would otherwise permanently mark a frame "measured" with
no data and it would never be retried.

Reuses solve._preflight (same poisoning risk this module faces: a
misconfigured astap_cli install is indistinguishable from "no stars
found" without it) and solve._diagnostics (same bounded stdout/stderr
tail behaviour) rather than duplicating either.
"""
import re
import shutil
import sqlite3
import subprocess
import tempfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from .solve import _diagnostics, _preflight

TIMEOUT_S = 60
_HFD = re.compile(r"^HFD_MEDIAN=([0-9.]+)", re.MULTILINE)
_STARS = re.compile(r"^STARS=([0-9]+)", re.MULTILINE)


@dataclass
class QualityResult:
    measured: int = 0
    failed: int = 0


def parse_analyse(stdout: str) -> dict:
    """Extract HFD_MEDIAN/STARS from astap_cli -analyse stdout."""
    h = _HFD.search(stdout or "")
    s = _STARS.search(stdout or "")
    return {"hfd_median": float(h.group(1)) if h else None,
            "star_count": int(s.group(1)) if s else None}


def _ini_error(ini: Path) -> str | None:
    """astap_cli's own ERROR= line from a failure .ini, when present."""
    if not ini.exists():
        return None
    for line in ini.read_text().splitlines():
        if line.startswith("ERROR="):
            return line.partition("=")[2].strip()
    return None


def measure_frame(cfg, frame_path: Path) -> dict:
    """
    Run astap_cli -analyse on one frame and return its metrics.

    Always returns {"hfd_median", "star_count", "ok", "error"}. `ok` is
    True only for a genuine measurement -- exit code 0 with both metrics
    present in stdout, which includes a legitimate "no stars found"
    result (hfd_median/star_count are still real numbers then, just an
    uninformative HFD with star_count 0). Never raises for an ordinary
    measurement failure; only an environment problem the caller's
    preflight should already have caught would raise.
    """
    cfg.scratch_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cfg.scratch_dir) as tmp:
        work = Path(tmp) / "quality.fit"
        shutil.copy2(frame_path, work)          # sources are read-only
        cmd = [str(cfg.astap_bin), "-f", str(work), "-analyse"]
        try:
            result = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return {"hfd_median": None, "star_count": None, "ok": False,
                    "error": f"timeout after {TIMEOUT_S}s"}

        stdout = (result.stdout or b"").decode("utf-8", "replace")
        metrics = parse_analyse(stdout)
        if (result.returncode == 0 and metrics["hfd_median"] is not None
                and metrics["star_count"] is not None):
            return {**metrics, "ok": True, "error": None}

        # Failure: fold astap_cli's own .ini ERROR field (written even
        # for -analyse on an access failure -- see module docstring)
        # together with the exit code/output tail, so the reason is
        # never a bare, unhelpful "solve failed".
        reason = _ini_error(work.with_suffix(".ini"))
        diag = _diagnostics(result)
        error = f"{reason} ({diag})" if reason else diag
        return {"hfd_median": None, "star_count": None, "ok": False,
                "error": error}


def measure_frames(conn, cfg, limit: int | None = None) -> QualityResult:
    """
    Measure quality for every light frame not yet measured.

    Idempotent: candidates come from a LEFT JOIN against quality on
    content_hash, WHERE quality.content_hash IS NULL -- a frame that
    already has a quality row (written only for a genuine measurement,
    see measure_frame) is never re-measured, and a frame whose
    measurement failed writes no row so it stays a candidate on the next
    run.

    Returns a QualityResult(measured, failed) rather than a bare count,
    so a caller can tell "nothing left to do" apart from "N frames are
    still stuck failing" -- both look identical as a single int.
    """
    _preflight(cfg)
    prev_factory = conn.row_factory
    conn.row_factory = sqlite3.Row
    try:
        q = ("SELECT f.content_hash, f.path, f.bg_median FROM frames f "
             "LEFT JOIN quality q ON q.content_hash = f.content_hash "
             "WHERE f.frame_type='light' AND f.read_error IS NULL "
             "AND q.content_hash IS NULL ORDER BY f.content_hash")
        params: list = []
        if limit is not None:
            q += " LIMIT ?"
            params.append(int(limit))
        rows = conn.execute(q, params).fetchall()

        res = QualityResult()
        for row in rows:
            m = measure_frame(cfg, Path(row["path"]))
            if not m["ok"]:
                res.failed += 1
                continue
            conn.execute("""
                INSERT INTO quality (content_hash, star_count, hfd_median,
                    sky_background, measured_at)
                VALUES (?,?,?,?,?)""",
                (row["content_hash"], m["star_count"], m["hfd_median"],
                 row["bg_median"], datetime.now(timezone.utc).isoformat()))
            res.measured += 1
            conn.commit()
        return res
    finally:
        conn.row_factory = prev_factory
