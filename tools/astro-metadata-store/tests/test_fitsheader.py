import pytest
from astrometa import fitsheader


def test_parses_string_number_and_logical(make_fits):
    p = make_fits("a.fit", {
        "SIMPLE": "                   T",
        "OBJECT": "'IC 1848_1-1'",
        "EXPTIME": "            120.0",
        "NAXIS1": "             3840",
    })
    h = fitsheader.read_header(p)
    assert h["OBJECT"] == "IC 1848_1-1"
    assert h["EXPTIME"] == 120.0
    assert h["NAXIS1"] == 3840
    assert h["SIMPLE"] is True


def test_object_value_preserves_internal_spaces(make_fits):
    p = make_fits("b.fit", {"OBJECT": "'M 20'"})
    assert fitsheader.read_header(p)["OBJECT"] == "M 20"


def test_slash_inside_quoted_string_is_not_a_comment(make_fits):
    p = make_fits("c.fit", {"FILTER": "'Ha/O3'  / dual band"})
    assert fitsheader.read_header(p)["FILTER"] == "Ha/O3"


def test_stops_at_END_and_does_not_read_pixels(make_fits):
    p = make_fits("d.fit", {"OBJECT": "'X'"}, pixels=b"\xff" * 100000)
    h = fitsheader.read_header(p)
    assert h["OBJECT"] == "X"
    assert "\xff" not in str(h)


def test_missing_END_raises(tmp_path):
    p = tmp_path / "bad.fit"
    p.write_bytes(b" " * 2880)
    with pytest.raises(fitsheader.FitsHeaderError):
        fitsheader.read_header(p)


def test_escaped_quotes_in_string_value(make_fits):
    """FITS standard: '' inside a quoted string means one literal '"""
    p = make_fits("e.fit", {"OBJECT": "'Barnard''s Loop'"})
    assert fitsheader.read_header(p)["OBJECT"] == "Barnard's Loop"


def test_non_ascii_byte_raises(tmp_path):
    """Non-ASCII bytes in header cards must raise loudly, not be silently replaced"""
    from tests.conftest import build_fits_header
    p = tmp_path / "bad_ascii.fit"
    # Create a valid header with one card having a non-ASCII byte
    cards = {"OBJECT": "'Test'"}
    header = build_fits_header(cards)
    # Replace a byte in the first card with non-ASCII (0xff)
    header_bytes = bytearray(header)
    header_bytes[50] = 0xff
    p.write_bytes(bytes(header_bytes))
    with pytest.raises(fitsheader.FitsHeaderError, match="non-ASCII"):
        fitsheader.read_header(p)
