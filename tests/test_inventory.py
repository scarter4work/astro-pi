import os
import shutil

import numpy as np
import pytest
from astropy.io import fits
from astrometa import db, inventory


def _write_light(path, obj="M 42", ra=83.8, dec=-5.4, seed=1):
    rng = np.random.default_rng(seed)
    data = rng.normal(100, 5, (64, 64)).astype(np.float32)
    hdu = fits.PrimaryHDU(data)
    hdu.header["OBJECT"] = obj
    hdu.header["RA"] = ra
    hdu.header["DEC"] = dec
    hdu.header["INSTRUME"] = "ZWO ASI585MC Air"
    hdu.header["FILTER"] = "HaO3"
    hdu.header["EXPTIME"] = 120.0
    hdu.header["IMAGETYP"] = "Light Frame"
    hdu.writeto(path, overwrite=True)


def _write_light_with_nan_pixel(path, obj="M 42", seed=1):
    # A derived/stacked frame with a good header but a NaN edge pixel from
    # registration -- header metadata must survive even though pixel_stats
    # (Task 4) raises loudly on non-finite pixels.
    rng = np.random.default_rng(seed)
    data = rng.normal(100, 5, (64, 64)).astype(np.float32)
    data[0, 0] = np.nan
    hdu = fits.PrimaryHDU(data)
    hdu.header["OBJECT"] = obj
    hdu.header["INSTRUME"] = "ZWO ASI585MC Air"
    hdu.header["FILTER"] = "HaO3"
    hdu.header["EXPTIME"] = 120.0
    hdu.header["IMAGETYP"] = "Light Frame"
    hdu.writeto(path, overwrite=True)


def test_scan_records_frames(tmp_path):
    root = tmp_path / "astro_data" / "2026-09-08" / "M 42"
    root.mkdir(parents=True)
    _write_light(root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 1
    row = conn.execute("SELECT frame_type, camera, filter, fingerprint, "
                       "disposition FROM frames").fetchone()
    assert row[0] == "light" and row[1] == "ZWO ASI585MC Air"
    assert row[2] == "HaO3" and row[3] and row[4] == "present"


def test_scan_is_idempotent(tmp_path):
    root = tmp_path / "astro_data" / "d" ; root.mkdir(parents=True)
    _write_light(root / "Light_M 42_a_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    inventory.scan(conn, [tmp_path / "astro_data"])
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 1


def test_colliding_filenames_are_distinct_rows(tmp_path):
    a = tmp_path / "astro_data" / "n1"; a.mkdir(parents=True)
    b = tmp_path / "astro_data" / "n2"; b.mkdir(parents=True)
    _write_light(a / "Light_M 42_x_0001.fit", seed=1)
    _write_light(b / "Light_M 42_x_0001.fit", seed=2)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 2


def test_excluded_paths_are_skipped(tmp_path):
    q = tmp_path / "astro_data" / "_dedup_quarantine_2026-05-12"
    q.mkdir(parents=True)
    _write_light(q / "Light_M 42_x_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    assert inventory.scan(conn, [tmp_path / "astro_data"]).added == 0


def test_unreadable_file_is_recorded_not_skipped(tmp_path):
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    (root / "Light_broken_0001.fit").write_bytes(b"not a fits file at all")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    # A failed read is a diagnostic overlay, not disjoint from added/updated:
    # the frame still gets a row (added), and the failure is also counted.
    assert res.added == 1
    assert res.failed == 1
    err = conn.execute("SELECT read_error FROM frames").fetchone()[0]
    assert err is not None and err != ""


def test_zero_byte_file_is_recorded_not_skipped(tmp_path):
    # Two genuine zero-byte .fit files exist in the real archive.
    # content_hash succeeds (hash of empty input); read_header raises on
    # the truncated block; the row must still be written with the
    # failure recorded, not silently dropped from the scan.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    (root / "Light_empty_0001.fit").write_bytes(b"")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 1
    assert res.failed == 1
    row = conn.execute("SELECT size, read_error FROM frames").fetchone()
    assert row[0] == 0
    assert row[1] is not None and row[1] != ""


def test_failed_scan_is_idempotent_and_updates_on_rescan(tmp_path):
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    (root / "Light_broken_0001.fit").write_bytes(b"still not a fits file")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    first = inventory.scan(conn, [tmp_path / "astro_data"])
    second = inventory.scan(conn, [tmp_path / "astro_data"])
    assert first.added == 1 and first.failed == 1
    assert second.added == 0 and second.updated == 1 and second.failed == 1
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 1


def test_nan_pixels_dont_lose_header_metadata(tmp_path):
    # Header parses fine; pixel_stats raises on the NaN edge pixel. Per the
    # corrected design, the header read and the pixel read are independent:
    # losing pixel-derived fields must not also discard camera/filter/etc.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_light_with_nan_pixel(root / "pp_light_stack_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 1
    assert res.failed == 1
    row = conn.execute(
        "SELECT frame_type, camera, filter, fingerprint, bg_median, "
        "saturated_frac, read_error FROM frames").fetchone()
    frame_type, camera, filt, fingerprint, bg_median, sat_frac, err = row
    # IMAGETYP header wins over the filename in classify() -- proof the
    # header was actually parsed and passed through despite the pixel error.
    assert frame_type == "light"
    assert camera == "ZWO ASI585MC Air"
    assert filt == "HaO3"
    assert fingerprint is None
    assert bg_median is None
    assert sat_frac is None
    assert err is not None and err.startswith("pixels:")
    assert "header:" not in err


@pytest.mark.parametrize("suffix", [".fit", ".fits", ".FIT", ".FITS", ".Fits"])
def test_fit_and_fits_extensions_are_scanned_case_insensitively(tmp_path, suffix):
    # 6,290 real files in the archive carry .fits (a different rig's naming
    # convention) rather than .fit -- rglob("*.fit") alone silently drops
    # ~22% of the science data. Both extensions, any casing, must be seen.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    name = f"Light_M57_20250811_030222_0001_300.0s_Bin1{suffix}"
    _write_light(root / name)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 1


def test_xisf_extension_is_not_scanned(tmp_path):
    # .xisf is PixInsight's own format (processed intermediates, not subs)
    # and needs a different parser entirely -- explicitly out of scope.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    (root / "master_stack.xisf").write_bytes(b"not a real xisf but irrelevant")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 0
    assert res.failed == 0


def test_missing_root_raises_loudly_instead_of_scanning_empty(tmp_path):
    # A root that doesn't exist (e.g. an unmounted NAS bind mount) must not
    # be silently skipped: scan() returning cleanly with an empty
    # seen_hashes would make mark_missing() flip every known frame to
    # "missing", indistinguishable from the operator deleting the archive.
    missing = tmp_path / "does_not_exist"
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    with pytest.raises(FileNotFoundError, match=str(missing)):
        inventory.scan(conn, [missing])


def test_missing_root_raises_even_when_another_root_is_valid(tmp_path):
    ok = tmp_path / "astro_data" / "d"; ok.mkdir(parents=True)
    _write_light(ok / "Light_M 42_a_0001.fit")
    missing = tmp_path / "astro_data" / "does_not_exist"
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    with pytest.raises(FileNotFoundError):
        inventory.scan(conn, [ok, missing])


# --- FIX 1: the upsert must refresh every column a scan derives -------
#
# The original ON CONFLICT clause set only path/last_seen/disposition/
# read_error, so any row that reached the table by a route other than a
# successful scan -- a manifest restore, or a scan whose pixel read
# failed -- could never acquire its derived columns, no matter how many
# times the archive was rescanned.


def test_rescan_after_manifest_restore_repopulates_derived_columns(tmp_path):
    # Manifest restore is a first-class durability guarantee (spec 3, 10),
    # but a manifest carries no header_ra/focallen/naxis/bg_median and only
    # the fingerprint. Restoring into a fresh database and then rescanning
    # the real files must yield a store that can actually be clustered --
    # cluster.assign_fields requires header_ra, header_dec, focallen,
    # xpixsz, naxis1 and naxis2, none of which the old upsert would fill.
    from astrometa import cluster, manifest

    def _write_indexed(path, index):
        # Identical pixels (so both frames share a fingerprint and cluster
        # into one field) but distinct bytes, so they are two distinct
        # content_hash identities rather than one.
        rng = np.random.default_rng(1)
        hdu = fits.PrimaryHDU(rng.normal(100, 5, (64, 64)).astype(np.float32))
        hdu.header["OBJECT"] = "M 42"
        hdu.header["RA"] = 83.8
        hdu.header["DEC"] = -5.4
        hdu.header["INSTRUME"] = "ZWO ASI585MC Air"
        hdu.header["FILTER"] = "HaO3"
        hdu.header["EXPTIME"] = 120.0
        hdu.header["FOCALLEN"] = 491.0
        hdu.header["XPIXSZ"] = 2.9
        hdu.header["IMAGETYP"] = "Light Frame"
        hdu.header["FRAMENUM"] = index
        hdu.writeto(path, overwrite=True)

    leaf = tmp_path / "astro_data" / "2026-09-08" / "M 42"
    leaf.mkdir(parents=True)
    _write_indexed(leaf / "Light_M 42_a_0001.fit", 1)
    _write_indexed(leaf / "Light_M 42_a_0002.fit", 2)

    original = db.connect(tmp_path / "a.sqlite"); db.init_schema(original)
    inventory.scan(original, [tmp_path / "astro_data"])
    manifest_path = manifest.export_dir(original, leaf, out_dir=tmp_path / "m")

    # DB lost; rebuild from the sidecar, then rescan the real files.
    restored = db.connect(tmp_path / "b.sqlite"); db.init_schema(restored)
    assert manifest.import_file(restored, manifest_path) == 2
    assert restored.execute(
        "SELECT COUNT(*) FROM frames WHERE header_ra IS NOT NULL"
    ).fetchone()[0] == 0

    res = inventory.scan(restored, [tmp_path / "astro_data"])
    assert res.added == 0 and res.updated == 2

    row = restored.execute(
        "SELECT header_ra, header_dec, focallen, xpixsz, naxis1, naxis2, "
        "bg_median, saturated_frac, fingerprint FROM frames "
        "ORDER BY filename").fetchone()
    assert row[0] is not None and row[1] is not None
    assert row[2] == 491.0 and row[3] == 2.9
    assert row[4] == 64 and row[5] == 64
    assert row[6] is not None and row[7] is not None and row[8] is not None

    # The whole point: a restored store must be clusterable.
    assert cluster.assign_fields(restored) == 1
    assert restored.execute(
        "SELECT COUNT(*) FROM frames WHERE field_id IS NOT NULL"
    ).fetchone()[0] == 2


def test_successful_rescan_fills_columns_a_failed_pixel_read_left_null(tmp_path):
    # A transient pixel-read failure (a NAS hiccup) records read_error and
    # leaves fingerprint/bg_median/saturated_frac NULL. The next SUCCESSFUL
    # scan of the same bytes cleared read_error but never filled those
    # columns, so the frame dropped out of clustering (which requires
    # fingerprint IS NOT NULL) with nothing left on record to say why.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_light(root / "Light_M 42_a_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)

    real_getdata = inventory.fits.getdata

    def _transient(*a, **kw):
        raise OSError("transient read failure")

    inventory.fits.getdata = _transient
    try:
        first = inventory.scan(conn, [tmp_path / "astro_data"])
    finally:
        inventory.fits.getdata = real_getdata
    assert first.added == 1 and first.failed == 1
    assert conn.execute(
        "SELECT fingerprint FROM frames").fetchone()[0] is None

    second = inventory.scan(conn, [tmp_path / "astro_data"])
    assert second.updated == 1 and second.failed == 0
    fp, bg, sat, err = conn.execute(
        "SELECT fingerprint, bg_median, saturated_frac, read_error "
        "FROM frames").fetchone()
    assert err is None
    assert fp is not None and bg is not None and sat is not None


def test_pixel_read_failure_does_not_wipe_a_previously_good_fingerprint(tmp_path):
    # The converse of the above, and the reason the refreshed columns are
    # gated on which read actually succeeded rather than written blindly
    # from `excluded`: a later transient failure must not overwrite good
    # derived values with NULL. cluster._seed_reps only considers frames
    # with a non-null fingerprint as field representatives, so nulling one
    # would silently drop a field's representative and spawn duplicate
    # fields on the next clustering run.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_light(root / "Light_M 42_a_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    good_fp = conn.execute("SELECT fingerprint FROM frames").fetchone()[0]
    assert good_fp is not None

    real_getdata = inventory.fits.getdata

    def _transient(*a, **kw):
        raise OSError("transient read failure")

    inventory.fits.getdata = _transient
    try:
        inventory.scan(conn, [tmp_path / "astro_data"])
    finally:
        inventory.fits.getdata = real_getdata

    fp, err = conn.execute(
        "SELECT fingerprint, read_error FROM frames").fetchone()
    assert fp == good_fp
    assert err is not None and err.startswith("pixels:")


def test_first_seen_is_preserved_across_rescans(tmp_path):
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_light(root / "Light_M 42_a_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    first_seen = conn.execute("SELECT first_seen FROM frames").fetchone()[0]
    inventory.scan(conn, [tmp_path / "astro_data"])
    row = conn.execute("SELECT first_seen, last_seen FROM frames").fetchone()
    assert row[0] == first_seen
    assert row[1] >= first_seen


# --- FIX 3: persist the OBJECT card and the leaf directory name -------


def test_scan_persists_object_card_and_leaf_dir(tmp_path):
    # inventory parsed OBJECT and threw it away, and `frames` had nowhere
    # to put it. Spec 6.7 needs both the OBJECT card and the directory
    # name later to cross-check a solved identity; recovering them after
    # the fact costs a fresh 34,000-file header re-read.
    leaf = tmp_path / "astro_data" / "2026-09-08" / "IC 1848"
    leaf.mkdir(parents=True)
    _write_light(leaf / "Light_IC 1848_1-1_120.0s_HaO3_20260908-220000_0deg_0001.fit",
                 obj="IC 1848")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    row = conn.execute("SELECT object_card, leaf_dir FROM frames").fetchone()
    assert row == ("IC 1848", "IC 1848")


def test_object_card_absent_is_null_but_leaf_dir_still_recorded(tmp_path):
    # 30 real directories are named only `Lights`, and many frames carry
    # no OBJECT card at all. The directory name is still worth having.
    import numpy as np
    from astropy.io import fits as _fits

    leaf = tmp_path / "astro_data" / "2026-09-08" / "Lights"
    leaf.mkdir(parents=True)
    rng = np.random.default_rng(4)
    hdu = _fits.PrimaryHDU(rng.normal(100, 5, (32, 32)).astype(np.float32))
    hdu.header["IMAGETYP"] = "Light Frame"
    hdu.writeto(leaf / "Light_x_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    row = conn.execute("SELECT object_card, leaf_dir FROM frames").fetchone()
    assert row == (None, "Lights")


def test_object_card_and_leaf_dir_refresh_on_rescan(tmp_path):
    leaf = tmp_path / "astro_data" / "2026-09-08" / "M 42"
    leaf.mkdir(parents=True)
    _write_light(leaf / "Light_M 42_a_0001.fit", obj="M 42")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    conn.execute("UPDATE frames SET object_card=NULL, leaf_dir=NULL")
    conn.commit()
    inventory.scan(conn, [tmp_path / "astro_data"])
    row = conn.execute("SELECT object_card, leaf_dir FROM frames").fetchone()
    assert row == ("M 42", "M 42")


# --- FIX 5: numeric header values must be coerced at inventory time ---


def _write_with_cards(path, extra: dict, seed=7):
    rng = np.random.default_rng(seed)
    hdu = fits.PrimaryHDU(rng.normal(100, 5, (64, 64)).astype(np.float32))
    hdu.header["INSTRUME"] = "ZWO ASI585MC Air"
    hdu.header["FILTER"] = "HaO3"
    hdu.header["IMAGETYP"] = "Light Frame"
    for k, v in extra.items():
        hdu.header[k] = v
    hdu.writeto(path, overwrite=True)


def test_sexagesimal_ra_is_not_stored_as_text_in_a_real_column(tmp_path):
    # fitsheader returns whatever a card parses to, and SQLite's REAL
    # affinity stores an unparseable string as TEXT rather than
    # rejecting it. That text only surfaces much later, as a TypeError
    # inside cluster.assign_fields. It must be caught and recorded here.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(root / "Light_x_0001.fit",
                      {"RA": "02 51 27.0", "DEC": 60.07})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.added == 1
    assert res.failed == 1
    ra, kind, err = conn.execute(
        "SELECT header_ra, typeof(header_ra), read_error FROM frames"
    ).fetchone()
    assert ra is None
    assert kind == "null"
    assert err is not None and "RA" in err


def test_uncoercible_value_is_loud_but_the_rest_of_the_header_survives(tmp_path):
    # An unusable RA must not cost the frame its camera, filter or
    # fingerprint -- the header and pixel reads both succeeded.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(root / "Light_x_0001.fit",
                      {"RA": "02 51 27.0", "DEC": 60.07, "EXPTIME": 120.0})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    camera, filt, exptime, dec, fp = conn.execute(
        "SELECT camera, filter, exptime, header_dec, fingerprint "
        "FROM frames").fetchone()
    assert camera == "ZWO ASI585MC Air"
    assert filt == "HaO3"
    assert exptime == 120.0
    assert dec == 60.07
    assert fp is not None


def test_numeric_headers_are_stored_with_the_right_sqlite_type(tmp_path):
    # This archive spans seven cameras and three naming eras; an integer
    # EXPTIME or a float-formatted NAXIS must land as REAL/INTEGER, not
    # as whatever the card happened to parse to.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(root / "Light_x_0001.fit",
                      {"RA": 42.86, "DEC": 60.07, "EXPTIME": 120,
                       "FOCALLEN": 491, "XPIXSZ": 2.9})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"])
    assert res.failed == 0
    types = conn.execute(
        "SELECT typeof(header_ra), typeof(exptime), typeof(focallen), "
        "typeof(naxis1) FROM frames").fetchone()
    assert types == ("real", "real", "real", "integer")


# --- FIX 8: captured_at falls back to the filename capture instant ----


def test_captured_at_falls_back_to_the_filename_capture_instant(tmp_path):
    # Spec 7: captured_at is "DATE-OBS, falling back to the filename
    # capture instant". Without the fallback a frame whose header lacks
    # DATE-OBS has no capture instant on record at all.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(
        root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit", {})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    assert conn.execute("SELECT captured_at FROM frames").fetchone()[0] \
        == "2026-09-08T22:00:00"


def test_date_obs_wins_over_the_filename_fallback(tmp_path):
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(
        root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit",
        {"DATE-OBS": "2026-09-09T03:00:00.500"})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    assert conn.execute("SELECT captured_at FROM frames").fetchone()[0] \
        == "2026-09-09T03:00:00.500"


def test_captured_at_is_null_when_neither_source_has_one(tmp_path):
    # ASIAIR autosave files carry no capture stamp in either place.
    # NULL is the honest answer; nothing is invented.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(root / "Autosave001.fit", {})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    assert conn.execute("SELECT captured_at FROM frames").fetchone()[0] is None


# --- Corrections to FIX 1/8: two regressions in the same update clause


def _failing_header_read(monkeypatch):
    """Make the next scan's header read fail while the pixel read still
    succeeds -- a transient header failure on bytes that parsed fine
    before."""
    def _boom(path):
        raise inventory.fitsheader.FitsHeaderError("transient header failure")
    monkeypatch.setattr(inventory.fitsheader, "read_header", _boom)


def test_transient_header_failure_does_not_reclassify_a_light_frame(tmp_path):
    # frame_type was the one header-derived column left updating
    # unconditionally. classify() falls back to the filename when the
    # header is empty, so a frame with IMAGETYP='Light Frame' but an
    # Autosave* name scans as 'light' and then gets REWRITTEN to
    # 'derived' by a rescan whose header read failed -- dropping it out
    # of cluster.assign_fields and build_projects, both of which filter
    # frame_type='light'. That is the silent-drop class FIX 1 exists to
    # close, reintroduced by a column FIX 1 newly added to the clause.
    import pytest as _pytest
    from astrometa import cluster

    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(root / "Autosave001.fit",
                      {"IMAGETYP": "Light Frame", "RA": 83.8, "DEC": -5.4,
                       "FOCALLEN": 491.0, "XPIXSZ": 2.9})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    assert conn.execute("SELECT frame_type FROM frames").fetchone()[0] == "light"

    mp = _pytest.MonkeyPatch()
    try:
        _failing_header_read(mp)
        res = inventory.scan(conn, [tmp_path / "astro_data"])
    finally:
        mp.undo()
    assert res.failed == 1

    assert conn.execute("SELECT frame_type FROM frames").fetchone()[0] == "light"
    # The concrete consequence: the frame is still clusterable.
    assert cluster.assign_fields(conn) == 1


def test_a_readable_header_still_reclassifies_a_renamed_frame(tmp_path):
    # The gate must not freeze frame_type forever: when the header IS
    # readable it remains authoritative, so a corrected IMAGETYP still
    # takes effect on rescan.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(root / "Light_x_0001.fit", {"IMAGETYP": "Light Frame"})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    conn.execute("UPDATE frames SET frame_type='unknown'")
    conn.commit()
    inventory.scan(conn, [tmp_path / "astro_data"])
    assert conn.execute("SELECT frame_type FROM frames").fetchone()[0] == "light"


def test_filename_fallback_fills_captured_at_when_the_header_read_fails(tmp_path):
    # captured_at was gated on :header_ok, so FIX 8's filename fallback
    # applied on first insert but never on a rescan whose header read
    # failed -- the one case it was added for. A row restored from a
    # manifest that carried no captured_at could therefore never acquire
    # one from its own filename.
    import pytest as _pytest

    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(
        root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit", {})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])
    # A manifest-restored row that carried no capture instant.
    conn.execute("UPDATE frames SET captured_at=NULL")
    conn.commit()

    mp = _pytest.MonkeyPatch()
    try:
        _failing_header_read(mp)
        inventory.scan(conn, [tmp_path / "astro_data"])
    finally:
        mp.undo()

    assert conn.execute("SELECT captured_at FROM frames").fetchone()[0] \
        == "2026-09-08T22:00:00"


def test_a_recorded_date_obs_is_not_clobbered_by_the_filename_fallback(tmp_path):
    # DATE-OBS is the better source (UTC, sub-second); the filename token
    # is local wall-clock. A later header failure must not downgrade a
    # captured_at that was read from DATE-OBS.
    import pytest as _pytest

    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_with_cards(
        root / "Light_M 42_120.0s_Bin1_HaO3_20260908-220000_0deg_0001.fit",
        {"DATE-OBS": "2026-09-09T03:00:00.500"})
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"])

    mp = _pytest.MonkeyPatch()
    try:
        _failing_header_read(mp)
        inventory.scan(conn, [tmp_path / "astro_data"])
    finally:
        mp.undo()

    assert conn.execute("SELECT captured_at FROM frames").fetchone()[0] \
        == "2026-09-09T03:00:00.500"


# ---------------------------------------------------------------------------
# Parallel inventory
#
# The per-frame work (content_hash over the whole file, the header read,
# the pixel decode, fingerprint, pixel stats) is farmed out to worker
# processes while every database write stays in the parent. These tests
# pin the two properties that make that safe: the result must not depend
# on how many workers ran, and a worker must never be able to make a
# frame quietly disappear.
# ---------------------------------------------------------------------------

_VOLATILE_COLUMNS = {"first_seen", "last_seen"}


def _all_rows(conn):
    """Every frames column except the two wall-clock stamps."""
    cols = [r[1] for r in conn.execute("PRAGMA table_info(frames)")]
    keep = [c for c in cols if c not in _VOLATILE_COLUMNS]
    return keep, conn.execute(
        f"SELECT {', '.join(keep)} FROM frames ORDER BY content_hash"
    ).fetchall()


def _mixed_archive(tmp_path):
    """
    A tree big enough to occupy several workers, holding every per-frame
    outcome the scan has to survive: clean frames, a header-read failure,
    a zero-byte file, a NaN-pixel frame whose header is fine, and a
    duplicate pair (same bytes, two paths) that collide on one row.
    """
    root = tmp_path / "astro_data"
    for night in ("2026-09-07", "2026-09-08"):
        d = root / night / "M 42"
        d.mkdir(parents=True)
        for i in range(16):
            _write_light(
                d / f"Light_M 42_120.0s_Bin1_HaO3_2026090{night[-1]}-22{i:02d}00"
                    f"_0deg_{i:04d}.fit",
                seed=hash(night) % 1000 + i)
    bad = root / "2026-09-08" / "M 42"
    (bad / "Light_broken_0001.fit").write_bytes(b"not a fits file at all")
    (bad / "Light_empty_0001.fit").write_bytes(b"")
    _write_light_with_nan_pixel(bad / "Light_nan_0001.fit", seed=99)

    # Genuine duplicate: identical bytes under two paths. Both hash to
    # one row, so which path lands in it is decided by which is
    # processed LAST -- the single place worker scheduling could leak
    # into the result if results were consumed out of walk order.
    src = root / "2026-09-07" / "M 42" / "Light_dup_0001.fit"
    _write_light(src, seed=4242)
    shutil.copy2(src, root / "2026-09-08" / "M 42" / "Light_dup_0001.fit")
    return root


def test_parallel_scan_matches_single_worker_scan_exactly(tmp_path):
    # Requirement: a scan is deterministic in its worker count. Anything
    # else would mean the archive's recorded state depends on how busy
    # the machine was, which is not a property you can build a
    # cull-tracking store on.
    root = _mixed_archive(tmp_path)

    serial_conn = db.connect(tmp_path / "serial.sqlite")
    db.init_schema(serial_conn)
    serial = inventory.scan(serial_conn, [root], workers=1)

    parallel_conn = db.connect(tmp_path / "parallel.sqlite")
    db.init_schema(parallel_conn)
    parallel = inventory.scan(parallel_conn, [root], workers=16)

    assert (serial.added, serial.updated, serial.failed) == \
           (parallel.added, parallel.updated, parallel.failed)
    assert serial.seen_hashes == parallel.seen_hashes
    assert serial.failed >= 3            # broken + empty + NaN pixels
    assert serial.added >= 30            # the pool really was exercised

    serial_cols, serial_rows = _all_rows(serial_conn)
    parallel_cols, parallel_rows = _all_rows(parallel_conn)
    assert serial_cols == parallel_cols
    assert serial_rows == parallel_rows


def test_parallel_rescan_is_idempotent(tmp_path):
    # Two parallel scans must land on the same state as one, including
    # the added/updated split -- a frame seen again is an update, never a
    # second row, no matter which worker read it.
    root = _mixed_archive(tmp_path)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    first = inventory.scan(conn, [root], workers=8)
    _, after_first = _all_rows(conn)
    first_stamps = dict(conn.execute(
        "SELECT content_hash, first_seen FROM frames").fetchall())
    second = inventory.scan(conn, [root], workers=8)
    _, after_second = _all_rows(conn)

    # Every walked file is an update the second time round. That is more
    # than first.added: the duplicate pair walks twice but adds one row,
    # so the first scan already counted one of the two as an update.
    assert second.added == 0
    assert second.updated == first.added + first.updated
    assert second.failed == first.failed
    assert after_first == after_second

    # first_seen records when a content_hash was FIRST inventoried and
    # is deliberately absent from the update clause. It is excluded from
    # the row comparison above (it is a wall-clock stamp), so pin it
    # here on the parallel path explicitly.
    stamps = conn.execute(
        "SELECT content_hash, first_seen, last_seen FROM frames").fetchall()
    assert stamps
    for chash, first_seen, last_seen in stamps:
        assert first_seen == first_stamps[chash]
        assert last_seen >= first_seen


def test_parallel_pixel_failure_still_records_header_fields(tmp_path):
    # The two independent try blocks have to survive the process
    # boundary: a pixel read that raises in a WORKER must still come back
    # carrying the header fields, with only the `pixels:` half of the
    # error recorded. Deliberately run through the pool, not serially.
    root = tmp_path / "astro_data" / "d"
    root.mkdir(parents=True)
    for i in range(24):
        _write_light(root / f"Light_M 42_ok_{i:04d}.fit", seed=i)
    _write_light_with_nan_pixel(root / "Light_M 42_nan_0001.fit", seed=7)

    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"], workers=8)

    assert res.added == 25
    assert res.failed == 1
    row = conn.execute(
        "SELECT camera, filter, exptime, object_card, frame_type, "
        "fingerprint, bg_median, read_error FROM frames "
        "WHERE filename='Light_M 42_nan_0001.fit'").fetchone()
    assert row[0] == "ZWO ASI585MC Air"      # header survived the failure
    assert row[1] == "HaO3"
    assert row[2] == 120.0
    assert row[3] == "M 42"
    assert row[4] == "light"
    assert row[5] is None and row[6] is None  # pixel-derived, correctly absent
    assert row[7] is not None and row[7].startswith("pixels: ")
    assert "header: " not in row[7]


def test_parallel_header_failure_still_records_pixel_fields(tmp_path):
    # The mirror image, also through the pool: an unreadable header must
    # not discard the fingerprint and pixel stats the same file's pixel
    # read recovered.
    root = tmp_path / "astro_data" / "d"
    root.mkdir(parents=True)
    for i in range(24):
        _write_light(root / f"Light_M 42_ok_{i:04d}.fit", seed=i)
    (root / "Light_broken_0001.fit").write_bytes(b"not a fits file at all")

    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    res = inventory.scan(conn, [tmp_path / "astro_data"], workers=8)

    assert res.added == 25 and res.failed == 1
    err = conn.execute(
        "SELECT read_error FROM frames "
        "WHERE filename='Light_broken_0001.fit'").fetchone()[0]
    assert err.startswith("header: ")
    assert "pixels: " in err          # both halves recorded, both prefixed


@pytest.mark.skipif(os.geteuid() == 0,
                    reason="root ignores the mode bits this test relies on")
def test_worker_failure_aborts_loudly_instead_of_dropping_the_frame(tmp_path):
    # A failure OUTSIDE the two read-error try blocks -- here an
    # unreadable file, so content_hash's open() raises -- aborted the
    # serial scan and must still abort the parallel one. The one outcome
    # that would be unacceptable is the frame silently vanishing from
    # the inventory, because "absent" is the signal this whole store
    # exists to make trustworthy.
    root = tmp_path / "astro_data" / "d"
    root.mkdir(parents=True)
    for i in range(24):
        _write_light(root / f"Light_M 42_ok_{i:04d}.fit", seed=i)
    locked = root / "Light_M 42_locked_0001.fit"
    _write_light(locked, seed=500)
    locked.chmod(0o000)

    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    try:
        with pytest.raises(inventory.InventoryWorkerError) as exc:
            inventory.scan(conn, [tmp_path / "astro_data"], workers=8)
    finally:
        locked.chmod(0o644)
    assert "Light_M 42_locked_0001.fit" in str(exc.value)
    assert isinstance(exc.value.__cause__, PermissionError)
    # Nothing committed: the abort happens before scan()'s commit.
    assert conn.execute("SELECT COUNT(*) FROM frames").fetchone()[0] == 0


def test_default_workers_is_derived_from_the_cpu_count(tmp_path):
    n = inventory.default_workers()
    assert 1 <= n <= (os.process_cpu_count() or 1)


def test_zero_workers_is_rejected_rather_than_silently_corrected(tmp_path):
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    _write_light(root / "Light_M 42_x_0001.fit")
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    with pytest.raises(ValueError, match="at least 1"):
        inventory.scan(conn, [tmp_path / "astro_data"], workers=0)


def test_a_handful_of_frames_never_starts_a_pool(tmp_path):
    # Worker count is damped by how much work there is: standing up a
    # spawn pool costs far more than reading three frames. Proven by
    # sabotaging the parallel path -- if it were taken, the scan would
    # raise rather than quietly succeeding.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    for i in range(3):
        _write_light(root / f"Light_M 42_x_{i:04d}.fit", seed=i)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)

    mp = pytest.MonkeyPatch()
    try:
        mp.setattr(inventory, "_scan_parallel", _never_called)
        res = inventory.scan(conn, [tmp_path / "astro_data"], workers=32)
    finally:
        mp.undo()
    assert res.added == 3


def _never_called(*args, **kwargs):
    raise AssertionError("a pool was started for a handful of frames")


def test_missing_root_still_raises_on_the_parallel_path(tmp_path):
    # The loud missing-root failure is not allowed to soften just
    # because the walk is now consumed lazily by a pool.
    good = tmp_path / "astro_data"
    d = good / "d"; d.mkdir(parents=True)
    for i in range(24):
        _write_light(d / f"Light_M 42_x_{i:04d}.fit", seed=i)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    with pytest.raises(FileNotFoundError):
        inventory.scan(conn, [good, tmp_path / "not_mounted"], workers=8)


def test_the_pool_is_actually_used_and_runs_out_of_process(tmp_path):
    # The determinism tests above would still pass if the parallel path
    # quietly degraded to a serial loop, so prove both halves directly:
    # that _scan_parallel is entered with more than one worker, and that
    # the per-frame work genuinely leaves this process.
    #
    # The out-of-process proof is the monkeypatch: it sabotages
    # _read_pixels in the PARENT only. A spawn-based worker re-imports
    # astrometa.inventory clean, so it never sees the patch -- if the
    # frames come back with fingerprints and no read_error, the reads
    # cannot have happened here.
    root = tmp_path / "astro_data" / "d"; root.mkdir(parents=True)
    for i in range(24):
        _write_light(root / f"Light_M 42_x_{i:04d}.fit", seed=i)
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)

    real_parallel = inventory._scan_parallel
    worker_counts = []

    def spy(conn_, res, frames, read_pixels, workers):
        worker_counts.append(workers)
        return real_parallel(conn_, res, frames, read_pixels, workers)

    mp = pytest.MonkeyPatch()
    try:
        mp.setattr(inventory, "_scan_parallel", spy)
        mp.setattr(inventory, "_read_pixels", _parent_must_not_read_pixels)
        res = inventory.scan(conn, [tmp_path / "astro_data"], workers=8)
    finally:
        mp.undo()

    assert worker_counts and worker_counts[0] > 1
    assert res.added == 24 and res.failed == 0
    n_fingerprinted = conn.execute(
        "SELECT COUNT(*) FROM frames "
        "WHERE fingerprint IS NOT NULL AND read_error IS NULL").fetchone()[0]
    assert n_fingerprinted == 24


def _parent_must_not_read_pixels(path):
    raise AssertionError(
        f"per-frame pixel read ran in the parent process for {path}")


def test_quarantine_survives_a_parallel_rescan(tmp_path):
    # The ON CONFLICT clause's `CASE WHEN disposition='quarantined'`
    # guard runs in the parent and is untouched by this change -- but it
    # is the guard that keeps a cull from being erased by the next walk,
    # so pin it on the parallel path too rather than only on the serial
    # one (test_disposition.test_quarantine_survives_a_rescan).
    from astrometa import disposition

    root = tmp_path / "astro_data" / "IC 59"
    root.mkdir(parents=True)
    for i in range(24):
        _write_light(root / f"Light_IC 59_x_{i:04d}.fit", seed=i)
    culled = root / "Light_IC 59_culled_0001.fit"
    _write_light(culled, seed=777)

    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    inventory.scan(conn, [tmp_path / "astro_data"], workers=8)
    chash = conn.execute(
        "SELECT content_hash FROM frames WHERE filename=?",
        (culled.name,)).fetchone()[0]

    dest = disposition.quarantine(conn, chash, "hfd 6.2 > 5.1", dry_run=False)
    assert dest is not None and dest.is_relative_to(tmp_path / "astro_data")

    inventory.scan(conn, [tmp_path / "astro_data"], workers=8)

    row = conn.execute("SELECT disposition, disposition_reason, path "
                       "FROM frames WHERE content_hash=?", (chash,)).fetchone()
    assert row[0] == "quarantined"
    assert row[1] == "hfd 6.2 > 5.1"
    assert row[2] == str(dest)
    # Every other frame is still plainly present.
    assert conn.execute(
        "SELECT COUNT(*) FROM frames WHERE disposition='present'"
    ).fetchone()[0] == 24
