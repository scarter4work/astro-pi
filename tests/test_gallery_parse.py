"""Pure parsers for Djangoplicity gallery pages (§5.2.1).

Values are measured from the real pages — see the plan's Verified Ground Truth table.
"""

from __future__ import annotations

import pytest

from autocontrast.db.discover.gallery import (
    GALLERIES,
    fov_radius_arcmin,
    labelled_value,
    parse_dec_sexagesimal,
    parse_fov_arcmin,
    parse_ra_sexagesimal,
    parse_release_date,
    search_url,
    text_lines,
)


def test_both_galleries_configured():
    assert set(GALLERIES) == {"esa_hubble", "eso"}
    assert GALLERIES["esa_hubble"].results_paginated is True
    assert GALLERIES["eso"].results_paginated is False


def test_search_url_paginates_hubble_but_not_eso():
    """Confirmed live: Hubble serves results at /page/N/, ESO only at the bare path."""
    hub = search_url(GALLERIES["esa_hubble"], {"subject_name": "Orion Nebula"}, page=2)
    assert "/images/archive/search/page/2/" in hub
    assert "subject_name=Orion+Nebula" in hub

    eso = search_url(GALLERIES["eso"], {"subject_name": "Orion Nebula"}, page=2)
    assert "/page/" not in eso
    assert eso.endswith("subject_name=Orion+Nebula")


def test_text_lines_drops_scripts_and_unescapes_entities():
    doc = "<div>Name:</div><script>var x='Nope';</script><p>M&nbsp;42 &amp; friends</p>"
    lines = text_lines(doc)
    assert "Nope" not in " ".join(lines)
    assert any("friends" in line for line in lines)


def test_labelled_value_returns_following_line_ignoring_colon_and_case():
    lines = ["Position (RA):", "5 35 9.73", "Position (Dec):", "-5° 24' 50.32\""]
    assert labelled_value(lines, "Position (RA)") == "5 35 9.73"
    assert labelled_value(lines, "position (dec)") == "-5° 24' 50.32\""
    assert labelled_value(lines, "Field of view") is None


def test_labelled_value_at_end_of_document_returns_none():
    assert labelled_value(["Field of view:"], "Field of view") is None


@pytest.mark.parametrize(
    "text, expected",
    [("5 35 9.73", 83.79054), ("5 35 17.32", 83.82217), ("0 0 0", 0.0), ("12 0 0", 180.0)],
)
def test_parse_ra_sexagesimal(text, expected):
    assert parse_ra_sexagesimal(text) == pytest.approx(expected, abs=1e-4)


@pytest.mark.parametrize(
    "text, expected",
    [
        ("-5° 24' 50.32\"", -5.41398),   # heic0601a
        ("-5° 23' 27.55\"", -5.39099),   # eso1103a
        ("+22° 0' 52.20\"", 22.01450),
        ("22° 0' 52.20\"", 22.01450),
    ],
)
def test_parse_dec_sexagesimal(text, expected):
    assert parse_dec_sexagesimal(text) == pytest.approx(expected, abs=1e-4)


def test_parse_dec_keeps_the_sign_when_degrees_are_zero():
    """-0° 30' 0" is a southern position; losing the sign flips the hemisphere."""
    assert parse_dec_sexagesimal("-0° 30' 0\"") == pytest.approx(-0.5, abs=1e-6)


def test_unparseable_coordinates_return_none_rather_than_guessing():
    assert parse_ra_sexagesimal("unknown") is None
    assert parse_dec_sexagesimal("") is None


@pytest.mark.parametrize(
    "text, expected",
    [
        ("30.03 x 30.03 arcminutes", (30.03, 30.03)),
        ("35.49 x 34.10 arcminutes", (35.49, 34.10)),
        ("2.5 × 2.5 arcminutes", (2.5, 2.5)),          # unicode multiplication sign
        ("120 x 90 arcseconds", (2.0, 1.5)),           # arcsec -> arcmin
        ("1.5 x 1.0 degrees", (90.0, 60.0)),           # deg -> arcmin
    ],
)
def test_parse_fov_arcmin_normalizes_units(text, expected):
    got = parse_fov_arcmin(text)
    assert got == pytest.approx(expected, rel=1e-6)


def test_parse_fov_rejects_unknown_units_rather_than_assuming_arcmin():
    """A silent unit assumption corrupts pixel scale, and hence band-limiting (§4.4)."""
    assert parse_fov_arcmin("30 x 30 parsecs") is None
    assert parse_fov_arcmin("not a field of view") is None


def test_fov_radius_is_the_half_diagonal():
    assert fov_radius_arcmin(30.03, 30.03) == pytest.approx(21.23, abs=0.01)
    assert fov_radius_arcmin(35.49, 34.10) == pytest.approx(24.61, abs=0.01)


def test_parse_release_date_is_locale_independent():
    """%B depends on locale, so month names are mapped explicitly."""
    assert parse_release_date("11 January 2006, 16:00") == "2006-01-11T16:00:00Z"
    assert parse_release_date("19 January 2011, 12:00") == "2011-01-19T12:00:00Z"
    assert parse_release_date("3 December 1999, 09:30") == "1999-12-03T09:30:00Z"
    assert parse_release_date("garbage") is None
