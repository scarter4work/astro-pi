import math
import sqlite3

from .imagekeys import hamming

DEFAULT_FOV_DEG = 1.0


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

    The representative for a field is the frame carrying that field_id
    with the smallest content_hash. That is always the frame that
    originally created the field: assign_fields processes candidate frames
    in ascending content_hash order, and a field is only ever created when
    the frame being processed fails to match any rep already in `reps` --
    so the first (smallest-hash) frame of a field is necessarily the one
    that created it, and every later member matched against it (or against
    a rep chained from it) rather than creating a field of its own.
    """
    rows = conn.execute("""
        SELECT f.field_id AS id, f.fingerprint AS fingerprint,
               f.header_ra AS ra, f.header_dec AS dec
        FROM frames f
        INNER JOIN (
            SELECT field_id, MIN(content_hash) AS rep_hash
            FROM frames
            WHERE field_id IS NOT NULL
            GROUP BY field_id
        ) rep ON f.field_id = rep.field_id AND f.content_hash = rep.rep_hash
        ORDER BY f.field_id
    """).fetchall()
    return [{"id": r["id"], "fingerprint": r["fingerprint"],
             "ra": r["ra"], "dec": r["dec"]} for r in rows]


def assign_fields(conn, fp_threshold: int = 12) -> int:
    """
    Cluster light frames into fields by fingerprint + pointing proximity
    and write the assignment to frames.field_id.

    Idempotent: `reps` is seeded from fields already in the database (see
    `_seed_reps`) before the clustering loop runs, so a frame that already
    belongs to a field matches its existing field again rather than
    spawning a duplicate. Returns the number of NEW fields created by this
    call (0 on a re-run that finds nothing new to cluster).
    """
    prev_factory = conn.row_factory
    conn.row_factory = sqlite3.Row
    try:
        reps = _seed_reps(conn)
        created = 0

        rows = conn.execute(
            "SELECT * FROM frames WHERE frame_type='light' "
            "AND fingerprint IS NOT NULL AND header_ra IS NOT NULL "
            "AND header_dec IS NOT NULL ORDER BY content_hash").fetchall()

        for row in rows:
            fov_w, fov_h = frame_fov_deg(row)
            tol = max(fov_w, fov_h)
            match = None
            for rep in reps:
                if hamming(row["fingerprint"], rep["fingerprint"]) > fp_threshold:
                    continue
                if angular_separation(row["header_ra"], row["header_dec"],
                                      rep["ra"], rep["dec"]) > tol:
                    continue
                match = rep
                break
            if match is None:
                cur = conn.execute(
                    "INSERT INTO fields (solve_source) VALUES ('none')")
                match = {"id": cur.lastrowid, "fingerprint": row["fingerprint"],
                         "ra": row["header_ra"], "dec": row["header_dec"]}
                reps.append(match)
                created += 1
            conn.execute("UPDATE frames SET field_id=? WHERE content_hash=?",
                         (match["id"], row["content_hash"]))
        conn.commit()
        return created
    finally:
        conn.row_factory = prev_factory
