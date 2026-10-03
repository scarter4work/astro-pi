"""Tests for cone-search lookup (§5.1) — key on sky position, not target name."""

import numpy as np

from autocontrast.db.records import ReferenceRecord
from autocontrast.db.store import FingerprintStore
from autocontrast.fingerprint.extract import extract


def _rec(id, ra, dec, *, fov=5.0, palette="RGB", source="professional_render"):
    rng = np.random.default_rng(abs(hash(id)) % 2**32)
    img = np.clip(rng.uniform(0, 0.4, size=(48, 48, 3)), 0, 1)
    fp = extract(img, pixel_scale_arcsec=1.0, n_scales=5, psf_fwhm_arcsec=2.0,
                 palette_class=palette)
    return ReferenceRecord(id=id, ra_deg=ra, dec_deg=dec, fov_radius_arcmin=fov,
                           palette_class=palette, source_type=source, fingerprint=fp,
                           provenance={"source_url": "", "license": "", "attribution": "",
                                       "ingested_utc": "2026-07-11T00:00:00Z",
                                       "wcs_source": "header"})


def test_record_at_query_position_matches(tmp_path):
    store = FingerprintStore(tmp_path / "c.sqlite")
    store.store(_rec("a", 10.0, 0.0, fov=5.0))
    matches = store.cone_search(10.0, 0.0, radius_arcmin=5.0)
    assert [m.record.id for m in matches] == ["a"]


def test_far_record_does_not_match(tmp_path):
    store = FingerprintStore(tmp_path / "c.sqlite")
    store.store(_rec("a", 10.0, 0.0, fov=5.0))
    assert store.cone_search(200.0, 0.0, radius_arcmin=5.0) == []


def test_matches_are_ordered_by_separation(tmp_path):
    store = FingerprintStore(tmp_path / "c.sqlite")
    store.store(_rec("far", 10.1, 0.0, fov=5.0))   # 6 arcmin away
    store.store(_rec("near", 10.0, 0.0, fov=5.0))  # 0 arcmin away
    ids = [m.record.id for m in store.cone_search(10.0, 0.0, radius_arcmin=30.0)]
    assert ids == ["near", "far"]


def test_overlap_uses_record_footprint_radius(tmp_path):
    store = FingerprintStore(tmp_path / "c.sqlite")
    # Query center is 20' from the record center, query radius only 5', but the
    # record's own footprint is 20' — the cones still overlap (5+20 >= 20).
    store.store(_rec("wide", 10.0, 0.0, fov=20.0))
    matches = store.cone_search(10.0 + 20.0 / 60.0, 0.0, radius_arcmin=5.0)
    assert [m.record.id for m in matches] == ["wide"]


def test_palette_compatibility_is_flagged_not_filtered(tmp_path):
    store = FingerprintStore(tmp_path / "c.sqlite")
    store.store(_rec("sho", 10.0, 0.0, palette="SHO"))
    matches = store.cone_search(10.0, 0.0, radius_arcmin=5.0, palette_class="RGB")
    # §2.3: a palette-mismatched reference still matches (structural+tonal only).
    assert len(matches) == 1
    assert matches[0].palette_compatible is False


def test_source_type_filter_restricts_results(tmp_path):
    store = FingerprintStore(tmp_path / "c.sqlite")
    store.store(_rec("pro", 10.0, 0.0, source="professional_render"))
    store.store(_rec("user", 10.0, 0.0, source="user_render"))
    ids = [m.record.id for m in store.cone_search(
        10.0, 0.0, radius_arcmin=5.0, source_types={"professional_render"})]
    assert ids == ["pro"]
