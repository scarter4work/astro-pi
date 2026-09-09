r"""
Per-leaf-directory manifest export and import: the project's durability
concession against database loss.

The SQLite database in db.py is the working store for every other module
in this system, but it is one file that can be lost or corrupted, and it
does not travel with the archive when a directory gets moved or renamed.
A `.astro-manifest.json` sidecar written into (or mirroring) each LEAF
directory -- the ~845 terminal directories that actually hold frame
files, not the ~28,135 individual frames -- means every frame's
classification and disposition labels survive independently of the
database and travel with the folder that holds them.

export_dir/export_all only ever READ frames out of the database and
write a JSON sidecar; import_file only ever writes rows back into a
fresh (or recovering) database. Neither one ever touches a FITS file --
the manifest sidecar is the one write this system makes outside of
disposition.py's quarantine (which moves FITS files themselves; see its
module docstring), never the frame data.

LIKE escaping (export_dir): SQLite's LIKE operator treats `_` as a
single-character wildcard, not a literal underscore. This archive's own
root path (`/mnt/qnap/astro_data/...`) contains one, and so do many
target directory names (`Sh2-106`, `IC 1848_1-1`), so an unescaped
`f"{leaf_dir}/%"` prefix is broader than intended and can silently match
a sibling path that differs from the leaf only where an underscore
sits. `_like_escape` neutralises `\`, `%` and `_` in the interpolated
prefix and the query pairs it with `ESCAPE '\'` so that escaped
underscore is matched literally rather than as a wildcard -- see
test_like_escape_rejects_underscore_wildcard_sibling for the concrete
collision this closes.

Read-only archive vs. the manifest write (export_all's out_dir): the
deployment mounts (`/archive`, `/live`) are read-only, so export_dir's
default of writing into the leaf directory it just read from would fail
on every call there. export_dir already accepted an `out_dir` escape
hatch; export_all did not propagate one, making it unusable on the
deployment host entirely. It now accepts `out_dir` and, when given,
writes each leaf's manifest beneath a MIRROR of that leaf's full
directory structure under `out_dir` (see `_mirror_leaf_dir`), rather
than flattening every manifest into one directory -- flattening would
collide on basename alone (target and panel directory names repeat
across different nights/objects in this archive) and silently keep only
the last manifest written for each colliding name. A write failure (a
still-read-only target, a permission error) is never swallowed: both
export_dir and export_all let the OSError propagate, naming the leaf
directory and the destination path, rather than skipping a directory
silently -- a partial export that looks complete is exactly the failure
this whole project exists to prevent.
"""
import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path

MANIFEST_NAME = ".astro-manifest.json"
_FIELDS = ("content_hash", "filename", "size", "frame_type", "camera",
           "filter", "exptime", "captured_at", "fingerprint",
           "disposition", "disposition_reason", "disposition_source")


def _like_escape(s: str) -> str:
    """
    Escape LIKE metacharacters (`\\`, `%`, `_`) in `s` so it is matched
    literally rather than as a pattern. SQLite's LIKE treats `_` as a
    single-character wildcard -- this archive's paths are full of
    literal underscores -- so an unescaped prefix is broader than the
    caller intends. Pair with `ESCAPE '\\'` in the query.
    """
    return s.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")


def _mirror_leaf_dir(leaf_dir: Path, out_dir: Path) -> Path:
    """
    Map `leaf_dir` onto a path beneath `out_dir` that preserves its full
    directory structure (minus the leading root), so two different leaf
    directories that happen to share a basename -- a target or panel
    name reused under a different night or object -- land at distinct
    destinations instead of colliding on the same manifest filename.

    Stripping the anchor handles a well-formed absolute leaf_dir, but
    does nothing about a literal '..' segment: a relative leaf_dir that
    contains one, or an absolute one carrying more '..' than the
    anchor-strip accounts for, is not collapsed by joinpath alone, and
    could otherwise walk the target back out of out_dir entirely --
    `export_dir` then mkdirs whatever path this function returns, so
    that is not cosmetic, it can create real directories outside
    out_dir (verified: an unguarded version of this function resolved a
    crafted path all the way to filesystem root). Both sides are
    resolved (symlinks and '..' collapsed) and checked with
    `is_relative_to` -- not string-prefix matching, which a sibling
    directory like "out_dir_evil" would defeat -- before anything is
    returned. Raises ValueError, naming both the original leaf_dir and
    where it actually resolved to, rather than silently handing back an
    unsafe path.
    """
    parts = leaf_dir.parts
    if leaf_dir.anchor:
        parts = parts[len(Path(leaf_dir.anchor).parts):]
    target = out_dir.joinpath(*parts) if parts else out_dir

    resolved_out_dir = out_dir.resolve()
    resolved_target = target.resolve()
    if (resolved_target != resolved_out_dir
            and not resolved_target.is_relative_to(resolved_out_dir)):
        raise ValueError(
            f"refusing to mirror leaf directory {leaf_dir} to {target} -- "
            f"it resolves to {resolved_target}, which is outside out_dir "
            f"({resolved_out_dir}). This would create directories outside "
            f"out_dir.")
    return resolved_target


def export_dir(conn, leaf_dir: Path, out_dir: Path | None = None) -> Path:
    """
    Write a `.astro-manifest.json` sidecar for every frame recorded
    under `leaf_dir`, into `out_dir` if given, else into `leaf_dir`
    itself.

    Frames are selected by path prefix with an escaped LIKE (see module
    docstring) so a sibling directory differing from `leaf_dir` only
    where an underscore sits is never pulled in.

    Raises OSError, naming both `leaf_dir` and the destination path, if
    the manifest can't actually be written (e.g. a read-only target) --
    never silently skips.
    """
    leaf_dir = Path(leaf_dir)
    prev_factory = conn.row_factory
    conn.row_factory = sqlite3.Row
    try:
        pattern = _like_escape(str(leaf_dir)) + "/%"
        rows = conn.execute(
            f"SELECT {', '.join(_FIELDS)} FROM frames "
            f"WHERE path LIKE ? ESCAPE '\\'",
            (pattern,)).fetchall()
    finally:
        conn.row_factory = prev_factory

    doc = {
        "version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "leaf_dir": str(leaf_dir),
        "frames": [dict(r) for r in rows],
    }
    target = (Path(out_dir) if out_dir is not None else leaf_dir) / MANIFEST_NAME
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(doc, indent=2))
    except OSError as e:
        raise OSError(
            f"failed to write manifest for leaf directory {leaf_dir} to "
            f"{target}: {e}") from e
    return target


def export_all(conn, out_dir: Path | None = None, dry_run: bool = False) -> int:
    """
    Export a manifest for every leaf directory referenced by `frames`.

    `dry_run=True` returns the count of leaf directories that would be
    exported without writing anything -- neither export_dir nor any
    filesystem call runs.

    When `out_dir` is given, each leaf's manifest is written beneath a
    mirror of its full directory structure under `out_dir` (see
    `_mirror_leaf_dir`), never flattened into `out_dir` directly -- see
    module docstring for why flattening would collide. A write failure
    raises immediately, naming the offending leaf directory (via
    export_dir); no directory is silently skipped, so a partial run
    never looks like a complete one.

    Not atomic: this aborts on the FIRST failure rather than attempting
    every leaf and reporting failures at the end, so on a partial run
    the manifests already written for leaves processed earlier in the
    same call remain on disk exactly as written -- each one is
    independently complete and valid for its own leaf. That's the
    intended, safe behaviour (a leaf's manifest is never half-written),
    not a bug; an operator reading a raised export_all should read it as
    "some real manifests exist, some leaves are not yet done", not as
    "nothing happened".
    """
    leaves = {Path(r[0]).parent for r in
              conn.execute("SELECT path FROM frames").fetchall()}
    if dry_run:
        return len(leaves)
    out_dir = Path(out_dir) if out_dir is not None else None
    for leaf in leaves:
        target_dir = _mirror_leaf_dir(leaf, out_dir) if out_dir is not None else None
        export_dir(conn, leaf, out_dir=target_dir)
    return len(leaves)


def import_file(conn, manifest_path: Path) -> int:
    """
    Reconstruct `frames` rows from one manifest sidecar.

    Identity is `content_hash`, never filename -- filenames are not
    unique across this archive -- so this is an upsert keyed on
    content_hash: re-running it against the same manifest updates the
    same rows rather than duplicating them.
    """
    doc = json.loads(Path(manifest_path).read_text())
    leaf = Path(doc["leaf_dir"])
    n = 0
    for fr in doc["frames"]:
        conn.execute("""INSERT INTO frames (content_hash, path, filename,
            size, mtime, frame_type, camera, filter, exptime, captured_at,
            fingerprint, disposition, disposition_reason, disposition_source)
            VALUES (?,?,?,?,0.0,?,?,?,?,?,?,?,?,?)
            ON CONFLICT(content_hash) DO UPDATE SET
              filter=excluded.filter, camera=excluded.camera,
              disposition=excluded.disposition,
              disposition_reason=excluded.disposition_reason""",
            (fr["content_hash"], str(leaf / fr["filename"]), fr["filename"],
             fr["size"], fr["frame_type"], fr["camera"], fr["filter"],
             fr["exptime"], fr["captured_at"], fr["fingerprint"],
             fr["disposition"], fr["disposition_reason"],
             fr["disposition_source"]))
        n += 1
    conn.commit()
    return n
