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
