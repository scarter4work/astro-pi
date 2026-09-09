r"""
Disposition tracking, quarantine, and historical cull backfill.

This is the direct fix for the failure that motivated the whole project.
The archive lives in two places (an ASIAIR camera and a NAS), and a prior
audit compared filenames between them: "present on the camera, absent
from the NAS" was treated as evidence a transfer had dropped frames. But
the operator also deletes bad subs from the NAS by hand, which produces
the exact same signature. On 2026-09-07 the operator manually culled 39
IC 59 frames; on 2026-09-09 the audit flagged the resulting gap and
queued an automated "repair" that would have silently re-added frames
the operator had just deliberately thrown out. It was only luck of
timing that stopped it running.

The filesystem alone can't express *why* a frame is absent -- disk
absence and operator intent are indistinguishable without a place to
record the intent. This module is that place: a frame that disappears
from a scan becomes `missing` (mark_missing), never dropped from the
database; an operator (or an automated quality gate) can then record
*why* a frame was rejected (mark_culled / quarantine); and
backfill_known_culls retroactively applies that record to culls that
already happened before this system existed, so the audit stops raising
false alarms on them forever after.

`quarantine` is the *only* archive-mutating function in this entire
system -- every other module only reads. It moves a frame into a sibling
`rejected/` directory next to the frame (matching the operator's
existing manual practice) and never deletes anything; `dry_run=True` is
the default, and a dry run touches neither the filesystem nor the
database. Machine deletion is out of scope permanently, not just for
this phase.

`mark_missing`'s empty-seen_hashes guard: mark_missing flips every frame
still `present` that isn't in `seen_hashes` to `missing`. Task 5's
inventory.scan() already raises if a scan root doesn't exist, which
closes today's path to seen_hashes arriving empty -- but that closes one
*cause*, not the *class*. If seen_hashes ever arrives empty while the
database holds present frames -- a caller that bypasses scan(), a walk
that silently returns nothing, a future refactor -- the correct
conclusion is "the scan failed", never "the operator deleted the entire
archive". Since `missing` is precisely how a deliberate cull gets
recorded, that misreading would be indistinguishable from the very
catastrophe this module exists to describe accurately, so mark_missing
refuses to run rather than guess. It does NOT additionally guard a
"suspiciously small but non-empty" seen_hashes -- see mark_missing's
docstring for why a fixed fraction-based cutoff was rejected rather than
silently chosen.

session_thresholds is deliberately per-session, never a fixed global
limit: seeing varies night to night, so a single fixed HFD ceiling would
cull an entire mediocre night while keeping junk from an excellent one.
It reports (hfd_limit, absolute_floor) rather than a single number so a
caller can see both the night-relative limit and the hard ceiling that
overrides it when a session is too small or too noisy to trust its own
statistics.

backfill_known_culls takes the cull list as a plain parameter and stays
generic -- the real historical list (39 IC 59 frames, night of
2026-09-07) is deliberately kept out of this module's source; callers
supply it. That real list is itself a trap for a careless pattern: its
Lqef and HaO3 filter sub-lists overlap on frame index (0038, 0039, 0040
appear in both), so a `filename LIKE` pattern keyed on index alone would
over-match across filters and quarantine the wrong frame. Patterns must
include the filter token, not just the index -- see
tests/test_disposition.py for a case proving two same-index,
different-filter frames are told apart.
"""
import shutil
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from statistics import median, pstdev

# session_thresholds constants, per the task brief.
HFD_SIGMA = 2.0
ABSOLUTE_HFD_FLOOR = 10.0


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def mark_missing(conn: sqlite3.Connection, seen_hashes: set[str]) -> int:
    """
    Flip every currently-`present` frame absent from `seen_hashes` to
    `missing`. Never deletes a row and never touches a frame that is
    already `missing` or `quarantined` -- only `present` frames are
    candidates, so a frame already dispositioned keeps its recorded
    disposition (and reason/source) rather than being silently
    overwritten by a later scan that simply didn't walk that far.

    Raises ValueError, touching nothing, if `seen_hashes` is empty (or
    otherwise falsy) while the database still holds `present` frames --
    see the module docstring for why. When the database holds no
    `present` frames, an empty seen_hashes is legitimate (there is
    nothing to protect) and this returns 0 rather than raising.

    A threshold beyond strict emptiness (e.g. refusing when seen_hashes
    covers only a suspiciously small fraction of present frames) was
    considered and deliberately NOT added: any fixed fraction is exactly
    the kind of global, un-derived limit session_thresholds exists to
    avoid for HFD, for the same reason -- how much of one night's data
    is *legitimately* culled varies, so a fixed cutoff would either be
    loose enough to miss a real scan failure or tight enough to reject a
    real, large, deliberate cull. Task 5's inventory.scan() already
    raises loudly on the concrete way this happens today (a missing scan
    root); if a new failure mode produces a small-but-nonempty
    seen_hashes, it should get its own named guard when it's observed,
    not a number picked in advance without evidence.
    """
    if not seen_hashes:
        present = conn.execute(
            "SELECT COUNT(*) FROM frames WHERE disposition='present'"
        ).fetchone()[0]
        if present > 0:
            raise ValueError(
                f"mark_missing refused: seen_hashes is empty but "
                f"{present} frame(s) are currently 'present' in the "
                f"database. An empty scan result almost certainly means "
                f"the scan itself failed (e.g. an unmounted archive "
                f"root) -- see inventory.scan's own guard against a "
                f"missing root -- not that the operator deleted the "
                f"entire archive. Fix the scan; don't run mark_missing "
                f"against its empty result.")

    n = 0
    for (h,) in conn.execute(
            "SELECT content_hash FROM frames WHERE disposition='present'"
            ).fetchall():
        if h not in seen_hashes:
            conn.execute("""UPDATE frames SET disposition='missing',
                disposition_at=? WHERE content_hash=?""", (_now(), h))
            n += 1
    conn.commit()
    return n


def mark_culled(conn: sqlite3.Connection, content_hash: str, reason: str,
                 source: str) -> None:
    """Record a frame as deliberately rejected, with why and by whom/what."""
    conn.execute("""UPDATE frames SET disposition='quarantined',
        disposition_reason=?, disposition_source=?, disposition_at=?
        WHERE content_hash=?""", (reason, source, _now(), content_hash))
    conn.commit()


def quarantine(conn: sqlite3.Connection, content_hash: str, reason: str,
               dry_run: bool = True) -> Path | None:
    """
    Move a frame's file into a sibling `rejected/` directory and record
    it as quarantined. The only archive-mutating function in this
    system -- moves, never deletes.

    Returns the destination Path (whether or not dry_run actually moved
    anything), or None if content_hash isn't in the database at all --
    that case is a caller error (an unknown/mistyped hash), not
    something to raise on, since `frames` rows are never deleted and so
    a genuine hash is never legitimately "missing from the table".

    If a matching row IS found but its recorded file isn't actually on
    disk, this raises FileNotFoundError -- in both dry_run and real
    mode. That disagreement (DB says the frame lives at `path`,
    filesystem disagrees) is exactly the kind of silent-fallback trap
    this task exists to close: a dry run that reported a plausible
    `dest` for a move that could never actually happen would be lying
    about what a real run would do, undermining the one function in
    this system that's supposed to be trustworthy enough to run for
    real. In a real (dry_run=False) run this check is redundant with
    what shutil.move would raise anyway, but making it explicit keeps
    dry_run and real runs honest about the same failure the same way.
    """
    row = conn.execute("SELECT path FROM frames WHERE content_hash=?",
                       (content_hash,)).fetchone()
    if row is None:
        return None

    src = Path(row[0])
    if not src.exists():
        raise FileNotFoundError(
            f"quarantine: frame {content_hash} is recorded at {src} but "
            f"that file does not exist -- refusing to report or perform "
            f"a move that couldn't actually happen. Check whether this "
            f"frame was already moved/quarantined or the archive path "
            f"has drifted.")

    dest = src.parent / "rejected" / src.name
    if dry_run:
        return dest

    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(src), str(dest))      # move, never delete
    conn.execute("""UPDATE frames SET path=?, disposition='quarantined',
        disposition_reason=?, disposition_source='auto', disposition_at=?
        WHERE content_hash=?""", (str(dest), reason, _now(), content_hash))
    conn.commit()
    return dest


def session_thresholds(hfds: list[float]) -> tuple[float, float]:
    """
    Derive (hfd_limit, absolute_floor) for one imaging session's own HFD
    measurements, rather than applying one fixed limit to every night.

    hfd_limit is the session's median HFD plus HFD_SIGMA population
    standard deviations -- frames far softer than the rest of that same
    night's own frames, regardless of how good or bad the night was
    overall. absolute_floor (ABSOLUTE_HFD_FLOOR) is a hard ceiling that
    a caller should apply in addition, since a session with fewer than 3
    HFD samples (or none) can't support a meaningful median/stdev at
    all -- in that case both returned values collapse to
    ABSOLUTE_HFD_FLOOR so the caller falls back to the one global
    number rather than trusting statistics computed from noise.
    """
    vals = [v for v in hfds if v is not None]
    if len(vals) < 3:
        return ABSOLUTE_HFD_FLOOR, ABSOLUTE_HFD_FLOOR
    med = median(vals)
    sd = pstdev(vals) or 0.0
    return med + HFD_SIGMA * sd, ABSOLUTE_HFD_FLOOR


def backfill_known_culls(conn: sqlite3.Connection, culls: list[dict]) -> int:
    """
    Apply a list of {"filename_like": ..., "reason": ...} historical cull
    records to matching frames via mark_culled(source="manual").

    Generic and data-free by design -- see module docstring for why the
    real cull list stays out of this source file, and why each pattern
    must include the filter token (not just the frame index) to avoid
    over-matching across filters that happen to share an index.

    Returns the number of (pattern, matching frame) pairs applied. A
    frame already quarantined that matches again (e.g. this is run
    twice) is counted and re-recorded, not skipped -- mark_culled is
    idempotent (it just rewrites the same reason/source), so re-running
    this is safe, but the count reports matches made, not "newly
    culled".
    """
    n = 0
    for c in culls:
        for (h,) in conn.execute(
                "SELECT content_hash FROM frames WHERE filename LIKE ?",
                (c["filename_like"],)).fetchall():
            mark_culled(conn, h, c["reason"], "manual")
            n += 1
    return n
