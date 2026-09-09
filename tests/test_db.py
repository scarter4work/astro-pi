import sqlite3
from astrometa import db

def test_init_schema_creates_expected_tables(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite")
    db.init_schema(conn)
    names = {r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"frames", "fields", "objects", "aliases",
            "identity_assertions", "quality", "projects",
            "frame_projects"} <= names

def test_init_schema_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite")
    db.init_schema(conn)
    db.init_schema(conn)          # must not raise
    conn.execute("INSERT INTO frames (content_hash, path, filename, size, mtime, "
                 "frame_type, disposition) VALUES ('h1','/a/b.fit','b.fit',1,1.0,"
                 "'light','present')")
    db.init_schema(conn)
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 1

def test_frames_primary_key_is_content_hash(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite")
    db.init_schema(conn)
    conn.execute("INSERT INTO frames (content_hash, path, filename, size, mtime, "
                 "frame_type, disposition) VALUES ('h1','/a/b.fit','b.fit',1,1.0,"
                 "'light','present')")
    try:
        conn.execute("INSERT INTO frames (content_hash, path, filename, size, mtime, "
                     "frame_type, disposition) VALUES ('h1','/c/d.fit','d.fit',1,1.0,"
                     "'light','present')")
        raise AssertionError("duplicate content_hash must be rejected")
    except sqlite3.IntegrityError:
        pass
