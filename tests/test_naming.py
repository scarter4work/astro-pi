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
