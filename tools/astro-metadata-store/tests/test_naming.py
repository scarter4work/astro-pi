import pytest
from astrometa import db, naming


@pytest.mark.parametrize("raw,expected", [
    ("M 42", "M42"), ("M42", "M42"), ("m42", "M42"), ("  M  42 ", "M42"),
    ("NGC 7635", "NGC7635"), ("ngc7635", "NGC7635"),
    ("IC 1848", "IC1848"), ("Sh2-106", "SH2-106"), ("SH2-101", "SH2-101"),
])
def test_canonicalise(raw, expected):
    assert naming.canonicalise(raw) == expected


def test_canonicalise_leaves_proper_names_alone():
    assert naming.canonicalise("Triangulum Galaxy") == "Triangulum Galaxy"


def test_upsert_object_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    a = naming.upsert_object(conn, "M42", "deepsky")
    b = naming.upsert_object(conn, "M42", "deepsky")
    assert a == b
    assert conn.execute("SELECT COUNT(*) FROM objects").fetchone()[0] == 1


def test_aliases_collapse_to_one_object(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M33", "deepsky")
    naming.add_alias(conn, oid, "Triangulum Galaxy", "dirname")
    naming.add_alias(conn, oid, "M 33", "dirname")
    naming.add_alias(conn, oid, "M 33", "dirname")     # duplicate is a no-op
    assert conn.execute("SELECT COUNT(*) FROM aliases").fetchone()[0] == 2


def test_confidence_gate_allows_only_trusted_sources():
    assert naming.may_drive_move("solved") is True
    assert naming.may_drive_move("propagated") is True
    assert naming.may_drive_move("manual") is True
    assert naming.may_drive_move("object_card") is False
    assert naming.may_drive_move("dirname") is False


def test_assert_identity_records_confidence(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M42", "deepsky")
    fid = conn.execute("INSERT INTO fields (solve_source) "
                       "VALUES ('astap')").lastrowid
    naming.assert_identity(conn, fid, oid, "solved")
    row = conn.execute("SELECT source, confidence FROM "
                       "identity_assertions").fetchone()
    assert row == ("solved", "high")


@pytest.mark.parametrize("source,confidence", [
    ("dirname", "low"),
    ("object_card", "medium"),
])
def test_assert_identity_weak_sources_never_set_object_id(tmp_path, source, confidence):
    # The confidence gate's most important property: a claim from a
    # source that may_drive_move() rejects must be recorded (never
    # silently dropped) but must never move fields.object_id -- that
    # column is what a future task reads to decide what may be
    # physically moved on disk.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M42", "deepsky")
    fid = conn.execute("INSERT INTO fields (solve_source) "
                       "VALUES ('none')").lastrowid

    naming.assert_identity(conn, fid, oid, source)

    row = conn.execute("SELECT source, confidence FROM "
                       "identity_assertions").fetchone()
    assert row == (source, confidence)
    assert conn.execute("SELECT object_id FROM fields WHERE id=?",
                        (fid,)).fetchone()[0] is None


def test_assert_identity_solved_claim_still_sets_object_id_after_weak_claims(tmp_path):
    # A weak claim recorded earlier must not poison the field against a
    # later trustworthy one -- the gate has to keep opening for solved/
    # propagated/manual regardless of what low-confidence noise already
    # sits in identity_assertions for the same field.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M42", "deepsky")
    fid = conn.execute("INSERT INTO fields (solve_source) "
                       "VALUES ('none')").lastrowid

    naming.assert_identity(conn, fid, oid, "dirname")
    naming.assert_identity(conn, fid, oid, "object_card")
    assert conn.execute("SELECT object_id FROM fields WHERE id=?",
                        (fid,)).fetchone()[0] is None

    naming.assert_identity(conn, fid, oid, "solved")
    assert conn.execute("SELECT object_id FROM fields WHERE id=?",
                        (fid,)).fetchone()[0] == oid
    # the weak claims are still on the record, not overwritten/removed
    assert conn.execute("SELECT COUNT(*) FROM identity_assertions"
                        ).fetchone()[0] == 3


def test_upsert_object_position_is_coalesced_not_clobbered(tmp_path):
    # Exercises the ra/dec COALESCE in both directions: a later call
    # supplying new position data must adopt it (proves the columns are
    # actually reachable on conflict, not silently excluded from the
    # UPDATE), and a still-later call supplying none must not clobber
    # what's already stored back to NULL (the common case while SIMBAD
    # resolution is out of scope -- see module docstring).
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)

    a = naming.upsert_object(conn, "M42", "deepsky")
    row = conn.execute("SELECT simbad_id, ra, dec FROM objects "
                       "WHERE id=?", (a,)).fetchone()
    assert row == (None, None, None)

    b = naming.upsert_object(conn, "M42", "deepsky",
                             simbad_id="M 42", ra=83.82, dec=-5.39)
    assert b == a
    row = conn.execute("SELECT simbad_id, ra, dec FROM objects "
                       "WHERE id=?", (a,)).fetchone()
    assert row == ("M 42", 83.82, -5.39)

    c = naming.upsert_object(conn, "M42", "deepsky")   # no new position data
    assert c == a
    row = conn.execute("SELECT simbad_id, ra, dec FROM objects "
                       "WHERE id=?", (a,)).fetchone()
    assert row == ("M 42", 83.82, -5.39)               # not clobbered


def test_assert_identity_raises_on_unknown_source(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M42", "deepsky")
    fid = conn.execute("INSERT INTO fields (solve_source) "
                       "VALUES ('none')").lastrowid
    with pytest.raises(KeyError):
        naming.assert_identity(conn, fid, oid, "guess")
