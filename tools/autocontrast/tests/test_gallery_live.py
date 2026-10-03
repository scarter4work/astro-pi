"""Opt-in drift detector. Deselected by default; run deliberately:

    .venv/bin/python -m pytest tests/test_gallery_live.py -m live -v

Its only job is to tell you whether the committed fixtures still reflect reality. If it
fails, re-run tools/capture_gallery_fixtures.sh and fix the parsers — do NOT relax the
assertions, which would hide the drift the crawler's DriftError exists to catch.
"""

from __future__ import annotations

import pytest

from autocontrast.db.discover.crawl import PoliteFetcher
from autocontrast.db.discover.gallery import GALLERIES, detail_url, parse_detail

pytestmark = pytest.mark.live


@pytest.mark.parametrize(
    "gallery, entry_id, ra, dec, radius",
    [
        ("esa_hubble", "heic0601a", 83.7905, -5.4140, 21.23),
        ("eso", "eso1103a", 83.82217, -5.39099, 24.61),
    ],
)
def test_live_detail_pages_still_publish_position_and_field_of_view(
    gallery, entry_id, ra, dec, radius
):
    cfg = GALLERIES[gallery]
    url = detail_url(cfg, entry_id)
    entry = parse_detail(
        PoliteFetcher().get_text(url), entry_id=entry_id, gallery=gallery, detail_url=url
    )
    assert entry.parsed_ok is True, f"{entry_id}: detail page no longer parses"
    assert entry.ra_deg == pytest.approx(ra, abs=1e-3)
    assert entry.dec_deg == pytest.approx(dec, abs=1e-3)
    assert entry.fov_radius_arcmin == pytest.approx(radius, abs=0.05)
    assert entry.attribution, f"{entry_id}: credit block no longer found (gate G5 blocker)"
    assert entry.image_url, f"{entry_id}: CDN image URL no longer found"
