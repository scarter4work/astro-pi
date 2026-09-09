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
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'present',?)
            ON CONFLICT(content_hash) DO UPDATE SET
                path=excluded.path, last_seen=excluded.last_seen,
                disposition=CASE WHEN disposition='quarantined'
                                  THEN disposition ELSE 'present' END,
                read_error=excluded.read_error
        """, (chash, str(path), path.name, stat.st_size, stat.st_mtime,
              _now(), _now(), classify(path.name, header),
              header.get("INSTRUME"), header.get("FILTER"),
              header.get("EXPTIME"), header.get("DATE-OBS"),
              header.get("RA"), header.get("DEC"), header.get("FOCALLEN"),
              header.get("XPIXSZ"), header.get("NAXIS1"), header.get("NAXIS2"),
              fp, bg, sat, err))
        if existing:
            res.updated += 1
        else:
            res.added += 1
    conn.commit()
    return res
