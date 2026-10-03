import math
import sqlite3

from .imagekeys import hamming

DEFAULT_FOV_DEG = 1.0

# Fingerprint (dHash) match tolerance for clustering frames into a field.
#
# Measured 2026-09-09 against real archive frames (ASI585MC Air, 3 targets,
# 6 frames each, 2026-09-08 session):
#   same-field pairs:      min 20, med 26-50, max 143 (the 143 outlier is a
#                           meridian-flipped frame -- 182 deg vs 1 deg
#                           rotation -- which is expected to NOT cluster;
#                           excluding it, the same-field max is 45)
#   different-field pairs:  min 113, med 126-129, max 138
# 12 (the value used while this pass was first drafted, before it was
# checked against real data) is far too strict: dithering, seeing, and
# noise make even genuinely identical pointings hash 20+ bits apart, so at
# 12 no two real frames would ever cluster and every frame would become
# its own field. 80 sits well above the observed same-field maximum (45)
# and well below the observed between-field minimum (113), erring toward
# the cheaper mistake: a threshold set too low only costs an extra plate
# solve (Task 7), while one set too high risks merging frames of different
# objects into the same field. Fingerprint is also ANDed with pointing
# proximity (see assign_fields), which is the stronger discriminator, so
# 80 is a safe margin rather than an aggressive one. A meridian-flipped
# frame splitting into its own field is accepted, not fixed here --
# rotation-invariant hashing is out of scope.
FP_THRESHOLD_DEFAULT = 80

# How many failing frames an error message names before it truncates.
MAX_REPORTED_FAILURES = 10


class ClusteringError(Exception):
    """One or more frames could not be clustered.

    Raised AFTER the run's successful work has been committed, so a
    single unusable row costs only that row -- see assign_fields.
    """


def _pointing(row) -> tuple[float, float]:
    """
    The frame's header pointing as two floats, or ValueError naming the
    offending column.

    Validated up front rather than left to fail inside the matching
    loop, so an unusable frame can never become a field representative:
    if it did, every later frame compared against it would fail too,
    cascading one bad value into a whole-run failure by a second route.
    """
    for name in ("header_ra", "header_dec"):
        v = row[name]
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            raise ValueError(
                f"{name} is {v!r} ({type(v).__name__}), not a number -- "
                f"a value SQLite stored in a numeric column without "
                f"converting it (see inventory._coerce_numeric)")
    return float(row["header_ra"]), float(row["header_dec"])


def angular_separation(ra1, dec1, ra2, dec2) -> float:
    p1, p2 = math.radians(dec1), math.radians(dec2)
    dl = math.radians(ra2 - ra1)
    v = (math.sin(p1) * math.sin(p2) +
         math.cos(p1) * math.cos(p2) * math.cos(dl))
    return math.degrees(math.acos(max(-1.0, min(1.0, v))))


def frame_fov_deg(row) -> tuple[float, float]:
    focallen, xpixsz, n1, n2 = (row["focallen"], row["xpixsz"],
                                row["naxis1"], row["naxis2"])
    if not (focallen and xpixsz and n1 and n2):
        return DEFAULT_FOV_DEG, DEFAULT_FOV_DEG
    arcsec_px = 206.265 * xpixsz / focallen
    return (arcsec_px * n1 / 3600.0, arcsec_px * n2 / 3600.0)


def _seed_reps(conn) -> list[dict]:
    """
    Load one representative frame per field already in the database, so a
    re-run matches frames against existing fields instead of creating a
    duplicate field for every frame (the clustering loop below only ever
    searches this in-memory list, never the database).

    The representative for a field is its EARLIEST-INSERTED member, found
    via SQLite's implicit `rowid` (MIN(rowid) per field_id) rather than
    content_hash: content_hash is a BLAKE2b digest, so its sort order is
    unrelated to insertion order, and a frame added on a later incremental
    run has roughly even odds of sorting before the frame that originally
    created the field. Picking by content_hash let representative status
    drift to a newly-inserted frame across runs -- and because matching is
    greedy single-link (a field's members are only guaranteed to be within
    tolerance of whichever single frame was rep AT THE TIME they matched,
    not of every other member), a member that matched the ORIGINAL
    representative could fall outside tolerance of a DIFFERENT one picked
    on a later run and silently move to a new field. rowid always
    increases with insertion, so the original representative keeps the
    role for as long as the row exists -- this relies on rowid stability,
    which holds unless the store is VACUUMed (VACUUM may renumber rowids).

    Filters on fingerprint IS NOT NULL as well as field_id IS NOT NULL:
    field_id is only ever set (below, by this module) on rows that already
    passed a fingerprint-not-null check, so this is currently unreachable,
    but it documents that invariant so a future change to how field_id
    gets set can't silently violate it here.
    """
    rows = conn.execute("""
        SELECT f.field_id AS id, f.fingerprint AS fingerprint,
               f.header_ra AS ra, f.header_dec AS dec
        FROM frames f
        INNER JOIN (
            SELECT field_id, MIN(rowid) AS rep_rowid
            FROM frames
            WHERE field_id IS NOT NULL AND fingerprint IS NOT NULL
            GROUP BY field_id
        ) rep ON f.field_id = rep.field_id AND f.rowid = rep.rep_rowid
        ORDER BY f.field_id
    """).fetchall()
    return [{"id": r["id"], "fingerprint": r["fingerprint"],
             "ra": r["ra"], "dec": r["dec"]} for r in rows]


def assign_fields(conn, fp_threshold: int = FP_THRESHOLD_DEFAULT) -> int:
    """
    Cluster light frames into fields by fingerprint + pointing proximity
    and write the assignment to frames.field_id.

    Idempotent in two parts:
      - Structural guarantee: the candidate query below only ever selects
        frames with field_id IS NULL, so a frame is never re-evaluated,
        and never reassigned, once it has been assigned a field_id. This
        holds regardless of representative selection.
      - Convergence: `reps` is seeded from fields already in the database
        (see `_seed_reps`) before the clustering loop runs, so a NEW frame
        still matches the same field a single full run would have put it
        in, rather than spawning a duplicate.
    Returns the number of NEW fields created by this call (0 on a re-run
    that finds nothing new to cluster).

    Resilient to an unusable row: a frame whose pointing or fingerprint
    cannot be used is skipped, recorded, and reported by raising
    ClusteringError AFTER the rest of the run has been committed.
    Previously the single commit at the end meant one bad value among
    34,000 frames aborted the pass and discarded the ENTIRE run's work
    (verified: 3 frames, 1 bad -> TypeError, 0 fields and 0 assignments
    persisted). The failure is still loud -- it just no longer costs
    every other frame its clustering.
    """
    prev_factory = conn.row_factory
    conn.row_factory = sqlite3.Row
    try:
        reps = _seed_reps(conn)
        created = 0
        failures: list[tuple[str, str]] = []

        rows = conn.execute(
            "SELECT * FROM frames WHERE frame_type='light' "
            "AND fingerprint IS NOT NULL AND header_ra IS NOT NULL "
            "AND header_dec IS NOT NULL AND field_id IS NULL "
            "ORDER BY content_hash").fetchall()

        for row in rows:
            # Everything that can fail on this row happens before
            # anything is written for it, so a failure leaves neither an
            # orphan field nor a half-assigned frame.
            try:
                ra, dec = _pointing(row)
                fov_w, fov_h = frame_fov_deg(row)
                tol = max(fov_w, fov_h)
                match = None
                for rep in reps:
                    if hamming(row["fingerprint"],
                               rep["fingerprint"]) > fp_threshold:
                        continue
                    if angular_separation(ra, dec,
                                          rep["ra"], rep["dec"]) > tol:
                        continue
                    match = rep
                    break
            except Exception as exc:
                failures.append((row["content_hash"],
                                 f"{type(exc).__name__}: {exc}"))
                continue

            if match is None:
                cur = conn.execute(
                    "INSERT INTO fields (solve_source) VALUES ('none')")
                match = {"id": cur.lastrowid, "fingerprint": row["fingerprint"],
                         "ra": ra, "dec": dec}
                reps.append(match)
                created += 1
            conn.execute("UPDATE frames SET field_id=? WHERE content_hash=?",
                         (match["id"], row["content_hash"]))
        conn.commit()
        if failures:
            shown = failures[:MAX_REPORTED_FAILURES]
            more = len(failures) - len(shown)
            detail = "; ".join(f"{h}: {why}" for h, why in shown)
            if more:
                detail += f"; ... and {more} more"
            raise ClusteringError(
                f"{len(failures)} frame(s) could not be clustered and were "
                f"left unassigned. This call's other work was committed "
                f"first: {created} new field(s) created. Failures: {detail}")
        return created
    finally:
        conn.row_factory = prev_factory
