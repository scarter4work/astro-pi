from datetime import date, datetime

from astrometa import db, grouping, naming


def _object(conn, name="M42"):
    return naming.upsert_object(conn, name, "deepsky")


def _field(conn, object_id):
    return conn.execute(
        "INSERT INTO fields (solve_source, object_id) VALUES ('none', ?)",
        (object_id,)).lastrowid


def _frame(conn, content_hash, filename, field_id=None, filt="Ha"):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition, filter, field_id)
        VALUES (?,?,?,1,1.0,'light','present',?,?)""",
        (content_hash, f"/x/{filename}", filename, filt, field_id))


def test_capture_instant_parsed():
    i = grouping.capture_instant(
        "Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit")
    assert i == datetime(2026, 9, 9, 1, 49, 8)


def test_capture_instant_absent_returns_none():
    assert grouping.capture_instant("Autosave001.fit") is None


def test_after_midnight_belongs_to_previous_night():
    assert grouping.session_date(datetime(2026, 9, 9, 1, 49)) == date(2026, 9, 8)


def test_evening_belongs_to_same_night():
    assert grouping.session_date(datetime(2026, 9, 8, 22, 54)) == date(2026, 9, 8)


def test_dusk_boundary_is_inclusive():
    assert grouping.session_date(datetime(2026, 9, 8, 17, 0)) == date(2026, 9, 8)
    assert grouping.session_date(datetime(2026, 9, 8, 16, 59)) == date(2026, 9, 7)


def test_panel_extracted_from_name_with_spaces():
    assert grouping.panel_of(
        "Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit") == "1-1"


def test_no_panel_returns_none():
    assert grouping.panel_of(
        "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit") is None


def test_panel_with_double_digit_column():
    # Regression: M42's real archive panels run into double digits
    # (1-10 through 8-12). A `\d-\d` pattern requires the char right
    # after the second digit to be "_", so "_1-10_" (second digit "1",
    # next char "0") silently fails to match and returns None.
    assert grouping.panel_of(
        "Light_M42_1-10_120.0s_Bin1_L_20230204-220000_0deg_0001.fit") == "1-10"


def test_panel_with_double_digit_row():
    assert grouping.panel_of(
        "Light_M42_10-2_120.0s_Bin1_L_20230204-220000_0deg_0001.fit") == "10-2"


def test_datestamp_is_not_mistaken_for_a_panel():
    # The widened \d+-\d+ pattern must still not match a bare datestamp
    # token -- it requires the trailing \d+(.\d+)?s_ exposure suffix,
    # which "182deg_" never satisfies.
    assert grouping.panel_of(
        "Light_M 20_120.0s_Bin1_L_20260713-225546_182deg_0001.fit") is None


def test_build_projects_groups_same_night_frames_into_one_session(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "SH2-106")
    fid = _field(conn, oid)
    _frame(conn, "h1",
           "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit", fid)
    _frame(conn, "h2",
           "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-230112_182deg_0002.fit", fid)

    assert grouping.build_projects(conn) == 1
    row = conn.execute("SELECT kind, object_id, filter FROM projects").fetchone()
    assert row == ("session", oid, "Ha")
    members = {r[0] for r in conn.execute(
        "SELECT content_hash FROM frame_projects")}
    assert members == {"h1", "h2"}


def test_build_projects_crossing_midnight_stays_one_session(tmp_path):
    # The real-world failure the dusk rule fixes: SH2-129 started 2026-07's
    # night at 22:54 and ran past midnight to 01:39, all under the NEXT
    # day's datestamp. Both frames must land in the same project.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "SH2-129")
    fid = _field(conn, oid)
    _frame(conn, "h1",
           "Light_Sh2-129_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit", fid)
    _frame(conn, "h2",
           "Light_Sh2-129_120.0s_Bin1_HaO3_20260909-013900_182deg_0100.fit", fid)

    assert grouping.build_projects(conn) == 1
    members = {r[0] for r in conn.execute(
        "SELECT content_hash FROM frame_projects")}
    assert members == {"h1", "h2"}


def test_build_projects_detects_mosaic_when_panels_present(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "IC1848")
    fid = _field(conn, oid)
    _frame(conn, "h1",
           "Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit", fid)
    _frame(conn, "h2",
           "Light_IC 1848_1-2_120.0s_Bin1_HaO3_20260909-015200_182deg_0019.fit", fid)

    assert grouping.build_projects(conn) == 1
    assert conn.execute("SELECT kind FROM projects").fetchone()[0] == "mosaic"
    panels = dict(conn.execute(
        "SELECT content_hash, panel FROM frame_projects"))
    assert panels == {"h1": "1-1", "h2": "1-2"}


def test_build_projects_skips_frames_with_no_capture_instant(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "M42")
    fid = _field(conn, oid)
    _frame(conn, "h1", "Autosave001.fit", fid)

    assert grouping.build_projects(conn) == 0
    assert conn.execute("SELECT COUNT(*) FROM projects").fetchone()[0] == 0
    assert conn.execute(
        "SELECT COUNT(*) FROM frame_projects").fetchone()[0] == 0


def test_build_projects_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "SH2-106")
    fid = _field(conn, oid)
    _frame(conn, "h1",
           "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit", fid)
    _frame(conn, "h2",
           "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-230112_182deg_0002.fit", fid)

    first = grouping.build_projects(conn)
    second = grouping.build_projects(conn)
    assert first == second == 1
    assert conn.execute("SELECT COUNT(*) FROM projects").fetchone()[0] == 1
    assert conn.execute(
        "SELECT COUNT(*) FROM frame_projects").fetchone()[0] == 2


def test_build_projects_rerun_after_new_frame_added_still_one_project_per_night(tmp_path):
    # Incremental-pull scenario: a third frame from the same night arrives
    # on a later run. Rebuild must fold it into the SAME session rather
    # than creating a duplicate project for the night.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "SH2-106")
    fid = _field(conn, oid)
    _frame(conn, "h1",
           "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit", fid)
    grouping.build_projects(conn)

    _frame(conn, "h2",
           "Light_Sh2-106_120.0s_Bin1_HaO3_20260908-230112_182deg_0002.fit", fid)
    assert grouping.build_projects(conn) == 1
    assert conn.execute("SELECT COUNT(*) FROM projects").fetchone()[0] == 1
    assert conn.execute(
        "SELECT COUNT(*) FROM frame_projects").fetchone()[0] == 2
