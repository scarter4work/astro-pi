"""The Phase 1 exit criterion as an automated test:
solve -> look up -> miss -> fetch -> fingerprint -> store, unattended."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from autocontrast.db.discover.discover import acquire_reference, discover_reference
from autocontrast.db.discover.gallery import GalleryEntry
from autocontrast.db.discover.index import GalleryIndex
from autocontrast.db.store import FingerprintStore
from autocontrast.db.wcs import WcsResult

FIXTURES = Path(__file__).parent / "fixtures" / "gallery"


@pytest.fixture
def synthetic_render(tmp_path):
    """A small RGB raster standing in for a fetched gallery JPEG. Real press-release
    files are hundreds of megapixels; the fingerprint math does not care about content
    here, only that a real image is loadable."""
    rng = np.random.default_rng(42)
    data = (rng.random((128, 128, 3)) * 200 + 20).astype(np.uint8)
    path = tmp_path / "render.jpg"
    Image.fromarray(data).save(path, quality=92)
    return path


class StubFetcher:
    def __init__(self, image_bytes: bytes):
        self.image_bytes = image_bytes
        self.requested: list[str] = []

    def get_text(self, url: str) -> str:
        self.requested.append(url)
        return ""

    def get_bytes(self, url: str, *, max_bytes: int) -> bytes:
        self.requested.append(url)
        return self.image_bytes


def indexed_entry(entry_id="eso1103a", palette="RGB", license_text="CC BY 4.0"):
    return GalleryEntry(
        id=entry_id, gallery="eso",
        detail_url="https://www.eso.org/public/images/eso1103a/",
        image_url="https://cdn.eso.org/images/large/eso1103a.jpg",
        ra_deg=83.82217, dec_deg=-5.39099, fov_w_arcmin=35.49, fov_h_arcmin=34.10,
        fov_radius_arcmin=24.61, width_px=8948, height_px=8597,
        pixel_scale_arcsec=0.238, object_name="M 42", category="Nebulae",
        entry_type="Observation", palette_class=palette, license=license_text,
        attribution="ESO/Igor Chekalin", published_utc="2011-01-19T12:00:00Z",
        parsed_ok=True,
    )


@pytest.fixture
def wiring(tmp_path, synthetic_render):
    store = FingerprintStore(tmp_path / "fingerprints.sqlite")
    index = GalleryIndex(tmp_path / "gallery_index.sqlite")
    index.upsert(indexed_entry())
    fetcher = StubFetcher(synthetic_render.read_bytes())
    yield store, index, fetcher, tmp_path / "cache"
    store.close()
    index.close()


def manual_wcs(path):
    return WcsResult(83.82217, -5.39099, 24.61, "manual", True,
                     "WCS from manual annotation", pixel_scale_arcsec=0.238)


def test_discovery_ingests_a_reference_on_a_miss(wiring):
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.ingested is True
    assert outcome.record.source_type == "professional_render"
    assert store.get(outcome.record.id) is not None


def test_stored_record_carries_license_attribution_and_wcs_source(wiring):
    """§5.5 requires the attribution and source URL to travel with the record."""
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    provenance = outcome.record.provenance
    assert provenance["license"] == "CC BY 4.0"
    assert provenance["attribution"] == "ESO/Igor Chekalin"
    assert provenance["source_url"] == "https://www.eso.org/public/images/eso1103a/"
    assert provenance["wcs_source"] == "manual"


def test_stored_position_and_palette_come_from_the_render_not_the_query(wiring):
    """The user's palette is used only for ranking; a discovered record's palette is
    always that render's own."""
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="HOO", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.record.palette_class == "RGB"
    assert outcome.record.ra_deg == pytest.approx(83.82217, abs=1e-4)


def test_downloaded_image_is_cached_and_not_refetched(wiring):
    store, index, fetcher, cache = wiring
    for _ in range(2):
        discover_reference(
            store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
            palette_class="RGB", fetcher=fetcher, cache_dir=cache,
            wcs_acquirer=manual_wcs,
        )
    downloads = [u for u in fetcher.requested if u.endswith(".jpg")]
    assert len(downloads) == 1


def test_no_candidate_in_the_index_reports_cleanly(wiring):
    """An empty result is a first-class branch, not an error (§5.2)."""
    store, index, fetcher, cache = wiring
    outcome = discover_reference(
        store, index, ra_deg=200.0, dec_deg=40.0, search_radius_arcmin=10.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.ingested is False
    assert outcome.considered == []
    assert "no candidate" in outcome.detail.lower()


def test_rejected_candidates_are_reported_with_reasons(wiring):
    """§12: every degraded path surfaces.

    The unlicensed entry is given an id that sorts first AND identical framing to the
    good one, so ranking's id tie-break puts it first and it is guaranteed to be
    considered — the assertion can never pass vacuously.
    """
    store, index, fetcher, cache = wiring
    index.upsert(indexed_entry(entry_id="aaa_unlicensed", license_text=None))
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
        top_k=1,
    )
    reports = {r.id: r for r in outcome.considered}
    assert "aaa_unlicensed" in reports, f"expected it to be considered; got {list(reports)}"
    assert reports["aaa_unlicensed"].accepted is False
    assert "license" in reports["aaa_unlicensed"].reason.lower()


def test_discovery_falls_through_to_the_next_candidate_after_a_rejection(wiring):
    """A rejected candidate must not abort discovery — the next one gets its turn."""
    store, index, fetcher, cache = wiring
    index.upsert(indexed_entry(entry_id="aaa_broken", license_text=None))
    outcome = discover_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.ingested is True
    assert outcome.record.id == "eso:eso1103a"
    rejected = {r.id: r for r in outcome.considered if not r.accepted}
    assert "aaa_broken" in rejected      # it was tried first, and rejected


def test_acquire_reference_returns_existing_matches_without_discovering(wiring):
    store, index, fetcher, cache = wiring
    first = acquire_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert first.discovered is not None and first.discovered.ingested is True

    fetcher.requested.clear()
    second = acquire_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert second.discovered is None          # already had it; no discovery attempted
    assert second.matches
    assert fetcher.requested == []


def test_the_full_exit_criterion_runs_unattended(wiring):
    """solve -> look up -> miss -> fetch -> fingerprint -> store, with no prompts."""
    store, index, fetcher, cache = wiring
    outcome = acquire_reference(
        store, index, ra_deg=83.82, dec_deg=-5.39, search_radius_arcmin=30.0,
        palette_class="RGB", fetcher=fetcher, cache_dir=cache, wcs_acquirer=manual_wcs,
    )
    assert outcome.discovered.ingested is True
    stored = store.cone_search(83.82, -5.39, 30.0, palette_class="RGB")
    assert len(stored) == 1
    assert stored[0].palette_compatible is True
    assert stored[0].record.fingerprint is not None
