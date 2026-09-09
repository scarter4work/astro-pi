"""
ASTAP plate-solving: ground truth for where a field actually points.

FITS header RA/DEC are the mount's COMMANDED position -- measured 10.6
arcminutes off the true pointing on a real frame -- so they are only ever
used for coarse clustering (see cluster.py) and as a search hint here,
never treated as identity. A plate solve matches the star geometry in the
pixel data against a catalogue and is authoritative.

Task 6 clusters frames into fields; this module solves ONE representative
frame per field and lets the result stand for the whole field, which is
what keeps solving ~34,000 frames affordable: a solve with a positional
hint (-r 30) costs ~0.1s, a blind search (-r 180) ~11.7s, both measured
2026-09-09 on the deployment host. Always pass a hint when header
coordinates exist.

astap_cli writes its .ini/.wcs sidecars NEXT TO the input file (verified
behaviour). The archive mounts are read-only by design and must stay free
of solver droppings, so solve_frame always copies the frame into
cfg.scratch_dir first and only ever hands astap_cli that scratch copy.

Roughly 405 frames in this archive (Moon, planets, bright stars) have no
star field to match and cannot be solved at all -- one measured taking 90s
to fail blind. solve_fields caps attempts per field so those don't get
retried forever, and records the failure reason so "permanently
unsolvable" (solve_attempts hit the cap, solve_error set) stays
distinguishable from "not yet processed" (solve_attempts still 0).
"""
import shutil
import sqlite3
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path

BLIND_RADIUS = 180
HINT_RADIUS = 30
TIMEOUT_S = 120
MAX_SOLVE_ATTEMPTS = 3

_UNSOLVED = {"solved": False, "ra": None, "dec": None,
             "scale_arcsec_px": None, "rotation": None, "warning": None}


def parse_ini(text: str) -> dict:
    """Parse an astap_cli .ini sidecar into a solution dict."""
    kv: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            kv[k.strip()] = v.strip()
    if kv.get("PLTSOLVD") != "T":
        return {**_UNSOLVED, "warning": kv.get("WARNING")}
    return {
        "solved": True,
        "ra": float(kv["CRVAL1"]),
        "dec": float(kv["CRVAL2"]),
        # CDELT1 is degrees/px in the .ini; callers want arcsec/px.
        "scale_arcsec_px": abs(float(kv["CDELT1"])) * 3600.0,
        "rotation": float(kv["CROTA1"]),
        "warning": kv.get("WARNING"),
    }


def solve_frame(cfg, frame_path: Path, hint: tuple[float, float] | None = None) -> dict:
    """
    Plate-solve one frame and return its solution (or a `solved: False`
    failure dict -- never raises for an ordinary "couldn't solve this
    frame", only for genuine environment failures such as a missing
    astap_cli binary).

    `hint`, when given, is (ra_degrees, dec_degrees) from the frame's FITS
    header -- the mount's commanded position, close enough to narrow the
    search radius from a 180-degree blind search down to 30 degrees.

    The frame is always copied into cfg.scratch_dir before astap_cli ever
    sees it: the archive mount frame_path lives on is read-only by design,
    and astap_cli writes its .ini/.wcs sidecars next to whatever path it
    is given, so solving in place would litter the archive.
    """
    cfg.scratch_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cfg.scratch_dir) as tmp:
        work = Path(tmp) / "solve.fit"
        shutil.copy2(frame_path, work)          # sources are read-only
        cmd = [str(cfg.astap_bin), "-f", str(work), "-d",
               str(cfg.astap_db_dir), "-fov", "0", "-wcs"]
        if hint and hint[0] is not None and hint[1] is not None:
            hint_ra_degrees, hint_dec = hint
            cmd += ["-ra", str(hint_ra_degrees / 15.0),
                    "-spd", str(hint_dec + 90.0),
                    "-r", str(HINT_RADIUS)]
        else:
            cmd += ["-r", str(BLIND_RADIUS)]
        try:
            subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return {**_UNSOLVED, "warning": f"timeout after {TIMEOUT_S}s"}
        ini = work.with_suffix(".ini")
        if not ini.exists():
            return {**_UNSOLVED, "warning": "no .ini produced"}
        return parse_ini(ini.read_text())


def solve_fields(conn, cfg, limit: int | None = None) -> int:
    """
    Solve one representative frame for every field not yet solved (or not
    yet permanently given up on), and write the result back.

    Idempotent: the candidate query only ever selects
    solve_source='none' AND solve_attempts < MAX_SOLVE_ATTEMPTS, so a
    solved field (solve_source='astap') is never re-solved, and a field
    that has exhausted its attempts is never retried, on a re-run.

    Returns the number of fields newly solved by this call.
    """
    prev_factory = conn.row_factory
    conn.row_factory = sqlite3.Row
    try:
        q = ("SELECT id FROM fields WHERE solve_source='none' "
             f"AND solve_attempts < {MAX_SOLVE_ATTEMPTS} ORDER BY id")
        if limit is not None:
            q += f" LIMIT {int(limit)}"
        field_ids = [r["id"] for r in conn.execute(q).fetchall()]

        solved = 0
        for fid in field_ids:
            rep = conn.execute(
                "SELECT path, header_ra, header_dec FROM frames "
                "WHERE field_id=? AND read_error IS NULL LIMIT 1",
                (fid,)).fetchone()
            if rep is None:
                continue
            res = solve_frame(cfg, Path(rep["path"]),
                               (rep["header_ra"], rep["header_dec"]))
            now = datetime.now(timezone.utc).isoformat()
            if res["solved"]:
                conn.execute("""
                    UPDATE fields SET solved_ra=?, solved_dec=?,
                        scale_arcsec_px=?, rotation=?, solve_source='astap',
                        solve_at=?, solve_attempts=solve_attempts+1,
                        solve_error=NULL
                    WHERE id=?""",
                    (res["ra"], res["dec"], res["scale_arcsec_px"],
                     res["rotation"], now, fid))
                solved += 1
            else:
                conn.execute("""
                    UPDATE fields SET solve_attempts=solve_attempts+1,
                        solve_error=?
                    WHERE id=?""",
                    (res["warning"] or "solve failed", fid))
            conn.commit()
        return solved
    finally:
        conn.row_factory = prev_factory
