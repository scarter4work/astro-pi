"""Tests for the SQLite FingerprintStore + provenance enforcement (§5.3)."""

import numpy as np
import pytest

from autocontrast.db.records import ReferenceRecord
from autocontrast.db.store import FingerprintStore
from autocontrast.fingerprint.extract import extract


def _record(source_type="professional_render", id="ref-1", ra=83.822, dec=-5.391,
            palette="RGB"):
    rng = np.random.default_rng(0)
    img = np.clip(rng.uniform(0, 0.4, size=(64, 64, 3)), 0, 1)
    fp = extract(img, pixel_scale_arcsec=0.05, n_scales=6, psf_fwhm_arcsec=0.11,
                 palette_class=palette)
    return ReferenceRecord(
        id=id, ra_deg=ra, dec_deg=dec, fov_radius_arcmin=21.0,
        palette_class=palette, source_type=source_type, fingerprint=fp,
        provenance={
            "source_url": "https://esahubble.org/images/heic0601a/",
            "license": "CC BY 4.0", "attribution": "NASA/ESA",
            "ingested_utc": "2026-07-11T00:00:00Z", "wcs_source": "header",
        },
    )


def test_store_and_get_round_trips_a_record(tmp_path):
    store = FingerprintStore(tmp_path / "fp.sqlite")
    rec = _record()
    store.store(rec)

    got = store.get("ref-1")
    assert got is not None
    assert got.id == "ref-1"
    assert got.palette_class == "RGB"
    assert got.source_type == "professional_render"
    np.testing.assert_allclose(got.fingerprint.tonal_quantiles, rec.fingerprint.tonal_quantiles)
    np.testing.assert_allclose(got.fingerprint.chroma_hist, rec.fingerprint.chroma_hist)


def test_tool_output_is_rejected_at_ingest(tmp_path):
    """§5.3: autophagy guard — the tool's own output must never enter the store."""
    store = FingerprintStore(tmp_path / "fp.sqlite")
    with pytest.raises(ValueError, match="tool_output"):
        store.store(_record(source_type="tool_output"))
    assert store.get("ref-1") is None  # nothing persisted


def test_unknown_source_type_is_rejected(tmp_path):
    store = FingerprintStore(tmp_path / "fp.sqlite")
    with pytest.raises(ValueError):
        store.store(_record(source_type="handwavy"))


def test_user_render_is_stored(tmp_path):
    store = FingerprintStore(tmp_path / "fp.sqlite")
    store.store(_record(source_type="user_render"))
    assert store.get("ref-1").source_type == "user_render"


def test_get_missing_returns_none(tmp_path):
    store = FingerprintStore(tmp_path / "fp.sqlite")
    assert store.get("nope") is None


def test_store_persists_across_connections(tmp_path):
    path = tmp_path / "fp.sqlite"
    FingerprintStore(path).store(_record())
    assert FingerprintStore(path).get("ref-1") is not None
