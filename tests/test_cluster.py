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

def _fp_pair(distance: int, width_hex: int = 64) -> tuple[str, str]:
    """
    Two width_hex-char hex fingerprints exactly `distance` bits apart
    (width_hex=64 matches the 256-bit width imagekeys.fingerprint() emits
    at its default size=16). Flipping the `distance` low-order bits of an
    all-zero base gives an exact, controlled Hamming distance -- unlike
    the 8-hex-char (32-bit) literals used elsewhere in this file, which
    cap out at a max possible distance of 32 and so cannot exercise
    fp_threshold values above that.
    """
    base = 0
    other = base ^ ((1 << distance) - 1)
    return f"{base:0{width_hex}x}", f"{other:0{width_hex}x}"

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
    # fp_threshold defaults to 80 (see FP_THRESHOLD_DEFAULT); an 8-hex-char
    # (32-bit) fingerprint pair can never exceed a distance of 32, so this
    # needs a wider pair to actually exercise "different fingerprint" at
    # the real default -- see _fp_pair.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    fp1, fp2 = _fp_pair(200)
    _insert(conn, "h1", fp1, 42.86, 60.07)
    _insert(conn, "h2", fp2, 42.86, 60.07)
    assert cluster.assign_fields(conn) == 2

def test_measured_same_field_distance_clusters(tmp_path):
    # Real-data check (2026-09-09, see FP_THRESHOLD_DEFAULT): same-field
    # frames measured 20-45 bits apart (excluding meridian flips). 45 is
    # the observed same-field maximum and must still cluster.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    fp1, fp2 = _fp_pair(45)
    _insert(conn, "h1", fp1, 42.86, 60.07)
    _insert(conn, "h2", fp2, 42.86, 60.07)
    assert cluster.assign_fields(conn) == 1

def test_measured_different_field_distance_does_not_cluster(tmp_path):
    # Real-data check (2026-09-09, see FP_THRESHOLD_DEFAULT): different
    # fields measured 113+ bits apart. 113 is the observed between-field
    # minimum and must still NOT cluster.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    fp1, fp2 = _fp_pair(113)
    _insert(conn, "h1", fp1, 42.86, 60.07)
    _insert(conn, "h2", fp2, 42.86, 60.07)
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
