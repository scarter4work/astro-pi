from dataclasses import dataclass, field as dc_field
from datetime import datetime, timezone
from pathlib import Path

from astropy.io import fits

from . import fitsheader, imagekeys
from .classify import classify
from .config import EXCLUDED_PATH_MARKERS


@dataclass
class InventoryResult:
    added: int = 0
    updated: int = 0
    failed: int = 0
    seen_hashes: set = dc_field(default_factory=set)


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


def scan(conn, roots, read_pixels: bool = True) -> InventoryResult:
    res = InventoryResult()
    for path in _iter_fits(roots):
        stat = path.stat()
        chash = imagekeys.content_hash(path)
        res.seen_hashes.add(chash)
        existing = conn.execute(
            "SELECT 1 FROM frames WHERE content_hash=?", (chash,)).fetchone()

        # Header and pixel reads are independent: one failing must not
        # discard data the other successfully recovered.
        header, header_err = _read_header(path)

        fp, bg, sat, pixel_err = None, None, None, None
        if read_pixels:
            fp, bg, sat, pixel_err = _read_pixels(path)

        errors = [e for e in (header_err, pixel_err) if e]
        err = "; ".join(errors) if errors else None
        if errors:
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
                captured_at, header_ra, header_dec, focallen, xpixsz,
                naxis1, naxis2, fingerprint, bg_median, saturated_frac,
                disposition, read_error)
            VALUES (:chash, :path, :filename, :size, :mtime,
                :now, :now, :frame_type, :camera, :filter, :exptime,
                :captured_at, :header_ra, :header_dec, :focallen, :xpixsz,
                :naxis1, :naxis2, :fingerprint, :bg_median, :saturated_frac,
                'present', :read_error)
            ON CONFLICT(content_hash) DO UPDATE SET
                path=excluded.path,
                filename=excluded.filename,
                size=excluded.size,
                mtime=excluded.mtime,
                last_seen=excluded.last_seen,
                frame_type=excluded.frame_type,
                camera=CASE WHEN :header_ok THEN excluded.camera
                            ELSE camera END,
                filter=CASE WHEN :header_ok THEN excluded.filter
                            ELSE filter END,
                exptime=CASE WHEN :header_ok THEN excluded.exptime
                             ELSE exptime END,
                captured_at=CASE WHEN :header_ok THEN excluded.captured_at
                                 ELSE captured_at END,
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
        """, {
            "chash": chash, "path": str(path), "filename": path.name,
            "size": stat.st_size, "mtime": stat.st_mtime, "now": _now(),
            "frame_type": classify(path.name, header),
            "camera": header.get("INSTRUME"), "filter": header.get("FILTER"),
            "exptime": header.get("EXPTIME"),
            "captured_at": header.get("DATE-OBS"),
            "header_ra": header.get("RA"), "header_dec": header.get("DEC"),
            "focallen": header.get("FOCALLEN"),
            "xpixsz": header.get("XPIXSZ"),
            "naxis1": header.get("NAXIS1"), "naxis2": header.get("NAXIS2"),
            "fingerprint": fp, "bg_median": bg, "saturated_frac": sat,
            "read_error": err,
            "header_ok": 1 if header_err is None else 0,
            "pixel_ok": 1 if (read_pixels and pixel_err is None) else 0,
        })
        if existing:
            res.updated += 1
        else:
            res.added += 1
    conn.commit()
    return res
