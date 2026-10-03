r"""
Session and project grouping: bucket light frames by (object, filter,
night) into `projects`, distinguishing a single-pointing session from a
multi-panel mosaic campaign.

"Object" here is fields.object_id when it is resolved and a recorded
per-frame fallback while it is not -- see `frame_identity`. That
fallback is load-bearing rather than defensive: nothing populates
object_id yet (live SIMBAD resolution is deferred), so bucketing on it
alone put every target shot on one night with one filter into a single
"project". Each project records which identity named it, so a
directory- or OBJECT-card-derived label is never mistaken for a solved
one.

The dusk rule (session_date) fixes a repeated real-world failure:
imaging sessions cross midnight, so a run starting at 01:39 carries only
the NEXT day's datestamp in its filename. A naive date filter then
mis-files or re-pulls a whole night's data. Folders in this archive are
named for the night a session STARTED, so an instant before dusk_hour
(default 17:00) is folded back onto the previous calendar day -- this
was verified against a real session (SH2-129, 2026-07): 100 frames
captured 22:54-01:39 all carrying the following day's datestamp, which
must land in one project rather than two.

Panels matter too: `Light_IC 1848_1-1_...` is mosaic panel row 1, column
1 -- a single target can have dozens (M42 alone has 77 panel directories
in this archive, including double-digit rows/columns like `1-10` and
`7-12`). A bucket containing any panel-tagged frame is a "mosaic"
project; one with none is a plain "session". The panel regex anchors on
the `_<digits>-<digits>_<exptime>s_` shape so it doesn't mis-fire on a
target whose own name contains digits and a hyphen, e.g.
`Light_Sh2-106_120.0s_..._` has no panel component, nor on a datestamp
token (`_20260713-225546_182deg_` fails the required `\d+(\.\d+)?s_`
suffix). Row/col groups are `\d+`, not `\d`: measured against the real
archive (2026-09-09), M42 alone has 120 frames across 24 distinct
double-digit panel tokens (`1-10` through `8-12`) that a single-digit
pattern silently drops to `None`, which would flatten those panels into
one undifferentiated "session" bucket -- precisely the failure the panel
level exists to prevent.

build_projects is idempotent by full rebuild, not by dedup-on-insert:
`projects` and `frame_projects` are wholly derived from `frames`, so
clearing and reinserting them from scratch on every call is
deterministic given the same frame data, and needs no schema change.
Deduplicating on (object_id, filter, started_at) was considered and
rejected -- started_at is the MINIMUM capture instant in a bucket, which
shifts whenever frames captured earlier the same night are pulled in on
a later incremental run, so it isn't a stable key across runs.
"""
import re
import sqlite3
from dataclasses import dataclass
from datetime import date, datetime, timedelta

@dataclass
class GroupingResult:
    """What one build_projects rebuild produced.

    A bare project count hid two things worth seeing: how each bucket's
    target was identified, and how many frames were dropped for having
    no parseable capture instant.
    """
    projects: int = 0
    # Light frames dropped from every project because their filename
    # carries no `_YYYYMMDD-HHMMSS_` token. Legitimately unplaceable
    # (ASIAIR autosaves have no capture stamp anywhere), but silence
    # made a frame vanishing from the grouping indistinguishable from
    # there being nothing to place.
    skipped_no_capture_instant: int = 0


_INSTANT = re.compile(r"_(\d{8})-(\d{6})_")
_PANEL = re.compile(r"_(\d+-\d+)_\d+(?:\.\d+)?s_")


def capture_instant(filename: str) -> datetime | None:
    """Parse the `_YYYYMMDD-HHMMSS_` capture timestamp out of a filename.

    Returns None when the pattern is absent -- a frame with no parseable
    capture instant is legitimately skipped by build_projects, not an
    error, since not every file in the archive carries this stamp (e.g.
    ASIAIR autosave files). build_projects counts those skips and
    reports them (GroupingResult.skipped_no_capture_instant) so
    "legitimately skipped" never means "silently gone".
    """
    m = _INSTANT.search(filename)
    if not m:
        return None
    return datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S")


def session_date(instant: datetime, dusk_hour: int = 17) -> date:
    """Which night `instant` belongs to, folding pre-dusk hours back a day.

    An instant at or after dusk_hour belongs to that calendar day's
    night. An instant before dusk_hour (e.g. 01:49) belongs to the
    PREVIOUS day's night -- it's the tail end of a session that started
    the evening before. The boundary at dusk_hour itself counts as the
    same night (inclusive).
    """
    if instant.hour < dusk_hour:
        return (instant - timedelta(days=1)).date()
    return instant.date()


def panel_of(filename: str) -> str | None:
    """Extract a `ROW-COL` mosaic panel tag (e.g. "1-1", "1-10") from a
    filename.

    Anchored on `_<digits>-<digits>_<exptime>s_` so a target name that
    itself contains a digit-hyphen-digit run (e.g. `Sh2-106`) is never
    mistaken for a panel tag -- the exposure-time suffix immediately
    after the candidate is required. Row/col each match one or more
    digits, not exactly one: real panels in this archive run into double
    digits (M42 has panels up to `8-12`).
    """
    m = _PANEL.search(filename)
    return m.group(1) if m else None


def frame_identity(object_id, object_card, leaf_dir) -> tuple[str | None, str]:
    """
    The target identity to bucket one frame on, and where it came from.

    `object_id` is authoritative when set, and the returned name is then
    None -- the FK carries the identity, and two differently-spelled
    directories for one resolved object (M31 and M 31 are real sibling
    directories here) must still land in one project.

    Nothing populates fields.object_id yet: live SIMBAD resolution is
    deliberately deferred, so in practice object_id is NULL for every
    frame in the archive today. Bucketing on it alone therefore put
    EVERY target shot on one night with one filter into a single
    "project". The fallbacks are the OBJECT card and then the leaf
    directory name -- both are the target name in practice -- and the
    source is recorded alongside so a fallback label is never mistaken
    for a solved one. Per spec 7 neither may ever drive a Phase 2 move.
    """
    if object_id is not None:
        return None, "object"
    if object_card:
        return object_card, "object_card"
    if leaf_dir:
        return leaf_dir, "dirname"
    return None, "unknown"


def build_projects(conn) -> GroupingResult:
    """
    Rebuild `projects` and `frame_projects` from `frames`, grouping light
    frames into per-night sessions (or multi-panel mosaics) keyed on
    (object identity, filter, session_date(capture_instant)) -- where the
    object identity is fields.object_id when resolved and a recorded
    per-frame fallback (see `frame_identity`) while it is not.

    Idempotent: this is a full rebuild of both tables (see module
    docstring), so calling it repeatedly with unchanged frame data
    reproduces the same projects and memberships every time, and calling
    it after new frames were added folds them into the correct existing
    night rather than creating a duplicate project for it.

    Returns a GroupingResult, not a bare count -- see its docstring.
    A frame with no parseable capture instant is skipped, as before, but
    now counted and reported rather than dropped in silence.
    """
    prev_factory = conn.row_factory
    conn.row_factory = sqlite3.Row
    try:
        rows = conn.execute(
            "SELECT f.content_hash, f.filename, f.filter, f.object_card, "
            "f.leaf_dir, fl.object_id "
            "FROM frames f LEFT JOIN fields fl ON fl.id = f.field_id "
            "WHERE f.frame_type = 'light'").fetchall()

        buckets: dict[tuple, list] = {}
        identity_sources: dict[tuple, str] = {}
        skipped = 0
        for r in rows:
            inst = capture_instant(r["filename"])
            if inst is None:
                # Not an error -- but counted, never silent.
                skipped += 1
                continue
            identity_name, identity_source = frame_identity(
                r["object_id"], r["object_card"], r["leaf_dir"])
            key = (r["object_id"], identity_name, r["filter"],
                   session_date(inst))
            identity_sources[key] = identity_source
            buckets.setdefault(key, []).append(
                (r["content_hash"], panel_of(r["filename"]), inst))

        # Full rebuild: both tables are wholly derived from frames, so
        # clearing and reinserting is deterministic and idempotent by
        # construction. frame_projects first -- it references projects.
        conn.execute("DELETE FROM frame_projects")
        conn.execute("DELETE FROM projects")

        for key, members in buckets.items():
            object_id, identity_name, filt, _session_date = key
            identity_source = identity_sources[key]
            kind = "mosaic" if any(p for _, p, _ in members) else "session"
            instants = [i for _, _, i in members]
            cur = conn.execute(
                "INSERT INTO projects (object_id, filter, kind, started_at, "
                "ended_at, identity_name, identity_source) "
                "VALUES (?,?,?,?,?,?,?)",
                (object_id, filt, kind, min(instants).isoformat(),
                 max(instants).isoformat(), identity_name, identity_source))
            pid = cur.lastrowid
            for chash, panel, _inst in members:
                conn.execute(
                    "INSERT OR REPLACE INTO frame_projects (content_hash, "
                    "project_id, panel) VALUES (?,?,?)",
                    (chash, pid, panel))
        conn.commit()
        return GroupingResult(projects=len(buckets),
                              skipped_no_capture_instant=skipped)
    finally:
        conn.row_factory = prev_factory
