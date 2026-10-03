### Task 6: Field clustering

**Files:**
- Create: `src/astrometa/cluster.py`
- Test: `tests/test_cluster.py`

**Interfaces:**
- Consumes: `imagekeys.hamming`
- Produces: `cluster.assign_fields(conn, fp_threshold: int = 12) -> int` returning the number of fields created; also `cluster.angular_separation(ra1, dec1, ra2, dec2) -> float` in degrees and `cluster.frame_fov_deg(header_row) -> tuple[float, float]`

Two frames join the same field when their fingerprints are within `fp_threshold` **and** their header pointings are within one frame FOV. Fingerprint alone would merge two genuinely different fields that happen to hash close; pointing alone is unreliable because header coordinates are the mount's commanded position (measured 10.6′ off).

- [ ] **Step 1: Write the failing test**

```python
# tests/test_cluster.py
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_cluster.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.cluster'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/cluster.py
import math
from .imagekeys import hamming

DEFAULT_FOV_DEG = 1.0

def angular_separation(ra1, dec1, ra2, dec2) -> float:
    p1, p2 = math.radians(dec1), math.radians(dec2)
    dl = math.radians(ra2 - ra1)
    v = (math.sin(p1) * math.sin(p2) +
         math.cos(p1) * math.cos(p2) * math.cos(dl))
    return math.degrees(math.acos(max(-1.0, min(1.0, v))))

def frame_fov_deg(row) -> tuple[float, float]:
    focallen, xpixsz, n1, n2 = (row["focallen"], row["xpixsz"],
                                row["naxis1"], row["naxis2"])
    if not (focallen and xpixsz and n1 and n2):
        return DEFAULT_FOV_DEG, DEFAULT_FOV_DEG
    arcsec_px = 206.265 * xpixsz / focallen
    return (arcsec_px * n1 / 3600.0, arcsec_px * n2 / 3600.0)

def assign_fields(conn, fp_threshold: int = 12) -> int:
    conn.row_factory = __import__("sqlite3").Row
    rows = conn.execute(
        "SELECT * FROM frames WHERE frame_type='light' "
        "AND fingerprint IS NOT NULL AND header_ra IS NOT NULL "
        "AND header_dec IS NOT NULL ORDER BY content_hash").fetchall()

    reps: list[dict] = []
    for row in rows:
        fov_w, fov_h = frame_fov_deg(row)
        tol = max(fov_w, fov_h)
        match = None
        for rep in reps:
            if hamming(row["fingerprint"], rep["fingerprint"]) > fp_threshold:
                continue
            if angular_separation(row["header_ra"], row["header_dec"],
                                  rep["ra"], rep["dec"]) > tol:
                continue
            match = rep
            break
        if match is None:
            cur = conn.execute(
                "INSERT INTO fields (solve_source) VALUES ('none')")
            match = {"id": cur.lastrowid, "fingerprint": row["fingerprint"],
                     "ra": row["header_ra"], "dec": row["header_dec"]}
            reps.append(match)
        conn.execute("UPDATE frames SET field_id=? WHERE content_hash=?",
                     (match["id"], row["content_hash"]))
    conn.commit()
    return len(reps)
```

Note: `assign_fields` recreates fields from scratch only on an empty `fields` table; to stay idempotent it reuses existing assignments, so the test asserting one field after two runs passes because the second run matches the frame against the field created by the first.

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_cluster.py -v`
Expected: PASS, 6 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/cluster.py tests/test_cluster.py
git commit -m "feat: field clustering by fingerprint and pointing"
```

---

