"""Detail-page parsing against committed real pages (§5.2.1).

Every expectation here is measured ground truth from data/seed_catalog.json or from the
pages themselves. Fixture choice is deliberate: alongside two clean observations there is
a render with no published position (opo0205c) and an artist's impression (heic0211i).
Commit 12e7041 shipped two bugs past 138 green tests because every fixture was a 2D mono
FITS — the gap was fixture diversity, not coverage. Hence the awkward cases are here from
the start.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from autocontrast.db.discover.gallery import (
    GALLERIES,
    asserts_copyright,
    parse_credit,
    parse_detail,
    parse_filter_bands,
    parse_size_px,
    text_lines,
)

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


def load(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8", errors="replace")


@pytest.fixture
def heic0601a():
    return parse_detail(
        load("hubble_detail_heic0601a.html"),
        entry_id="heic0601a", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/heic0601a/",
        width_px=18000, height_px=18000,
    )


@pytest.fixture
def eso1103a():
    return parse_detail(
        load("eso_detail_eso1103a.html"),
        entry_id="eso1103a", gallery="eso",
        detail_url="https://www.eso.org/public/images/eso1103a/",
        width_px=8948, height_px=8597,
    )


def test_hubble_position_matches_the_curated_catalog(heic0601a):
    """seed_catalog.json declares 83.7905 / -5.4140 from the embedded AVM tag; the
    published text must agree, since that is what makes pre-download filtering sound."""
    assert heic0601a.ra_deg == pytest.approx(83.7905, abs=1e-3)
    assert heic0601a.dec_deg == pytest.approx(-5.4140, abs=1e-3)


def test_eso_position_matches_the_curated_catalog(eso1103a):
    assert eso1103a.ra_deg == pytest.approx(83.82217, abs=1e-4)
    assert eso1103a.dec_deg == pytest.approx(-5.39099, abs=1e-4)


def test_footprint_radii_match_the_curated_catalog(heic0601a, eso1103a):
    assert heic0601a.fov_radius_arcmin == pytest.approx(21.23, abs=0.02)
    assert eso1103a.fov_radius_arcmin == pytest.approx(24.61, abs=0.02)


def test_pixel_scale_is_derivable_from_published_metadata(heic0601a, eso1103a):
    """fov_w * 60 / width_px. Catalog says 0.1001 and 0.238 arcsec/px. This is what lets
    gate G4 be satisfied without downloading a 324-megapixel JPEG."""
    assert heic0601a.pixel_scale_arcsec == pytest.approx(0.1001, abs=1e-4)
    assert eso1103a.pixel_scale_arcsec == pytest.approx(0.238, abs=1e-3)


def test_image_urls_point_at_the_cdn(heic0601a, eso1103a):
    assert heic0601a.image_url == \
        "https://cdn.esahubble.org/archives/images/large/heic0601a.jpg"
    assert eso1103a.image_url == "https://cdn.eso.org/images/large/eso1103a.jpg"


def test_names_types_and_release_dates(heic0601a, eso1103a):
    assert heic0601a.object_name == "Messier 42"
    assert eso1103a.object_name == "M 42"          # the naming swamp §5.1 warns about
    assert heic0601a.entry_type == "Observation"
    assert heic0601a.published_utc == "2006-01-11T16:00:00Z"
    assert eso1103a.published_utc == "2011-01-19T12:00:00Z"


def test_palette_is_derived_from_published_filters(heic0601a, eso1103a):
    """Both are broadband-dominated composites; the catalog declares RGB for both."""
    assert heic0601a.palette_class == "RGB"
    assert eso1103a.palette_class == "RGB"


def test_filter_bands_are_extracted(heic0601a):
    bands = parse_filter_bands(load("hubble_detail_heic0601a.html"))
    assert bands == ["B", "V", "H-alpha", "I", "Z"]


def test_size_is_published_as_a_cross_check_on_listing_dimensions():
    lines = text_lines(load("hubble_detail_heic0601a.html"))
    assert parse_size_px(lines) == (18000, 18000)


def test_attribution_is_the_full_credit_line(heic0601a):
    """The site states crediting with the FULL credit line is mandatory, so the whole
    string is captured, not the first fragment before a nested tag."""
    assert "NASA" in heic0601a.attribution
    assert "ESA" in heic0601a.attribution
    assert "Robberto" in heic0601a.attribution
    assert "Orion Treasury Project Team" in heic0601a.attribution


def test_license_defaults_to_the_gallery_license_when_no_copyright_is_asserted(heic0601a):
    assert heic0601a.license == "CC BY 4.0"
    assert GALLERIES["esa_hubble"].default_license == "CC BY 4.0"


def test_render_with_no_published_position_yields_nulls_not_a_guess():
    """opo0205c: ESA/Hubble publishes neither coordinates nor field of view. It must
    become a NULL row so the crawler remembers it, never a guessed position — a wrong
    pixel scale corrupts band-limiting (§4.4)."""
    entry = parse_detail(
        load("hubble_detail_opo0205c.html"),
        entry_id="opo0205c", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/opo0205c/",
    )
    assert entry.ra_deg is None
    assert entry.dec_deg is None
    assert entry.fov_radius_arcmin is None
    assert entry.pixel_scale_arcsec is None
    assert entry.entry_type == "Photographic"
    assert entry.parsed_ok is True     # the page parsed; the data is simply absent


def test_copyrighted_render_is_flagged_so_it_is_never_ingested_as_cc_by():
    """opo0205c's credit asserts AAO copyright. ESA/Hubble hosts third-party
    copyrighted images, so the per-gallery CC BY 4.0 default is NOT universal. Claiming
    CC BY 4.0 here would attach a false license to every fingerprint record (§5.5)."""
    credit = parse_credit(load("hubble_detail_opo0205c.html"))
    assert asserts_copyright(credit) is True
    entry = parse_detail(
        load("hubble_detail_opo0205c.html"),
        entry_id="opo0205c", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/opo0205c/",
    )
    assert entry.license is None       # unestablished, not defaulted


def test_permissive_credits_are_not_flagged_as_copyrighted():
    assert asserts_copyright(parse_credit(load("hubble_detail_heic0601a.html"))) is False
    assert asserts_copyright(parse_credit(load("eso_detail_eso1103a.html"))) is False
    assert asserts_copyright(None) is False


def test_artwork_has_no_position_and_no_filters():
    """An artist's impression has nothing to match; it must not enter the position index."""
    entry = parse_detail(
        load("hubble_detail_heic0211i.html"),
        entry_id="heic0211i", gallery="esa_hubble",
        detail_url="https://esahubble.org/images/heic0211i/",
    )
    assert entry.entry_type == "Artwork"
    assert entry.ra_deg is None
    assert entry.palette_class == "unknown"


def test_a_page_that_does_not_parse_at_all_reports_failure():
    """Distinguishing 'parsed fine, data absent' from 'parse broke' is what lets the
    crawler abort loudly on a site redesign instead of indexing an empty archive."""
    entry = parse_detail(
        "<html><body><p>503 Service Unavailable</p></body></html>",
        entry_id="whatever", gallery="eso",
        detail_url="https://www.eso.org/public/images/whatever/",
    )
    assert entry.parsed_ok is False
