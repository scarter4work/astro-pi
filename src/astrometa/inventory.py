import itertools
import multiprocessing
import os
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from concurrent.futures.process import BrokenProcessPool
from dataclasses import dataclass, field as dc_field
from datetime import datetime, timezone
from pathlib import Path

from astropy.io import fits

from . import fitsheader, imagekeys
from .classify import classify
from .grouping import capture_instant
from .config import EXCLUDED_PATH_MARKERS


@dataclass
class InventoryResult:
    added: int = 0
    updated: int = 0
    failed: int = 0
    seen_hashes: set = dc_field(default_factory=set)


class InventoryWorkerError(RuntimeError):
    """
    A worker process failed while computing one frame's payload.

    Raised in the PARENT, naming the frame, and it aborts the run. This
    exists so a worker failure can never be mistaken for a frame that
    simply wasn't there: this project's entire purpose is knowing why a
    frame is absent, so a silently dropped frame is the worst outcome
    available. Anything that would have aborted the serial scan (a stat
    that raises, an unreadable directory, a content_hash that cannot be
    computed) still aborts, and a worker that DIES -- segfault in a C
    extension, OOM kill -- surfaces here too rather than leaving a hole
    in the inventory.

    Per-frame READ failures are not this: a bad header or unreadable
    pixels are caught inside the worker exactly as they were serially,
    recorded in the frame's read_error, and the scan continues.
    """


def _excluded(p: Path) -> bool:
    s = str(p)
    return any(m in s for m in EXCLUDED_PATH_MARKERS)


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


_FITS_SUFFIXES = (".fit", ".fits")


def _iter_fits(roots):
    for root in roots:
        if not root.exists():
            # A missing root (e.g. an unmounted NAS bind mount) must not be
            # mistaken for an empty archive: scan() returning cleanly with
            # zero results would make a caller's mark_missing() pass flip
            # every known frame to "missing", indistinguishable from the
            # operator having deleted the archive. Fail loudly instead.
            raise FileNotFoundError(f"inventory root does not exist: {root}")
        for p in root.rglob("*"):
            if (p.is_file() and p.suffix.lower() in _FITS_SUFFIXES
                    and "_thn" not in p.name and not _excluded(p)):
                yield p


def _read_header(path: Path) -> tuple[dict, str | None]:
    """Independent header read. On failure, returns ({}, "header: ...")."""
    try:
        return fitsheader.read_header(path), None
    except Exception as exc:                      # loud, never silent
        return {}, f"header: {type(exc).__name__}: {exc}"


def _read_pixels(path: Path) -> tuple[str | None, float | None, float | None, str | None]:
    """
    Independent pixel read: one `fits.getdata` feeds both the fingerprint
    and the pixel stats, so the archive is never read three times for a
    single frame. On failure, all three derived values stay None -- the
    fingerprint is never partially committed if pixel_stats subsequently
    raises (e.g. on NaN pixels), so a failure here can't leave a
    fingerprint on record without the stats that would normally sit
    beside it.
    """
    try:
        data = fits.getdata(path, memmap=False)
        fp = imagekeys.fingerprint(data)
        bg, sat = imagekeys.pixel_stats(data)
        return fp, bg, sat, None
    except Exception as exc:                       # loud, never silent
        return None, None, None, f"pixels: {type(exc).__name__}: {exc}"


# Header cards that must land in a numeric column, and what they must
# become. fitsheader._parse_value returns whatever a card happens to
# parse to, and SQLite's REAL/INTEGER affinity does NOT reject a value
# it cannot convert -- it stores the string as TEXT. That text survives
# the whole inventory pass silently and only surfaces much later, as a
# TypeError inside cluster.assign_fields, which is exactly the place it
# does the most damage. Coerce here, at the point of reading, and record
# a failure loudly when a value will not coerce.
#
# Measured 2026-09-09 across a 200-frame archive-wide sample: every `RA`
# card is already a bare decimal, so today's trigger is not present --
# the sexagesimal form appears only under `OBJCTRA`, which nothing here
# reads. This archive spans seven cameras and three naming eras, so the
# guard is kept regardless; sexagesimal is deliberately NOT parsed into
# a decimal, because inventing a value for a card this code does not
# claim to understand is precisely the silent fallback the project
# rules forbid.
_NUMERIC_HEADER_FIELDS: dict[str, type] = {
    "EXPTIME": float, "RA": float, "DEC": float,
    "FOCALLEN": float, "XPIXSZ": float,
    "NAXIS1": int, "NAXIS2": int,
}


def _coerce_numeric(header: dict) -> tuple[dict, str | None]:
    """
    Coerce the numeric header cards, returning (values, error).

    A card that is absent is None and is not an error. A card that will
    not coerce is recorded in the error string and stored as None --
    never as text in a numeric column. Booleans are rejected outright:
    Python would happily coerce a FITS `T` card to 1.0, inventing a
    pointing out of a flag.
    """
    values: dict[str, float | int | None] = {}
    bad: list[str] = []
    for card, caster in _NUMERIC_HEADER_FIELDS.items():
        v = header.get(card)
        if v is None:
            values[card] = None
        elif isinstance(v, bool):
            values[card] = None
            bad.append(f"{card}={v!r} (boolean, not a number)")
        else:
            try:
                values[card] = caster(v)
            except (TypeError, ValueError) as exc:
                values[card] = None
                bad.append(f"{card}={v!r} ({type(exc).__name__})")
    if not bad:
        return values, None
    return values, "numeric: uncoercible header value(s): " + "; ".join(bad)


def _captured_at(header: dict, filename: str) -> str | None:
    """
    The frame's capture instant: DATE-OBS, falling back to the
    `_YYYYMMDD-HHMMSS_` token in the filename (spec 7).

    The fallback is not cosmetic. Without it a frame whose header
    carries no DATE-OBS has no capture instant on record anywhere, so it
    can never be placed in a session or project. Nothing is invented:
    when neither source has one, this is None.

    The filename parser is grouping.capture_instant -- the same one
    grouping itself buckets on -- rather than a second copy of the
    regex, so the two can never disagree about what a capture instant is.
    """
    v = header.get("DATE-OBS")
    if isinstance(v, str) and v.strip():
        return v.strip()
    if v is not None and not isinstance(v, str):
        return str(v)
    inst = capture_instant(filename)
    return inst.isoformat() if inst else None


def _object_card(header: dict) -> str | None:
    """
    The OBJECT card as a string, or None when absent or blank.

    Coerced rather than passed through: fitsheader._parse_value returns
    whatever a card parses to, and an unquoted numeric OBJECT (a target
    written as a bare catalogue number) would otherwise land in a TEXT
    column as an int. This is the operator's own label, never an
    identity claim -- it is stored verbatim, not canonicalised.
    """
    v = header.get("OBJECT")
    if v is None:
        return None
    s = v.strip() if isinstance(v, str) else str(v)
    return s or None


# How much work one worker process has to be given before starting it
# pays for itself. Standing up a spawn-based pool costs roughly a
# quarter-second; hashing and decoding a single frame costs
# milliseconds, so a pool is a straight loss on a handful of frames and
# would turn the test suite (dozens of one- and two-frame scans) into a
# process-startup benchmark. Worker count is damped by this so a tiny
# scan runs serially in-process, and the full archive (34,000+ frames)
# always reaches the configured maximum.
_MIN_FRAMES_PER_WORKER = 8


def default_workers() -> int:
    """
    Default parallelism: every available CPU but one.

    The per-frame work is 94% CPU-bound (measured over a 3h52m partial
    pass on the deployment host: 3h37m of CPU against 3h52m elapsed), so
    the core count is the right scale. One core is left for the parent,
    which is not idle -- it unpickles every result and performs every
    upsert, and starving it would just move the bottleneck.

    os.process_cpu_count() rather than os.cpu_count(): it honours CPU
    affinity and cgroup limits, so running under a restricted cpuset
    does not silently oversubscribe.
    """
    return max(1, (os.process_cpu_count() or 1) - 1)


def _frame_payload(path_str: str, read_pixels: bool) -> dict:
    """
    All of one frame's per-file work, in whichever process is running it.

    This is the entire expensive half of a scan -- content_hash (BLAKE2b
    over the whole file), the header read, the pixel decode, the
    fingerprint and the pixel stats -- and it is deliberately the SAME
    function on the serial and parallel paths, so the two cannot drift
    apart. Nothing here touches the database: SQLite stays single-writer
    in the parent.

    It returns flat, picklable scalars and never a decoded array. That
    is a memory bound, not a style choice: frames run to 33 MB and there
    are 34,400+ of them, so returning `data` would put one array per
    in-flight result on the wire and in the parent's heap. Only
    `workers` arrays exist at any moment, each freed when its worker
    moves to the next frame.

    The header-derived fields (classify, _captured_at, _object_card,
    _coerce_numeric) are computed here rather than in the parent for the
    same reason: it keeps a several-hundred-card header dict off the
    pickle wire, and it keeps the serial and parallel results identical
    by construction.

    Only the two read failures are caught, exactly as they were when
    this ran serially -- see _read_header/_read_pixels. path.stat() and
    content_hash are deliberately NOT wrapped: they aborted the scan
    before this change and must still abort it, because a stat that
    raises is a statement about the archive (an unmounted root, a
    permission change), not about one frame, and turning it into 34,400
    recorded read_errors would be exactly the silent fallback this
    project forbids.
    """
    path = Path(path_str)
    stat = path.stat()
    chash = imagekeys.content_hash(path)

    # Header and pixel reads are independent: one failing must not
    # discard data the other successfully recovered.
    header, header_err = _read_header(path)
    nums, numeric_err = _coerce_numeric(header)

    fp, bg, sat, pixel_err = None, None, None, None
    if read_pixels:
        fp, bg, sat, pixel_err = _read_pixels(path)

    errors = [e for e in (header_err, numeric_err, pixel_err) if e]
    return {
        "chash": chash, "path": path_str, "filename": path.name,
        "size": stat.st_size, "mtime": stat.st_mtime,
        "frame_type": classify(path.name, header),
        "camera": header.get("INSTRUME"), "filter": header.get("FILTER"),
        "exptime": nums["EXPTIME"],
        "captured_at": _captured_at(header, path.name),
        "object_card": _object_card(header), "leaf_dir": path.parent.name,
        "header_ra": nums["RA"], "header_dec": nums["DEC"],
        "focallen": nums["FOCALLEN"], "xpixsz": nums["XPIXSZ"],
        "naxis1": nums["NAXIS1"], "naxis2": nums["NAXIS2"],
        "fingerprint": fp, "bg_median": bg, "saturated_frac": sat,
        "read_error": "; ".join(errors) if errors else None,
        "header_ok": 1 if header_err is None else 0,
        "pixel_ok": 1 if (read_pixels and pixel_err is None) else 0,
    }


def _result_or_raise(path: Path, future) -> dict:
    """
    Unwrap one worker result, or convert its failure into a loud,
    frame-attributed abort.

    A future that raises is never swallowed. concurrent.futures would
    otherwise let an unretrieved exception disappear entirely, which
    here would mean a frame quietly missing from the inventory with
    nothing on record to say why.
    """
    try:
        return future.result()
    except BrokenProcessPool as exc:
        raise InventoryWorkerError(
            f"inventory worker process died while processing {path}: "
            f"{type(exc).__name__}: {exc}\n"
            f"This is a hard abort rather than a skipped frame: a dead "
            f"worker (segfault in a C extension, OOM kill) gives no way "
            f"to tell which frames were read and which were dropped, and "
            f"a silently dropped frame is indistinguishable from a "
            f"deleted one.\n"
            f"If this was the FIRST frame of the run the pool never came "
            f"up, and the cause is almost always the caller's __main__: "
            f"the spawn start method re-imports it in every worker, so "
            f"module-level code must sit behind "
            f"`if __name__ == \"__main__\":` and a `python -` heredoc "
            f"(whose __main__ has no importable file) cannot drive a "
            f"parallel scan at all. Pass workers=1 to run in-process."
        ) from exc
    except Exception as exc:
        raise InventoryWorkerError(
            f"inventory worker failed on {path}: "
            f"{type(exc).__name__}: {exc}"
        ) from exc


def _scan_parallel(conn, res, frames, read_pixels: bool, workers: int) -> None:
    """
    Farm the per-frame work out to `workers` processes, consuming the
    results IN SUBMISSION ORDER and upserting them from this process.

    Two properties this shape buys, both required:

    Determinism. Results are consumed in walk order, not completion
    order, so the database ends up identical no matter how many workers
    ran. That is not cosmetic: two files can share a content_hash (the
    archive has genuine duplicates), and they collide on one row whose
    `path` is written by whichever is processed LAST. Completion order
    would make that column a race.

    Bounded memory. Work is submitted through a sliding window rather
    than handed to ProcessPoolExecutor.map, which would submit all
    34,400 items at once and hold every completed-but-unconsumed result.
    The walk stays lazy -- the tree is never materialised -- and the
    window holds only path strings and small result dicts, never a
    decoded array. Decoded arrays exist only inside the workers, at most
    one per worker.

    The window is deliberately wider than the pool. Results are consumed
    strictly in order, so a single slow frame stalls the head of the
    queue; a window of one task per worker would leave every worker idle
    behind it. Four deep costs a few hundred bytes per slot and keeps
    them fed.

    The start method is pinned to spawn rather than left to the default,
    for two reasons and NOT for a third that was checked and found not
    to hold.

    It has to be pinned at all because the default is not stable across
    the versions this runs on: 3.13 defaults to fork and 3.14 defaults
    to forkserver, so an unpinned pool would behave differently on the
    dev box and the deployment host. And it is pinned to spawn because
    fork is the one method that is genuinely unsafe here -- forking a
    process that may have numpy/astropy threads running can deadlock the
    child, which is exactly why 3.14 moved the default off it -- and
    because a spawned worker gets a clean interpreter rather than a copy
    of a parent holding an open SQLite connection, which SQLite
    documents as unsupported to carry across a fork.

    What is NOT a reason, though it reads plausibly: forked children
    inheriting the database file descriptor and dropping the parent's
    locks when they exit. Measured 2026-09-09 -- fcntl locks are not
    inherited across fork, so a child holds none to release, and the
    parent's write lock survives a child's exit intact.

    The cost of spawn is that a worker re-imports the caller's
    __main__. A caller must therefore guard its module-level code with
    `if __name__ == "__main__"` (astrometa.cli does), and cannot drive a
    parallel scan from a `python -` heredoc, where __main__ has no
    importable file at all. Both surface as a BrokenProcessPool on the
    first frame; see _result_or_raise, which names them.
    """
    ctx = multiprocessing.get_context("spawn")
    window = 4 * workers
    pending: deque[tuple[Path, object]] = deque()
    with ProcessPoolExecutor(max_workers=workers, mp_context=ctx) as pool:
        try:
            for path in itertools.islice(frames, window):
                pending.append(
                    (path, pool.submit(_frame_payload, str(path), read_pixels)))
            while pending:
                path, future = pending.popleft()
                _record(conn, res, _result_or_raise(path, future))
                nxt = next(frames, None)
                if nxt is not None:
                    pending.append(
                        (nxt, pool.submit(_frame_payload, str(nxt),
                                          read_pixels)))
        except BaseException:
            # Don't let the executor's shutdown(wait=True) sit through a
            # queue of work whose results nobody will read.
            for _, future in pending:
                future.cancel()
            raise


def _record(conn, res, p: dict) -> None:
    """Everything a scan writes, from the parent process only."""
    res.seen_hashes.add(p["chash"])
    existing = conn.execute(
        "SELECT 1 FROM frames WHERE content_hash=?", (p["chash"],)).fetchone()

    # `failed` is an OVERLAPPING counter, not a fourth disjoint bucket:
    # a frame whose read failed is still inventoried, so it increments
    # `failed` AND one of added/updated. The alternative reads as
    # "34,400 scanned, 12 failed, so 34,388 are on record", which would
    # be wrong in the one direction that matters here.
    if p["read_error"] is not None:
        res.failed += 1

    # The upsert refreshes EVERY column a scan derives, not just
    # path/last_seen/disposition/read_error. A row can reach this
    # table by a route other than a successful scan -- manifest
    # restore (which carries no header_ra/focallen/naxis/bg_median),
    # or a scan whose pixel read failed -- and if the update clause
    # skipped those columns, no number of rescans could ever fill
    # them. That silently defeated two things measured in review:
    # a manifest-restored store could never be clustered (
    # cluster.assign_fields requires header_ra/header_dec/
    # fingerprint), and a frame that recovered from a transient
    # pixel-read failure had read_error cleared to NULL while
    # fingerprint/bg_median/saturated_frac stayed NULL -- dropping
    # out of clustering with nothing left on record to say why.
    #
    # Which columns update is gated on WHICH READ SUCCEEDED, not
    # written blindly from `excluded`: the header read and the pixel
    # read are independent (see _read_header/_read_pixels), so a
    # later transient failure must not overwrite good values with
    # NULL. Nulling a fingerprint would drop that frame from
    # cluster._seed_reps -- which only considers non-null
    # fingerprints as field representatives -- and spawn duplicate
    # fields on the next run.
    #
    # first_seen is deliberately absent from the update clause: it
    # records when this content_hash was FIRST inventoried and must
    # survive every rescan.
    #
    # Ownership split enforced here: scan() owns the present/missing
    # transition (a rescan may legitimately move a frame back to
    # 'present', e.g. one disposition.mark_missing() previously
    # flagged), but it must never overwrite 'quarantined' -- that
    # disposition is the operator's/quality gate's, recorded via
    # disposition.mark_culled()/quarantine(). Without the CASE guard,
    # quarantine() moving a file into its sibling rejected/ dir (still
    # under the scanned root) would get walked right back into by the
    # next scan and silently flipped back to 'present', erasing the
    # cull -- unqualified `disposition` on the right of the CASE reads
    # the pre-existing row's value (SQLite UPSERT semantics), not the
    # value this INSERT would have written. path/last_seen/read_error
    # still update unconditionally: relocating a quarantined frame's
    # recorded path to where it now actually lives is useful, not
    # a disposition change.
    conn.execute("""
        INSERT INTO frames (content_hash, path, filename, size, mtime,
            first_seen, last_seen, frame_type, camera, filter, exptime,
            captured_at, object_card, leaf_dir,
            header_ra, header_dec, focallen, xpixsz,
            naxis1, naxis2, fingerprint, bg_median, saturated_frac,
            disposition, read_error)
        VALUES (:chash, :path, :filename, :size, :mtime,
            :now, :now, :frame_type, :camera, :filter, :exptime,
            :captured_at, :object_card, :leaf_dir,
            :header_ra, :header_dec, :focallen, :xpixsz,
            :naxis1, :naxis2, :fingerprint, :bg_median, :saturated_frac,
            'present', :read_error)
        ON CONFLICT(content_hash) DO UPDATE SET
            path=excluded.path,
            filename=excluded.filename,
            size=excluded.size,
            mtime=excluded.mtime,
            last_seen=excluded.last_seen,
            -- frame_type is header-derived and gated like its
            -- siblings, NOT unconditional. classify() falls back to
            -- the filename when the header is empty, so a frame with
            -- IMAGETYP='Light Frame' but an Autosave* name scans as
            -- 'light' and an unconditional update would REWRITE it to
            -- 'derived' on any rescan whose header read failed --
            -- dropping it out of cluster.assign_fields and
            -- build_projects, both of which filter frame_type='light'.
            frame_type=CASE WHEN :header_ok THEN excluded.frame_type
                            ELSE frame_type END,
            camera=CASE WHEN :header_ok THEN excluded.camera
                        ELSE camera END,
            filter=CASE WHEN :header_ok THEN excluded.filter
                        ELSE filter END,
            exptime=CASE WHEN :header_ok THEN excluded.exptime
                         ELSE exptime END,
            -- captured_at is the one header-derived column with a
            -- source that needs no header: _captured_at falls back to
            -- the filename capture instant (spec 7), and with a failed
            -- header read `excluded.captured_at` IS that fallback. A
            -- plain :header_ok gate therefore made the fallback apply
            -- on first insert but never on a rescan whose header
            -- failed -- the one case it was added for, e.g. a
            -- manifest-restored row that carried no capture instant.
            -- COALESCE order matters: when the header IS readable it
            -- stays authoritative, and when it is not, an already
            -- recorded DATE-OBS (UTC, sub-second) is never downgraded
            -- to the filename's local wall-clock token.
            captured_at=CASE WHEN :header_ok THEN excluded.captured_at
                             ELSE COALESCE(captured_at,
                                           excluded.captured_at) END,
            object_card=CASE WHEN :header_ok THEN excluded.object_card
                             ELSE object_card END,
            leaf_dir=excluded.leaf_dir,
            header_ra=CASE WHEN :header_ok THEN excluded.header_ra
                           ELSE header_ra END,
            header_dec=CASE WHEN :header_ok THEN excluded.header_dec
                            ELSE header_dec END,
            focallen=CASE WHEN :header_ok THEN excluded.focallen
                          ELSE focallen END,
            xpixsz=CASE WHEN :header_ok THEN excluded.xpixsz
                        ELSE xpixsz END,
            naxis1=CASE WHEN :header_ok THEN excluded.naxis1
                        ELSE naxis1 END,
            naxis2=CASE WHEN :header_ok THEN excluded.naxis2
                        ELSE naxis2 END,
            fingerprint=CASE WHEN :pixel_ok THEN excluded.fingerprint
                             ELSE fingerprint END,
            bg_median=CASE WHEN :pixel_ok THEN excluded.bg_median
                           ELSE bg_median END,
            saturated_frac=CASE WHEN :pixel_ok
                                THEN excluded.saturated_frac
                                ELSE saturated_frac END,
            disposition=CASE WHEN disposition='quarantined'
                              THEN disposition ELSE 'present' END,
            read_error=excluded.read_error
    """, {**p, "now": _now()})
    if existing:
        res.updated += 1
    else:
        res.added += 1


def scan(conn, roots, read_pixels: bool = True,
         workers: int | None = None) -> InventoryResult:
    """
    Inventory every FITS frame under `roots`.

    The expensive per-frame work runs across a process pool; every
    database write happens here, in the parent, because SQLite is
    single-writer. `workers` caps the pool and defaults to
    default_workers(); 1 runs everything in-process.

    The walk stays lazy. Only enough of it is drawn up front to size the
    pool -- there is no point starting 47 workers for 30 frames -- and
    the rest is pulled through as results are consumed, so a missing
    root still raises the moment the walk reaches it, exactly as it did
    when this was a plain loop.
    """
    max_workers = default_workers() if workers is None else int(workers)
    if max_workers < 1:
        raise ValueError(
            f"workers must be at least 1, got {workers!r}")

    res = InventoryResult()
    frames = _iter_fits(roots)
    head = list(itertools.islice(frames, _MIN_FRAMES_PER_WORKER * max_workers))
    frames = itertools.chain(head, frames)
    n_workers = min(max_workers,
                    max(1, len(head) // _MIN_FRAMES_PER_WORKER))

    if n_workers == 1:
        for path in frames:
            _record(conn, res, _frame_payload(str(path), read_pixels))
    else:
        _scan_parallel(conn, res, frames, read_pixels, n_workers)

    conn.commit()
    return res
