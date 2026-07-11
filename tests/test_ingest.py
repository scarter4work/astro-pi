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


# ---- auto-WCS ingest (§5.2 three-tier) ------------------------------------


def _auto(store, path, **overrides):
    from autocontrast.db.ingest import ingest_reference_auto
    kwargs = dict(psf_fwhm_arcsec=2.0, palette_class="RGB",
                  source_type="professional_render", provenance=dict(_PROV))
    kwargs.update(overrides)
    return ingest_reference_auto(store, path, **kwargs)


def test_auto_ingest_uses_fits_wcs_and_derived_pixel_scale(tmp_path):
    """A solved WCS supplies position AND pixel scale (§2.2)."""
    from astropy.io import fits
    from astropy.wcs import WCS

    nx, ny = 64, 64
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crpix = [nx / 2, ny / 2]
    w.wcs.crval = [83.822, -5.391]
    w.wcs.cdelt = [-1.22 / 3600, 1.22 / 3600]
    path = tmp_path / "ref.fits"
    hdu = fits.PrimaryHDU(data=np.random.default_rng(0).random((ny, nx)).astype(np.float32))
    hdu.header.update(w.to_header())
    hdu.writeto(path)

    store = FingerprintStore(tmp_path / "db.sqlite")
    outcome = _auto(store, path, id="auto-1", n_scales=5)

    assert outcome.ingested
    assert outcome.wcs.wcs_source == "header"
    assert outcome.record.fingerprint.pixel_scale_arcsec == pytest.approx(1.22, rel=1e-3)
    assert outcome.record.provenance["wcs_source"] == "header"


def test_auto_ingest_skips_unsolved_reference_without_raising(tmp_path):
    """§5.2: an unsolvable render is skipped with a reason — not an exception, and
    never stored with a bogus position."""
    png = tmp_path / "plain.png"; _write_png(png)
    store = FingerprintStore(tmp_path / "db.sqlite")

    outcome = _auto(store, png, id="auto-2")

    assert outcome.ingested is False
    assert outcome.record is None
    assert outcome.wcs.wcs_source == "unsolved"
    assert outcome.detail
    assert store.get("auto-2") is None  # nothing persisted


def test_auto_ingest_accepts_manual_annotation(tmp_path):
    png = tmp_path / "plain.png"; _write_png(png)
    store = FingerprintStore(tmp_path / "db.sqlite")

    outcome = _auto(store, png, id="auto-3",
                    manual={"ra_deg": 83.822, "dec_deg": -5.391,
                            "fov_radius_arcmin": 21.0, "pixel_scale_arcsec": 1.22})

    assert outcome.ingested
    assert outcome.wcs.wcs_source == "manual"
    assert store.get("auto-3").provenance["wcs_source"] == "manual"


def test_pixel_scale_is_corrected_for_downsampling(tmp_path):
    """§2.2 regression: a WCS gives the ORIGINAL file's pixel scale. When we
    fingerprint a downsampled copy, the stored scale must describe the array we
    actually measured — otherwise band-limiting (§4.4) maps wavelet layers to
    angular scales that are wrong by the downsample factor.

    The Hubble Orion mosaic is 18000px at 0.1"/px; fingerprinted at 1200px its
    effective scale is 15x coarser (~1.5"/px), NOT 0.1"/px.
    """
    from astropy.wcs import WCS
    from pyavm import AVM
    from PIL import Image as PILImage

    nx = ny = 200
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crpix = [nx / 2, ny / 2]
    w.wcs.crval = [83.822, -5.391]
    w.wcs.cdelt = [-0.10 / 3600, 0.10 / 3600]   # 0.10 arcsec/px at native 200px
    w.pixel_shape = (nx, ny)

    plain = tmp_path / "_p.jpg"
    rng = np.random.default_rng(0)
    PILImage.fromarray((rng.uniform(0, 0.5, (ny, nx, 3)) * 255).astype(np.uint8)).save(plain)
    tagged = tmp_path / "big.jpg"
    AVM.from_wcs(w, shape=(ny, nx)).embed(str(plain), str(tagged))

    store = FingerprintStore(tmp_path / "db.sqlite")
    outcome = _auto(store, tagged, id="ds-1", n_scales=5, max_dim=100)  # 2x downsample

    assert outcome.ingested
    assert outcome.wcs.wcs_source == "avm"
    # native 0.10"/px, fingerprinted at half size => effective 0.20"/px
    assert outcome.record.fingerprint.pixel_scale_arcsec == pytest.approx(0.20, rel=1e-2)
