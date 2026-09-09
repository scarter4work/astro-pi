import math
from astrometa import db, cluster

def _insert(conn, h, fp, ra, dec, focallen=491.0, xpixsz=2.9,
            naxis1=3840, naxis2=2160):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition, fingerprint, header_ra, header_dec,
        focallen, xpixsz, naxis1, naxis2)
        VALUES (?,?,?,1,1.0,'light','present',?,?,?,?,?,?,?)""",
        (h, f"/x/{h}.fit", f"{h}.fit", fp, ra, dec, focallen, xpixsz,
         naxis1, naxis2))

def test_angular_separation_is_correct():
    assert abs(cluster.angular_separation(0, 0, 0, 1) - 1.0) < 1e-9
    assert abs(cluster.angular_separation(10, 60, 11, 60) - 0.5) < 0.01

def test_frames_of_same_field_share_a_field_id(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", 42.86, 60.07)
    _insert(conn, "h2", "ffff0001", 42.87, 60.08)
    assert cluster.assign_fields(conn) == 1
    ids = [r[0] for r in conn.execute("SELECT field_id FROM frames")]
    assert ids[0] == ids[1] and ids[0] is not None

def test_distant_pointings_are_separate_fields(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", 42.86, 60.07)
    _insert(conn, "h2", "ffff0001", 10.00, -20.00)
    assert cluster.assign_fields(conn) == 2

def test_same_pointing_different_fingerprint_is_separate(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "0000ffff", 42.86, 60.07)
    _insert(conn, "h2", "ffff0000", 42.86, 60.07)
    assert cluster.assign_fields(conn) == 2

def test_assign_fields_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", 42.86, 60.07)
    cluster.assign_fields(conn)
    cluster.assign_fields(conn)
    assert conn.execute("SELECT COUNT(*) FROM fields").fetchone()[0] == 1

def test_frames_without_pointing_are_left_unassigned(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _insert(conn, "h1", "ffff0000", None, None)
    cluster.assign_fields(conn)
    assert conn.execute("SELECT field_id FROM frames").fetchone()[0] is None
