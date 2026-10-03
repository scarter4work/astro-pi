"""Tests for the curated seed catalog + seeder (§5.2 baseline reference sourcing).

A seed entry declares a render's URL, palette, provenance and a fallback position.
The seeder prefers WCS embedded in the image itself (AVM) and uses the declared
position only as a manual annotation — position from the image always wins.
"""

import json

import numpy as np
import pytest
from astropy.wcs import WCS
from PIL import Image
from pyavm import AVM

from autocontrast.db.seed import load_catalog, seed_catalog
from autocontrast.db.store import FingerprintStore

RA, DEC = 83.822, -5.391


def _entry(**over):
    e = {
        "id": "heic0601a",
        "url": "https://cdn.esahubble.org/archives/images/large/heic0601a.jpg",
        "filename": "heic0601a.jpg",
        "palette_class": "RGB",
        "source_type": "professional_render",
        "attribution": "NASA/ESA",
        "license": "CC BY 4.0",
        "source_url": "https://esahubble.org/images/heic0601a/",
        "ra_deg": RA, "dec_deg": DEC, "fov_radius_arcmin": 21.0,
        "pixel_scale_arcsec": 1.0, "psf_fwhm_arcsec": 2.0,
    }
    e.update(over)
    return e


def _plain_jpg(path, nx=64, ny=64):
    rng = np.random.default_rng(0)
    Image.fromarray((rng.uniform(0, 0.5, (ny, nx, 3)) * 255).astype(np.uint8)).save(path)


def _avm_jpg(path, tmp_path, nx=64, ny=64, scale_arcsec=0.05):
    plain = tmp_path / "_p.jpg"; _plain_jpg(plain, nx, ny)
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crpix = [nx / 2, ny / 2]
    w.wcs.crval = [RA, DEC]
    w.wcs.cdelt = [-scale_arcsec / 3600, scale_arcsec / 3600]
    w.pixel_shape = (nx, ny)
    AVM.from_wcs(w, shape=(ny, nx)).embed(str(plain), str(path))


def test_load_catalog_reads_entries(tmp_path):
    path = tmp_path / "cat.json"
    path.write_text(json.dumps([_entry()]))
    entries = load_catalog(path)
    assert entries[0]["id"] == "heic0601a"


def test_seeds_declared_position_when_image_has_no_avm(tmp_path):
    img_dir = tmp_path / "img"; img_dir.mkdir()
    _plain_jpg(img_dir / "heic0601a.jpg")
    store = FingerprintStore(tmp_path / "db.sqlite")

    outcomes = seed_catalog(store, [_entry()], image_dir=img_dir, n_scales=5)

    assert outcomes[0].ingested
    assert outcomes[0].wcs_source == "manual"  # fell back to the declared position
    rec = store.get("heic0601a")
    assert rec.provenance["attribution"] == "NASA/ESA"
    assert rec.provenance["license"] == "CC BY 4.0"


def test_embedded_avm_beats_the_declared_position(tmp_path):
    """The image's own WCS is authoritative; the catalog's declared position is only
    a fallback (§2.2 — a real solve is never overridden by a nominal value)."""
    img_dir = tmp_path / "img"; img_dir.mkdir()
    _avm_jpg(img_dir / "heic0601a.jpg", tmp_path, scale_arcsec=0.05)
    store = FingerprintStore(tmp_path / "db.sqlite")

    outcomes = seed_catalog(store, [_entry()], image_dir=img_dir, n_scales=5)

    assert outcomes[0].ingested
    assert outcomes[0].wcs_source == "avm"
    # pixel scale came from the AVM WCS (0.05), not the catalog's declared 1.0
    assert store.get("heic0601a").fingerprint.pixel_scale_arcsec == pytest.approx(0.05, rel=1e-2)


def test_seeded_record_is_findable_by_cone_search(tmp_path):
    img_dir = tmp_path / "img"; img_dir.mkdir()
    _plain_jpg(img_dir / "heic0601a.jpg")
    store = FingerprintStore(tmp_path / "db.sqlite")
    seed_catalog(store, [_entry()], image_dir=img_dir, n_scales=5)

    matches = store.cone_search(RA, DEC, radius_arcmin=10.0, palette_class="RGB")
    assert [m.record.id for m in matches] == ["heic0601a"]


def test_missing_image_is_reported_not_raised(tmp_path):
    img_dir = tmp_path / "img"; img_dir.mkdir()  # nothing in it
    store = FingerprintStore(tmp_path / "db.sqlite")

    outcomes = seed_catalog(store, [_entry()], image_dir=img_dir, n_scales=5)

    assert outcomes[0].ingested is False
    assert "not found" in outcomes[0].detail.lower()


def test_tool_output_entry_is_rejected(tmp_path):
    """§5.3 autophagy guard survives the seeding path."""
    img_dir = tmp_path / "img"; img_dir.mkdir()
    _plain_jpg(img_dir / "heic0601a.jpg")
    store = FingerprintStore(tmp_path / "db.sqlite")

    outcomes = seed_catalog(store, [_entry(source_type="tool_output")],
                            image_dir=img_dir, n_scales=5)

    assert outcomes[0].ingested is False
    assert "tool_output" in outcomes[0].detail
    assert store.get("heic0601a") is None
