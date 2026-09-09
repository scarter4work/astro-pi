from datetime import date, datetime

from astrometa import db, grouping, naming


def _object(conn, name="M42"):
    return naming.upsert_object(conn, name, "deepsky")


def _field(conn, object_id):
    return conn.execute(
        "INSERT INTO fields (solve_source, object_id) VALUES ('none', ?)",
        (object_id,)).lastrowid


def _frame(conn, content_hash, filename, field_id=None, filt="Ha",
           object_card=None, leaf_dir=None):
    conn.execute("""INSERT INTO frames (content_hash, path, filename, size,
        mtime, frame_type, disposition, filter, field_id, object_card,
        leaf_dir)
        VALUES (?,?,?,1,1.0,'light','present',?,?,?,?)""",
        (content_hash, f"/x/{leaf_dir or 'd'}/{filename}", filename, filt,
         field_id, object_card, leaf_dir))


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

    assert grouping.build_projects(conn).projects == 1
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

    assert grouping.build_projects(conn).projects == 1
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

    assert grouping.build_projects(conn).projects == 1
    assert conn.execute("SELECT kind FROM projects").fetchone()[0] == "mosaic"
    panels = dict(conn.execute(
        "SELECT content_hash, panel FROM frame_projects"))
    assert panels == {"h1": "1-1", "h2": "1-2"}


def test_build_projects_skips_frames_with_no_capture_instant(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "M42")
    fid = _field(conn, oid)
    _frame(conn, "h1", "Autosave001.fit", fid)

    assert grouping.build_projects(conn).projects == 0
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
    assert first.projects == second.projects == 1
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
    assert grouping.build_projects(conn).projects == 1
    assert conn.execute("SELECT COUNT(*) FROM projects").fetchone()[0] == 1
    assert conn.execute(
        "SELECT COUNT(*) FROM frame_projects").fetchone()[0] == 2


# --- FIX 4: an unresolved object_id must not lump every target together


def test_different_targets_same_night_same_filter_are_separate_projects(tmp_path):
    # Nothing populates fields.object_id yet (live SIMBAD resolution is
    # deliberately deferred), so object_id is NULL for every frame in the
    # archive. Bucketing on (object_id, filter, session_date) alone
    # therefore collapsed EVERY target shot on one night with one filter
    # into a single "project".
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1",
           "Light_IC 59_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit",
           object_card="IC 59", leaf_dir="IC 59")
    _frame(conn, "h2",
           "Light_M 42_120.0s_Bin1_HaO3_20260908-230112_182deg_0001.fit",
           object_card="M 42", leaf_dir="M 42")

    assert grouping.build_projects(conn).projects == 2
    names = {r[0] for r in conn.execute(
        "SELECT identity_name FROM projects")}
    assert names == {"IC 59", "M 42"}


def test_identity_falls_back_to_leaf_dir_when_no_object_card(tmp_path):
    # 85% of leaf directories hold frames with no OBJECT card at all; the
    # directory name is the target name in practice.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1",
           "Light_a_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit",
           leaf_dir="Veil2")
    _frame(conn, "h2",
           "Light_b_120.0s_Bin1_HaO3_20260908-230112_182deg_0001.fit",
           leaf_dir="NGC6960")

    assert grouping.build_projects(conn).projects == 2
    rows = {r[0]: r[1] for r in conn.execute(
        "SELECT identity_name, identity_source FROM projects")}
    assert rows == {"Veil2": "dirname", "NGC6960": "dirname"}


def test_identity_source_records_how_the_bucket_was_named(tmp_path):
    # A fallback identity must never be mistaken for a solved one: the
    # confidence model (spec 7) rates `object_card` medium and `dirname`
    # low, and neither may drive a Phase 2 move.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1",
           "Light_IC 59_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit",
           object_card="IC 59", leaf_dir="whatever")
    assert grouping.build_projects(conn).projects == 1
    row = conn.execute(
        "SELECT identity_name, identity_source FROM projects").fetchone()
    assert row == ("IC 59", "object_card")


def test_resolved_object_id_takes_precedence_over_the_fallback(tmp_path):
    # Once object_id IS resolved it is authoritative -- two frames of the
    # same object filed under differently-spelled directories (M31 vs
    # M 31, both real sibling directories in this archive) must still
    # land in ONE project, which is the whole reason object_id is the
    # primary key of the bucket.
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = _object(conn, "M31")
    fid = _field(conn, oid)
    _frame(conn, "h1",
           "Light_M31_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit",
           fid, object_card="M31", leaf_dir="M31")
    _frame(conn, "h2",
           "Light_M 31_120.0s_Bin1_HaO3_20260908-230112_182deg_0001.fit",
           fid, object_card="M 31", leaf_dir="M 31")

    assert grouping.build_projects(conn).projects == 1
    row = conn.execute(
        "SELECT object_id, identity_name, identity_source "
        "FROM projects").fetchone()
    assert row == (oid, None, "object")


def test_frames_with_no_identity_at_all_still_group_by_night(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    _frame(conn, "h1",
           "Light_x_120.0s_Bin1_HaO3_20260908-225448_182deg_0001.fit")
    assert grouping.build_projects(conn).projects == 1
    row = conn.execute(
        "SELECT identity_name, identity_source FROM projects").fetchone()
    assert row == (None, "unknown")
