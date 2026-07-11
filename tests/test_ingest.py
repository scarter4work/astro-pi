"""Tests for the reference ingest pipeline (§5, §10 Phase 1 spine)."""

import numpy as np
import pytest
from PIL import Image

from autocontrast.db.ingest import ingest_reference
from autocontrast.db.store import FingerprintStore


def _write_png(path, size=96):
    rng = np.random.default_rng(0)
    img = rng.uniform(0, 0.05, size=(size, size, 3))
    yy, xx = np.mgrid[0:size, 0:size]
    blob = np.exp(-(((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (2 * (size / 8) ** 2)))
    img[..., 0] += 0.6 * blob
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(path)


_PROV = {"source_url": "https://esahubble.org/x/", "license": "CC BY 4.0",
         "attribution": "NASA/ESA", "ingested_utc": "2026-07-11T00:00:00Z",
         "wcs_source": "header"}


def _ingest(store, path, **overrides):
    kwargs = dict(id="ref-1", ra_deg=83.822, dec_deg=-5.391, fov_radius_arcmin=21.0,
                  pixel_scale_arcsec=1.0, psf_fwhm_arcsec=2.0, palette_class="RGB",
                  source_type="professional_render", provenance=_PROV)
    kwargs.update(overrides)
    return ingest_reference(store, path, **kwargs)


def test_ingest_stores_and_returns_record(tmp_path):
    png = tmp_path / "ref.png"; _write_png(png)
    store = FingerprintStore(tmp_path / "db.sqlite")

    record = _ingest(store, png)

    assert record.id == "ref-1"
    stored = store.get("ref-1")
    assert stored is not None
    assert stored.palette_class == "RGB"
    assert stored.fingerprint.tonal_quantiles.shape[0] == 10


def test_ingest_is_findable_by_cone_search(tmp_path):
    png = tmp_path / "ref.png"; _write_png(png)
    store = FingerprintStore(tmp_path / "db.sqlite")
    _ingest(store, png)

    matches = store.cone_search(83.822, -5.391, radius_arcmin=5.0, palette_class="RGB")
    assert [m.record.id for m in matches] == ["ref-1"]
    assert matches[0].palette_compatible is True


def test_ingest_auto_generates_id_when_missing(tmp_path):
    png = tmp_path / "ref.png"; _write_png(png)
    store = FingerprintStore(tmp_path / "db.sqlite")

    record = _ingest(store, png, id=None)

    assert record.id
    assert store.get(record.id) is not None


def test_ingest_rejects_tool_output(tmp_path):
    png = tmp_path / "ref.png"; _write_png(png)
    store = FingerprintStore(tmp_path / "db.sqlite")
    with pytest.raises(ValueError, match="tool_output"):
        _ingest(store, png, source_type="tool_output")
