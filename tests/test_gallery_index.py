"""The local position index — a genuine cone search over professional renders (§5.1)."""

from __future__ import annotations

import pytest

from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.discover.index import GalleryIndex


def entry(entry_id="x1", gallery="esa_hubble", ra=83.8, dec=-5.4, radius=20.0,
          palette="RGB", license_text="CC BY 4.0", attribution="NASA, ESA"):
    return GalleryEntry(
        id=entry_id, gallery=gallery,
        detail_url=f"https://example.test/images/{entry_id}/",
        image_url=f"https://cdn.example.test/images/large/{entry_id}.jpg",
        ra_deg=ra, dec_deg=dec, fov_w_arcmin=30.0, fov_h_arcmin=30.0,
        fov_radius_arcmin=radius, width_px=18000, height_px=18000,
        pixel_scale_arcsec=0.1001, object_name="Messier 42", category="Nebulae",
        entry_type="Observation", palette_class=palette, license=license_text,
        attribution=attribution, published_utc="2006-01-11T16:00:00Z", parsed_ok=True,
    )


@pytest.fixture
def index(tmp_path):
    idx = GalleryIndex(tmp_path / "gallery_index.sqlite")
    yield idx
    idx.close()


def test_roundtrip_preserves_every_field(index):
    original = entry()
    index.upsert(original)
    assert index.get("esa_hubble", "x1") == original


def test_upsert_is_idempotent_on_gallery_and_id(index):
    index.upsert(entry(ra=83.8))
    index.upsert(entry(ra=99.9))
    assert index.count() == 1
    assert index.get("esa_hubble", "x1").ra_deg == pytest.approx(99.9)


def test_same_id_in_two_galleries_are_distinct_rows(index):
    index.upsert(entry(entry_id="dup", gallery="esa_hubble"))
    index.upsert(entry(entry_id="dup", gallery="eso"))
    assert index.count() == 2


def test_has_reports_membership_so_the_crawler_can_skip(index):
    assert index.has("esa_hubble", "x1") is False
    index.upsert(entry())
    assert index.has("esa_hubble", "x1") is True


def test_cone_search_finds_an_overlapping_footprint(index):
    index.upsert(entry(ra=83.82, dec=-5.39, radius=24.6))
    matches = index.cone_search(83.82, -5.38, radius_arcmin=30.0)
    assert [m.entry.id for m in matches] == ["x1"]
    assert matches[0].separation_arcmin < 1.0


def test_cone_search_excludes_a_disjoint_footprint(index):
    index.upsert(entry(ra=83.8, dec=-5.4, radius=5.0))
    assert index.cone_search(200.0, 40.0, radius_arcmin=10.0) == []


def test_overlap_is_boundary_inclusive(index):
    """Footprints exactly touching count as overlapping, matching FingerprintStore."""
    index.upsert(entry(ra=10.0, dec=0.0, radius=6.0))
    assert len(index.cone_search(10.0, 10.0 / 60.0, radius_arcmin=4.0)) == 1


def test_entries_without_a_position_are_never_returned(index):
    """opo0205c-style NULL rows are remembered so the crawler skips them, but they can
    never be selected as a reference."""
    index.upsert(entry(entry_id="nopos", ra=None, dec=None, radius=None))
    assert index.has("esa_hubble", "nopos") is True
    assert index.cone_search(83.8, -5.4, radius_arcmin=60.0) == []


def test_cone_search_handles_ra_wrap(index):
    """The dec-band SQL prefilter cannot express RA wrap, so the precise haversine
    check must catch it: 359.9 and 0.1 are 12 arcmin apart, not 359 degrees.

    The separation assertion is the load-bearing one — a wrap-broken implementation
    computes |359.9 - 0.1| * 60 = 21588 arcmin, so asserting ~12 discriminates a
    correct implementation from a wrong one in a way a bare match count cannot.
    """
    index.upsert(entry(ra=359.9, dec=0.0, radius=20.0))
    matches = index.cone_search(0.1, 0.0, radius_arcmin=5.0)
    assert len(matches) == 1
    assert matches[0].separation_arcmin == pytest.approx(12.0, abs=0.1)


def test_cone_search_orders_nearest_first(index):
    index.upsert(entry(entry_id="far", ra=84.3, dec=-5.4, radius=20.0))
    index.upsert(entry(entry_id="near", ra=83.81, dec=-5.4, radius=20.0))
    assert [m.entry.id for m in index.cone_search(83.8, -5.4, 10.0)] == ["near", "far"]


def test_sync_state_roundtrips_per_gallery(index):
    assert index.get_sync_state("eso") is None
    index.set_sync_state("eso", "2026-07-26T00:00:00Z")
    index.set_sync_state("esa_hubble", "2026-01-01T00:00:00Z")
    assert index.get_sync_state("eso") == "2026-07-26T00:00:00Z"
    assert index.get_sync_state("esa_hubble") == "2026-01-01T00:00:00Z"


def test_index_is_a_separate_file_from_the_fingerprint_store(tmp_path):
    """The index is a regenerable external-derived cache; deleting it must never risk
    user fingerprints, so it lives in its own file."""
    path = tmp_path / "gallery_index.sqlite"
    idx = GalleryIndex(path)
    idx.upsert(entry())
    idx.close()
    assert path.exists()
    assert not (tmp_path / "fingerprints.sqlite").exists()
