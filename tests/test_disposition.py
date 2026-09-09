from pathlib import Path

import pytest

from astrometa import db, disposition


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
    n = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0019%",
         "reason": "operator cull, night of 2026-09-07"}])
    assert n == 1
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
    n = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%HaO3%0038%", "reason": "HaO3 cull 0038"}])
    assert n == 1
    rows = dict(conn.execute("SELECT content_hash, disposition FROM frames"))
    assert rows["h1"] == "present"
    assert rows["h2"] == "quarantined"


def test_backfill_known_culls_multiple_patterns_sum(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1", "/a/Light_IC 59_Lqef_0006.fit")
    _frame(conn, "h2", "/a/Light_IC 59_HaO3_0019.fit")
    _frame(conn, "h3", "/a/Light_IC 59_HaO3_0041.fit")  # outside the cull range
    n = disposition.backfill_known_culls(conn, [
        {"filename_like": "%IC 59%Lqef%0006%", "reason": "Lqef cull 0006"},
        {"filename_like": "%IC 59%HaO3%0019%", "reason": "HaO3 cull 0019"},
    ])
    assert n == 2
    rows = dict(conn.execute("SELECT content_hash, disposition FROM frames"))
    assert rows["h1"] == "quarantined"
    assert rows["h2"] == "quarantined"
    assert rows["h3"] == "present"
