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
