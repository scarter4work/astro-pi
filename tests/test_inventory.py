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
