"""Listing-page parsing. Results arrive as an inline `var images = [...]` JS literal —
machine-oriented data that churns far less than rendered markup, and it publishes pixel
dimensions, which is what makes gate G4 satisfiable without a download."""

from __future__ import annotations

from pathlib import Path

from autocontrast.db.discover.gallery import parse_listing, parse_result_total

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


def load(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8", errors="replace")


def test_hubble_listing_yields_every_result():
    """The page reports 'Showing 1 to 35 of 35'."""
    items = parse_listing(load("hubble_listing_orion.html"))
    assert len(items) == 35


def test_eso_listing_yields_every_result():
    items = parse_listing(load("eso_listing_orion.html"))
    assert len(items) == 26


def test_listing_publishes_dimensions_used_for_pixel_scale():
    items = {i.id: i for i in parse_listing(load("hubble_listing_orion.html"))}
    assert items["heic0601a"].width_px == 18000
    assert items["heic0601a"].height_px == 18000
    assert items["heic0601a"].detail_path == "/images/heic0601a/"


def test_eso_detail_paths_carry_the_public_prefix():
    items = {i.id: i for i in parse_listing(load("eso_listing_orion.html"))}
    assert items["eso1103a"].width_px == 8948
    assert items["eso1103a"].height_px == 8597
    assert items["eso1103a"].detail_path == "/public/images/eso1103a/"


def test_titles_are_html_unescaped():
    items = {i.id: i for i in parse_listing(load("hubble_listing_orion.html"))}
    assert items["heic0601a"].title == "Hubble's sharpest view of the Orion Nebula"


def test_listing_contains_the_curated_seed_ids():
    """Four of the five hand-curated M42 seeds are discovered automatically, which is
    the evidence that the curated catalog is a subset of what discovery returns."""
    ids = {i.id for i in parse_listing(load("hubble_listing_orion.html"))}
    assert {"heic0601a", "opo0205c", "opo0205d", "opo9545a"} <= ids


def test_navigation_links_are_not_mistaken_for_results():
    ids = {i.id for i in parse_listing(load("hubble_listing_orion.html"))}
    assert not ids & {"feed", "potm", "potw", "search", "viewall", "archive"}


def test_result_total_is_read_from_the_page():
    assert parse_result_total(load("hubble_listing_orion.html")) == 35
    assert parse_result_total(load("eso_listing_orion.html")) == 26


def test_page_without_a_results_block_yields_nothing_rather_than_raising():
    assert parse_listing("<html><body>No results.</body></html>") == []
    assert parse_result_total("<html><body>No results.</body></html>") is None
