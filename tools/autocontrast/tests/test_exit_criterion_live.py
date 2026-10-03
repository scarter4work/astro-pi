"""The Phase 1 exit criterion, run for real (§10).

Nothing here is stubbed. This is the whole claim, end to end:

    real stacked FITS -> real header WCS solve -> real fingerprint-store MISS ->
    real cone search over a real crawled index -> real HTTP download of a real
    press-release JPEG -> real verification gate -> real fingerprint -> real store

``tests/test_discover_e2e.py`` covers the same wiring against a fake fetcher and a
synthetic raster. That is useful for branch coverage and it is fast, but it CANNOT
establish the exit criterion: a stub proves the code calls itself in the right order,
not that the archives publish what we think they publish or that a real 8000-pixel
JPEG survives the gate. Hence this file.

The user image is their own M42 stack (ZWO ASI2400MC Pro, 1213 mm, FILTER=HaO3), which
solves from its own FITS header — tier 1, no plate solver needed.

Cost control, so running the suite is not abusive to free public archives:
  * the position index at ``data/gallery_index.sqlite`` is built once and reused —
    this is also exactly how the system works in production (crawl once, query locally)
  * downloaded renders are cached under ``data/discovery_cache/`` and never refetched
A first run does real network work; later runs reuse both caches and still exercise
verification, fingerprinting and ingest for real.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from autocontrast.db.discover.crawl import PoliteFetcher, crawl_gallery
from autocontrast.db.discover.discover import acquire_reference
from autocontrast.db.discover.gallery import GALLERIES
from autocontrast.db.discover.index import GalleryIndex
from autocontrast.db.store import FingerprintStore
from autocontrast.db.wcs import acquire_wcs
from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import extract
from autocontrast.fingerprint.palette import palette_chroma_compatible
from autocontrast.io.loaders import downsample_factor, load_image

pytestmark = pytest.mark.live

# The user's own stacked M42 (see project notes). Deliberately NOT a fixture file:
# the exit criterion is about real acquisition data, not something we authored.
USER_M42 = Path(
    "/mnt/qnap/astro_data/9_16_2023/M42/Stacked14_M42_10.0s_Bin1_HaO3_20230916-043848.fit"
)
USER_PALETTE = "HOO"          # derived from FILTER=HaO3

REPO = Path(__file__).resolve().parent.parent
INDEX_PATH = REPO / "data" / "gallery_index.sqlite"
CACHE_DIR = REPO / "data" / "discovery_cache"

# Orion Nebula is the covered field. minimum_size=2 on Hubble drops thumbnails.
CRAWL_TARGETS = [
    ("eso", {"subject_name": "Orion Nebula"}),
    ("esa_hubble", {"subject_name": "Orion Nebula", "minimum_size": "2"}),
]


@pytest.fixture(scope="module")
def user_wcs():
    """Solve the user's real stack. Tier 1 (FITS header) — no stub, no annotation."""
    if not USER_M42.exists():
        raise AssertionError(
            f"The exit criterion needs the real acquisition data at {USER_M42}, which is "
            "not readable (is /mnt/qnap mounted?). This is deliberately a FAILURE and not "
            "a skip: the suite must not report green while the one test that proves the "
            "Phase 1 claim quietly sat out."
        )
    wcs = acquire_wcs(USER_M42)
    assert wcs.solved is True, f"user stack did not solve: {wcs.detail}"
    assert wcs.wcs_source == "header"
    return wcs


@pytest.fixture(scope="module")
def live_index(user_wcs):
    """A real position index, crawled from the real galleries if not already present."""
    INDEX_PATH.parent.mkdir(parents=True, exist_ok=True)
    index = GalleryIndex(INDEX_PATH)
    covering = index.cone_search(
        user_wcs.ra_deg, user_wcs.dec_deg, user_wcs.fov_radius_arcmin
    )
    if not covering:
        fetcher = PoliteFetcher()
        for key, criteria in CRAWL_TARGETS:
            crawl_gallery(index, GALLERIES[key], fetcher, criteria, max_pages=1)
    yield index
    index.close()


def test_the_index_was_built_from_real_crawled_pages(live_index, user_wcs):
    """Guards the fixture above: an empty or non-covering index would make every
    assertion below vacuous."""
    assert live_index.count() > 0
    covering = live_index.cone_search(
        user_wcs.ra_deg, user_wcs.dec_deg, user_wcs.fov_radius_arcmin
    )
    assert covering, "no indexed render covers the user's field — nothing to discover"
    for match in covering:
        assert match.entry.detail_url.startswith("https://")
        assert match.entry.parsed_ok is True


def test_exit_criterion_real_miss_discovers_and_ingests_a_real_render(
    live_index, user_wcs, tmp_path
):
    """solve -> look up -> MISS -> fetch -> fingerprint -> store, unattended, for real."""
    store = FingerprintStore(tmp_path / "fingerprints.sqlite")
    try:
        # A genuinely empty store: the miss is real, not arranged.
        assert store.cone_search(
            user_wcs.ra_deg, user_wcs.dec_deg, user_wcs.fov_radius_arcmin, USER_PALETTE
        ) == []

        outcome = acquire_reference(
            store, live_index,
            ra_deg=user_wcs.ra_deg, dec_deg=user_wcs.dec_deg,
            search_radius_arcmin=user_wcs.fov_radius_arcmin,
            palette_class=USER_PALETTE,
            fetcher=PoliteFetcher(),          # real HTTP, 1 req/s, real User-Agent
            cache_dir=CACHE_DIR,
        )

        assert outcome.discovered is not None, "no discovery was attempted"
        assert outcome.discovered.ingested is True, (
            "discovery ingested nothing. Candidates considered:\n"
            + "\n".join(f"  {r.id}: {r.reason}" for r in outcome.discovered.considered)
        )

        record = outcome.discovered.record
        assert record.source_type == "professional_render"
        assert store.get(record.id) is not None, "record is not actually in the store"

        # The store went from miss to hit — that IS the criterion.
        after = store.cone_search(
            user_wcs.ra_deg, user_wcs.dec_deg, user_wcs.fov_radius_arcmin, USER_PALETTE
        )
        assert len(after) == 1
        assert after[0].record.fingerprint is not None
    finally:
        store.close()


def test_the_downloaded_render_is_a_real_multi_megabyte_jpeg(live_index, user_wcs, tmp_path):
    """Proves an actual press-release file crossed the wire, not a synthetic raster.

    The synthetic stand-in in test_discover_e2e.py is 128x128; a real ESO/Hubble 'large'
    render is thousands of pixels across and megabytes on disk. Asserting the size is
    what distinguishes 'we downloaded something' from 'we downloaded the real thing'.
    """
    store = FingerprintStore(tmp_path / "fingerprints.sqlite")
    try:
        outcome = acquire_reference(
            store, live_index,
            ra_deg=user_wcs.ra_deg, dec_deg=user_wcs.dec_deg,
            search_radius_arcmin=user_wcs.fov_radius_arcmin,
            palette_class=USER_PALETTE,
            fetcher=PoliteFetcher(), cache_dir=CACHE_DIR,
        )
        assert outcome.discovered.ingested is True
    finally:
        store.close()

    cached = sorted(CACHE_DIR.glob("*.jpg"))
    assert cached, f"nothing was downloaded into {CACHE_DIR}"
    biggest = max(cached, key=lambda p: p.stat().st_size)
    assert biggest.stat().st_size > 1_000_000, (
        f"{biggest.name} is only {biggest.stat().st_size} bytes — that is not a real "
        "press-release render"
    )
    assert biggest.read_bytes()[:2] == b"\xff\xd8", "not actually a JPEG"

    from PIL import Image
    with Image.open(biggest) as img:
        assert min(img.size) > 1000, f"{biggest.name} is only {img.size} — too small to be real"


def test_the_discovered_record_carries_its_licensing_obligation(live_index, user_wcs, tmp_path):
    """§5.5: a discovered reference is unusable unless the obligation travels with it."""
    store = FingerprintStore(tmp_path / "fingerprints.sqlite")
    try:
        outcome = acquire_reference(
            store, live_index,
            ra_deg=user_wcs.ra_deg, dec_deg=user_wcs.dec_deg,
            search_radius_arcmin=user_wcs.fov_radius_arcmin,
            palette_class=USER_PALETTE,
            fetcher=PoliteFetcher(), cache_dir=CACHE_DIR,
        )
        provenance = outcome.discovered.record.provenance
        assert provenance["license"], "ingested with no established license"
        assert provenance["attribution"], "ingested with no attribution"
        assert provenance["source_url"].startswith("https://")
        assert provenance["gallery"] in GALLERIES
        assert provenance["discovered"] is True
        assert provenance["wcs_source"] in {"header", "avm", "blind", "manual"}
    finally:
        store.close()


def test_the_discovered_reference_is_usable_against_the_users_own_fingerprint(
    live_index, user_wcs, tmp_path
):
    """The point of discovering a reference is to measure distance to it.

    This assertion was originally written the other way round — the author assumed M42's
    professional renders are all broadband, so an HOO acquisition would have chroma gated
    out. The real run disproved that: ESA/Hubble's Orion set includes narrowband
    composites (opo9545a1 publishes Oiii, H-alpha, Nii), and §2.3's palette-first ranking
    correctly surfaced one AHEAD of all 35 broadband candidates covering the same field.

    So what is locked in here is the actual contract: the compatibility flag must agree
    with the §2.3 family relation, and when a chroma-compatible reference exists in the
    index the ranking must be the thing that finds it.
    """
    store = FingerprintStore(tmp_path / "fingerprints.sqlite")
    try:
        outcome = acquire_reference(
            store, live_index,
            ra_deg=user_wcs.ra_deg, dec_deg=user_wcs.dec_deg,
            search_radius_arcmin=user_wcs.fov_radius_arcmin,
            palette_class=USER_PALETTE,
            fetcher=PoliteFetcher(), cache_dir=CACHE_DIR,
        )
        reference = outcome.discovered.record

        rgb = load_image(USER_M42, max_dim=1600)
        effective_scale = user_wcs.pixel_scale_arcsec * downsample_factor(USER_M42, rgb)
        user_fingerprint = extract(
            rgb, pixel_scale_arcsec=effective_scale, n_scales=7,
            psf_fwhm_arcsec=2.0, palette_class=USER_PALETTE,
        )

        distance = fingerprint_distance(reference.fingerprint, user_fingerprint)
        assert distance == distance, "distance is NaN"      # NaN != NaN
        assert 0.0 <= distance < float("inf")

        match = store.cone_search(
            user_wcs.ra_deg, user_wcs.dec_deg, user_wcs.fov_radius_arcmin, USER_PALETTE
        )[0]
        assert match.palette_compatible is palette_chroma_compatible(
            reference.palette_class, USER_PALETTE
        ), "the store's compatibility flag disagrees with the §2.3 family relation"

        # A compatible reference exists in the real index, so ranking must have chosen
        # one — palette compatibility is the FIRST ranking key precisely for this.
        compatible_available = [
            m for m in live_index.cone_search(
                user_wcs.ra_deg, user_wcs.dec_deg, user_wcs.fov_radius_arcmin
            )
            if palette_chroma_compatible(m.entry.palette_class, USER_PALETTE)
        ]
        assert compatible_available, (
            "no chroma-compatible render in the index — this assertion would be vacuous"
        )
        assert match.palette_compatible is True, (
            f"{len(compatible_available)} chroma-compatible render(s) were available, but "
            f"ranking ingested {reference.palette_class} instead"
        )
    finally:
        store.close()
