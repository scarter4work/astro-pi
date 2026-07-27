"""Crawl orchestration. Network is behind the Fetcher protocol, so these tests are
offline and deterministic."""

from __future__ import annotations

from pathlib import Path

import pytest

from autocontrast.db.discover.crawl import (
    DRIFT_THRESHOLD,
    MAX_IMAGE_BYTES,
    DriftError,
    crawl_gallery,
    sync_gallery,
)
from autocontrast.db.discover.gallery import GALLERIES
from autocontrast.db.discover.index import GalleryIndex

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


def load(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8", errors="replace")


class FakeFetcher:
    """Serves fixtures by URL substring and records the request order."""

    def __init__(self, routes: dict[str, str], default: str | None = None):
        self.routes = routes
        self.default = default
        self.requested: list[str] = []

    def get_text(self, url: str) -> str:
        self.requested.append(url)
        for needle, body in self.routes.items():
            if needle in url:
                return body
        if self.default is not None:
            return self.default
        raise AssertionError(f"unrouted URL: {url}")

    def get_bytes(self, url: str, *, max_bytes: int) -> bytes:
        self.requested.append(url)
        return b"\xff\xd8\xff\xd9"


@pytest.fixture
def index(tmp_path):
    idx = GalleryIndex(tmp_path / "gallery_index.sqlite")
    yield idx
    idx.close()


def hubble_routes():
    return {
        "archive/search": load("hubble_listing_orion.html"),
        "/images/heic0601a/": load("hubble_detail_heic0601a.html"),
        "/images/opo0205c/": load("hubble_detail_opo0205c.html"),
        "/images/heic0211i/": load("hubble_detail_heic0211i.html"),
    }


def test_crawl_indexes_positioned_and_positionless_entries(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.listed == 35
    assert report.indexed == 35
    assert index.has("esa_hubble", "heic0601a")
    assert index.has("esa_hubble", "opo0205c")


def test_crawled_entry_carries_position_and_scale_from_published_metadata(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                  {"subject_name": "Orion Nebula"}, max_pages=1)
    entry = index.get("esa_hubble", "heic0601a")
    assert entry.ra_deg == pytest.approx(83.7905, abs=1e-3)
    assert entry.pixel_scale_arcsec == pytest.approx(0.1001, abs=1e-4)
    assert entry.width_px == 18000        # from the listing, not the download


def test_crawl_skips_already_indexed_ids(index):
    routes = hubble_routes()
    first = FakeFetcher(routes, default=load("hubble_detail_heic0211i.html"))
    crawl_gallery(index, GALLERIES["esa_hubble"], first,
                  {"subject_name": "Orion Nebula"}, max_pages=1)

    second = FakeFetcher(routes, default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], second,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.skipped == 35
    assert report.indexed == 0
    detail_requests = [u for u in second.requested if "archive/search" not in u]
    assert detail_requests == []          # resumable: no wasted requests


def test_refresh_re_fetches_indexed_ids(index):
    routes = hubble_routes()
    crawl_gallery(index, GALLERIES["esa_hubble"],
                  FakeFetcher(routes, default=load("hubble_detail_heic0211i.html")),
                  {"subject_name": "Orion Nebula"}, max_pages=1)
    fetcher = FakeFetcher(routes, default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1, refresh=True)
    assert report.indexed == 35
    assert report.skipped == 0


def test_site_drift_aborts_loudly_instead_of_indexing_an_empty_archive(index):
    """A Djangoplicity redesign must be a noisy failure, not a quietly-empty index (§12)."""
    fetcher = FakeFetcher({"archive/search": load("hubble_listing_orion.html")},
                          default="<html><body>503 Service Unavailable</body></html>")
    with pytest.raises(DriftError) as excinfo:
        crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                      {"subject_name": "Orion Nebula"}, max_pages=1)
    assert "parse" in str(excinfo.value).lower()


def test_legitimately_positionless_pages_do_not_trip_the_drift_guard(index):
    """Artwork and old photographic releases publish no position; that is normal data
    absence, not parse breakage, so a page full of them must NOT abort."""
    fetcher = FakeFetcher({"archive/search": load("hubble_listing_orion.html")},
                          default=load("hubble_detail_heic0211i.html"))
    report = crawl_gallery(index, GALLERIES["esa_hubble"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.indexed == 35
    assert report.failed == 0


def test_eso_crawl_uses_the_unpaginated_results_path(index):
    fetcher = FakeFetcher({
        "archive/search": load("eso_listing_orion.html"),
        "/public/images/eso1103a/": load("eso_detail_eso1103a.html"),
    }, default=load("eso_detail_eso1103a.html"))
    report = crawl_gallery(index, GALLERIES["eso"], fetcher,
                           {"subject_name": "Orion Nebula"}, max_pages=1)
    assert report.listed == 26
    assert all("/page/" not in u for u in fetcher.requested if "archive/search" in u)


def test_sync_queries_from_the_last_sync_minus_the_safety_window(index):
    index.set_sync_state("esa_hubble", "2026-07-20T00:00:00Z")
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    sync_gallery(index, GALLERIES["esa_hubble"], fetcher, now_utc="2026-07-26T00:00:00Z")
    search = next(u for u in fetcher.requested if "archive/search" in u)
    # 2026-07-20 minus the 7-day safety window
    assert "published_since_year=2026" in search
    assert "published_since_month=7" in search
    assert "published_since_day=13" in search


def test_first_sync_with_no_state_crawls_from_the_epoch_year(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    sync_gallery(index, GALLERIES["esa_hubble"], fetcher, now_utc="2026-07-26T00:00:00Z")
    search = next(u for u in fetcher.requested if "archive/search" in u)
    assert "published_since_year=1990" in search


def test_sync_records_new_state_so_the_next_run_is_incremental(index):
    fetcher = FakeFetcher(hubble_routes(), default=load("hubble_detail_heic0211i.html"))
    sync_gallery(index, GALLERIES["esa_hubble"], fetcher, now_utc="2026-07-26T00:00:00Z")
    assert index.get_sync_state("esa_hubble") == "2026-07-26T00:00:00Z"


def test_download_cap_is_a_quarter_gigabyte():
    """The 18000x18000 Hubble Orion mosaic is the largest known case and fits inside."""
    assert MAX_IMAGE_BYTES == 256 * 1024 * 1024
    assert DRIFT_THRESHOLD == 0.5
