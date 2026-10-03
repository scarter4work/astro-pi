from pathlib import Path

import numpy as np
import pytest
from astropy.io import fits

from astrometa import db, disposition, inventory


def _frame(conn, h, path="/a/b.fit"):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition) VALUES (?,?,?,1,1.0,'light','present')""",
        (h, path, Path(path).name))
    conn.commit()


def test_vanished_frame_becomes_missing_not_deleted(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1"); _frame(conn, "h2")
    assert disposition.mark_missing(conn, {"h1"}) == 1
    rows = dict(conn.execute("SELECT content_hash, disposition FROM frames"))
    assert rows["h1"] == "present" and rows["h2"] == "missing"
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 2


def test_mark_missing_refuses_empty_seen_hashes_when_frames_are_present(tmp_path):
    # Correction: an empty seen_hashes set against a non-empty archive must
    # never be read as "the operator deleted everything" -- it means the
    # scan itself failed (unmounted root, aborted walk, ...). Task 5's
    # inventory.scan() already raises on a missing root, which closes
    # today's path to this happening -- but mark_missing must refuse the
    # whole *class* of empty-scan-result, not rely on callers always going
    # through scan() first.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1"); _frame(conn, "h2")
    with pytest.raises(ValueError, match="empty"):
        disposition.mark_missing(conn, set())
    # nothing was touched by the refused call
    rows = dict(conn.execute("SELECT content_hash, disposition FROM frames"))
    assert rows["h1"] == "present" and rows["h2"] == "present"


def test_mark_missing_allows_empty_seen_hashes_when_no_frames_are_present(tmp_path):
    # The guard exists to protect real data, not to forbid every empty
    # set unconditionally -- an empty database (or one with nothing left
    # in 'present') has nothing to protect, and legitimately produces 0.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    assert disposition.mark_missing(conn, set()) == 0


def test_mark_culled_records_reason_and_source(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1")
    disposition.mark_culled(conn, "h1", "operator cull 2026-09-07", "manual")
    row = conn.execute("SELECT disposition, disposition_reason, "
                       "disposition_source FROM frames").fetchone()
    assert row == ("quarantined", "operator cull 2026-09-07", "manual")


def test_quarantine_dry_run_does_not_touch_disk(tmp_path):
    src = tmp_path / "M 42" / "Light_M 42_0001.fit"
    src.parent.mkdir(parents=True); src.write_bytes(b"x")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))
    dest = disposition.quarantine(conn, "h1", "hfd", dry_run=True)
    assert src.exists()
    assert dest == src.parent / "rejected" / src.name
    # dry run must not touch the database either
    row = conn.execute("SELECT disposition FROM frames WHERE content_hash='h1'").fetchone()
    assert row[0] == "present"


def test_quarantine_moves_and_never_deletes(tmp_path):
    src = tmp_path / "M 42" / "Light_M 42_0001.fit"
    src.parent.mkdir(parents=True); src.write_bytes(b"x")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))
    dest = disposition.quarantine(conn, "h1", "hfd 6.2 > 5.1", dry_run=False)
    assert not src.exists()
    assert dest.exists() and dest.read_bytes() == b"x"
    row = conn.execute("SELECT path, disposition, disposition_reason, "
                       "disposition_source FROM frames WHERE content_hash='h1'").fetchone()
    assert row == (str(dest), "quarantined", "hfd 6.2 > 5.1", "auto")


def test_quarantine_unknown_hash_returns_none(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    assert disposition.quarantine(conn, "does-not-exist", "hfd") is None


def test_quarantine_raises_loudly_when_source_file_already_gone(tmp_path):
    # The DB says a frame lives at `path`, but the file isn't there. That
    # disagreement is exactly the kind of silent-fallback trap this task
    # exists to close -- surfacing it (even in dry_run) beats returning a
    # dest path that a real run could never actually produce.
    src = tmp_path / "M 42" / "Light_M 42_0002.fit"
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))
    with pytest.raises(FileNotFoundError):
        disposition.quarantine(conn, "h1", "hfd", dry_run=True)
    with pytest.raises(FileNotFoundError):
        disposition.quarantine(conn, "h1", "hfd", dry_run=False)


def test_quarantine_is_idempotent_on_repeat_calls(tmp_path):
    # A quality-gate re-run or an operator re-applying mark_culled then
    # quarantine must not nest rejected/rejected/ on the second call --
    # once a frame is quarantined, quarantine() is a no-op that reports
    # where the frame already lives.
    src = tmp_path / "M 42" / "Light_M 42_0001.fit"
    src.parent.mkdir(parents=True); src.write_bytes(b"x")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))

    dest1 = disposition.quarantine(conn, "h1", "hfd 6.2 > 5.1", dry_run=False)
    dest2 = disposition.quarantine(conn, "h1", "hfd re-check", dry_run=False)

    assert dest2 == dest1
    assert dest2.parent.name == "rejected"
    assert not (dest1.parent / "rejected").exists()   # no nested rejected/rejected
    assert dest2.exists() and dest2.read_bytes() == b"x"
    # The repeat call is a true no-op: it doesn't touch the database, so
    # the originally recorded reason survives rather than being silently
    # overwritten by the second call's (different) reason.
    row = conn.execute("SELECT disposition_reason FROM frames WHERE "
                       "content_hash='h1'").fetchone()
    assert row[0] == "hfd 6.2 > 5.1"


def test_quarantine_treats_a_path_already_under_rejected_as_idempotent(tmp_path):
    # Defense in depth beyond the disposition check: even if disposition
    # somehow isn't (yet) 'quarantined', a recorded path that already
    # lives under a rejected/ directory must never be nested into
    # rejected/rejected/ -- the structural signal is checked independently.
    rejected_dir = tmp_path / "M 42" / "rejected"
    rejected_dir.mkdir(parents=True)
    f = rejected_dir / "Light_M 42_0001.fit"
    f.write_bytes(b"x")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(f))   # disposition column is still 'present'
    dest = disposition.quarantine(conn, "h1", "hfd", dry_run=False)
    assert dest == f
    assert f.exists()
    assert not (rejected_dir / "rejected").exists()


def test_quarantine_refuses_to_overwrite_an_existing_destination(tmp_path):
    # The operator's own manual culling method is "move the bad sub into
    # rejected/ by hand" -- that directory routinely already holds files
    # he put there himself, and filenames in this archive are not
    # unique. A machine silently overwriting a hand-culled frame via
    # shutil.move's os.rename fallback would itself be a deletion, on
    # the one function in this system that promises never to delete.
    src = tmp_path / "M 42" / "Light_M 42_0001.fit"
    src.parent.mkdir(parents=True); src.write_bytes(b"new frame")
    existing = src.parent / "rejected" / src.name
    existing.parent.mkdir(parents=True)
    existing.write_bytes(b"operator's hand-culled frame")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))

    with pytest.raises(FileExistsError):
        disposition.quarantine(conn, "h1", "hfd", dry_run=False)
    assert src.exists() and src.read_bytes() == b"new frame"
    assert existing.read_bytes() == b"operator's hand-culled frame"


def test_quarantine_dry_run_also_refuses_a_destination_collision(tmp_path):
    # Consistent with the FileNotFoundError check: a dry run that reports
    # a move as feasible when it would actually clobber something is
    # itself a silent-fallback bug.
    src = tmp_path / "M 42" / "Light_M 42_0001.fit"
    src.parent.mkdir(parents=True); src.write_bytes(b"new frame")
    existing = src.parent / "rejected" / src.name
    existing.parent.mkdir(parents=True)
    existing.write_bytes(b"operator's hand-culled frame")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", str(src))

    with pytest.raises(FileExistsError):
        disposition.quarantine(conn, "h1", "hfd", dry_run=True)
    assert src.exists()
    assert existing.read_bytes() == b"operator's hand-culled frame"


def test_session_thresholds_scale_with_the_night():
    good = [2.0, 2.1, 2.2, 2.0, 2.3]
    poor = [5.0, 5.2, 5.1, 5.3, 5.1]
    assert disposition.session_thresholds(good)[0] < \
           disposition.session_thresholds(poor)[0]


def test_session_thresholds_falls_back_to_absolute_floor_with_too_few_samples():
    lo, floor = disposition.session_thresholds([4.0, 4.1])
    assert lo == floor == disposition.ABSOLUTE_HFD_FLOOR


def test_backfill_known_culls(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "/a/Light_IC 59_HaO3_0019.fit")
    res = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0019%",
         "reason": "operator cull, night of 2026-09-07"}])
    assert res.applied == 1
    assert conn.execute("SELECT disposition FROM frames").fetchone()[0] \
        == "quarantined"


def test_backfill_known_culls_pattern_discriminates_by_filter(tmp_path):
    # Real data trap (2026-09-07 IC 59 culls): the Lqef and HaO3 filter
    # lists overlap on frame index (0038, 0039, 0040 appear in both). A
    # pattern keyed on index alone would over-match across filters and
    # quarantine the wrong frame. Two frames sharing an index but
    # differing by filter must be told apart.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "/a/Light_IC 59_Lqef_0038.fit")
    _frame(conn, "h2", "/a/Light_IC 59_HaO3_0038.fit")
    res = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0038%", "reason": "HaO3 cull 0038"}])
    assert res.applied == 1
    rows = dict(conn.execute("SELECT content_hash, disposition FROM frames"))
    assert rows["h1"] == "present"
    assert rows["h2"] == "quarantined"


def test_backfill_known_culls_multiple_patterns_sum(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "/a/Light_IC 59_Lqef_0006.fit")
    _frame(conn, "h2", "/a/Light_IC 59_HaO3_0019.fit")
    _frame(conn, "h3", "/a/Light_IC 59_HaO3_0041.fit")  # outside the cull range
    res = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%Lqef%0006%", "reason": "Lqef cull 0006"},
        {"filename_like": "%IC 59%HaO3%0019%", "reason": "HaO3 cull 0019"},
    ])
    assert res.applied == 2
    rows = dict(conn.execute("SELECT content_hash, disposition FROM frames"))
    assert rows["h1"] == "quarantined"
    assert rows["h2"] == "quarantined"
    assert rows["h3"] == "present"


def test_mark_missing_never_touches_a_quarantined_frame(tmp_path):
    # The store's ownership split: inventory (mark_missing) owns the
    # present/missing transition; the operator (mark_culled/quarantine)
    # owns quarantined. A quarantined frame legitimately absent from a
    # scan's seen_hashes is not lost, it's filed -- it must stay
    # 'quarantined', never regress to 'missing'.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1")
    disposition.mark_culled(conn, "h1", "operator cull", "manual")
    assert disposition.mark_missing(conn, set()) == 0
    assert conn.execute("SELECT disposition FROM frames WHERE "
                        "content_hash='h1'").fetchone()[0] == "quarantined"


def _write_light(path, obj="IC 59"):
    rng = np.random.default_rng(1)
    data = rng.normal(100, 5, (64, 64)).astype(np.float32)
    hdu = fits.PrimaryHDU(data)
    hdu.header["OBJECT"] = obj
    hdu.header["INSTRUME"] = "ZWO ASI585MC Air"
    hdu.header["FILTER"] = "HaO3"
    hdu.header["EXPTIME"] = 120.0
    hdu.header["IMAGETYP"] = "Light Frame"
    hdu.writeto(path, overwrite=True)


def test_quarantine_survives_a_rescan(tmp_path):
    # Integration regression guard for the real bug: quarantine() moves a
    # frame's file into a sibling rejected/ dir, still inside the scanned
    # archive root. A naive inventory.scan() re-walking that root finds
    # the same content_hash again and, left unguarded, its ON CONFLICT
    # upsert would silently reset disposition back to 'present' --
    # erasing the operator's (or the quality gate's) recorded intent one
    # layer below where this task's mark_missing guard operates. Only
    # inventory.py's ON CONFLICT clause was touched to fix this (directed
    # by team-lead, not unilateral scope creep by task 11) -- see task-11
    # report for detail.
    root = tmp_path / "astro_data" / "IC 59"
    root.mkdir(parents=True)
    src = root / "Light_IC 59_120.0s_Bin1_HaO3_20260907-220000_0deg_0038.fit"
    _write_light(src)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)

    inventory.scan(conn, [tmp_path / "astro_data"])
    chash = conn.execute("SELECT content_hash FROM frames").fetchone()[0]

    dest = disposition.quarantine(conn, chash, "hfd 6.2 > 5.1", dry_run=False)
    assert dest is not None and dest.exists()
    assert dest.is_relative_to(tmp_path / "astro_data")  # still under the scan root

    inventory.scan(conn, [tmp_path / "astro_data"])

    row = conn.execute("SELECT disposition, disposition_reason, path "
                       "FROM frames WHERE content_hash=?", (chash,)).fetchone()
    assert row[0] == "quarantined"
    assert row[1] == "hfd 6.2 > 5.1"
    assert row[2] == str(dest)


# --- FIX 7: a cull matching nothing is the case this project exists for


def test_backfill_reports_unmatched_culls_instead_of_a_silent_zero(tmp_path):
    # The motivating case. The 39 real IC 59 culls are absent from BOTH
    # the live NAS and the ZFS backup (verified 2026-09-09: both hold 91
    # frames, HaO3 indices stop at 0018 while the culled range is
    # 0019-0040). They survive only on the camera, which is not one of
    # the store's roots -- so there is no frames row to UPDATE, and the
    # old implementation returned 0 having silently done nothing, on
    # exactly the case this project was built for.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "/a/Light_IC 59_HaO3_0018.fit")   # not culled

    res = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0019%",
         "reason": "operator cull, night of 2026-09-07"}])

    assert res.applied == 0
    assert res.matched == 0
    assert res.unmatched == 1
    assert res.unmatched_patterns == ["%IC 59%HaO3%0019%"]


def test_an_unmatched_cull_is_persisted_independently_of_any_frame_row(tmp_path):
    # There may never be a frames row for these culls -- the bytes exist
    # nowhere the store can reach. Fabricating a row with an invented
    # content_hash would corrupt the store's central identity claim
    # ("the bytes of this file"), so the knowledge is kept in its own
    # table instead.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0019%",
         "reason": "operator cull, night of 2026-09-07"}])

    row = conn.execute(
        "SELECT pattern, reason, recorded_at, matched "
        "FROM known_culls").fetchone()
    assert row[0] == "%IC 59%HaO3%0019%"
    assert row[1] == "operator cull, night of 2026-09-07"
    assert row[2] is not None and row[2] != ""
    assert row[3] == 0
    # No frames row was invented for a file that exists nowhere.
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 0


def test_matched_culls_are_recorded_too(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "/a/Light_IC 59_HaO3_0019.fit")
    res = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0019%", "reason": "HaO3 cull 0019"}])
    assert res.matched == 1 and res.unmatched == 0
    assert conn.execute(
        "SELECT matched FROM known_culls").fetchone()[0] == 1


def test_backfill_is_idempotent_and_self_corrects_when_a_frame_appears(tmp_path):
    # Re-running must not duplicate the cull record, and if the frame
    # later turns up (restored from the camera), the recorded match count
    # must catch up rather than stay stale at 0.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    culls = [{"filename_like": "%IC 59%HaO3%0019%", "reason": "HaO3 cull 0019"}]

    first = disposition.backfill_known_culls(conn, culls)
    assert first.unmatched == 1
    recorded_at = conn.execute(
        "SELECT recorded_at FROM known_culls").fetchone()[0]

    disposition.backfill_known_culls(conn, culls)
    assert conn.execute("SELECT COUNT(*) FROM known_culls").fetchone()[0] == 1

    _frame(conn, "h1", "/a/Light_IC 59_HaO3_0019.fit")
    third = disposition.backfill_known_culls(conn, culls)
    assert third.matched == 1 and third.unmatched == 0
    row = conn.execute(
        "SELECT matched, recorded_at FROM known_culls").fetchone()
    assert row[0] == 1
    # recorded_at is when this knowledge was FIRST written down.
    assert row[1] == recorded_at
    assert conn.execute(
        "SELECT disposition FROM frames").fetchone()[0] == "quarantined"
