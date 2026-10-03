"""Tests for WCS acquisition — the §5.2 three-tier fallback.

    FITS header -> embedded AVM -> blind plate solve -> manual annotation

Tiers 1-2 run in the sidecar (pure Python). Tier 3 is an injected ``blind_solver``
callback: in deployment the PJSR orchestrator fills it with PixInsight's
ImageSolver; standalone it is None. Failing all tiers is an EXPECTED branch for
starless/mosaic renders, not an error state (§5.2) — it must surface, not raise.
"""

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS
from PIL import Image
from pyavm import AVM

from autocontrast.db.wcs import WcsResult, acquire_wcs

RA, DEC = 83.822, -5.391


def _wcs(nx, ny, scale_arcsec=1.22):
    w = WCS(naxis=2)
    w.wcs.ctype = ["RA---TAN", "DEC--TAN"]
    w.wcs.crpix = [nx / 2, ny / 2]
    w.wcs.crval = [RA, DEC]
    w.wcs.cdelt = [-scale_arcsec / 3600, scale_arcsec / 3600]
    w.pixel_shape = (nx, ny)
    return w


def _fits_with_wcs(path, nx=64, ny=48):
    hdu = fits.PrimaryHDU(data=np.zeros((ny, nx), dtype=np.float32))
    hdu.header.update(_wcs(nx, ny).to_header())
    hdu.writeto(path)


def _fits_without_wcs(path):
    fits.PrimaryHDU(data=np.zeros((16, 16), dtype=np.float32)).writeto(path)


def _plain_png(path, nx=64, ny=48):
    Image.fromarray(np.zeros((ny, nx, 3), dtype=np.uint8)).save(path)


def _avm_tagged_jpg(path, tmp_path, nx=64, ny=48):
    plain = tmp_path / "_plain.jpg"
    Image.fromarray(np.zeros((ny, nx, 3), dtype=np.uint8)).save(plain)
    AVM.from_wcs(_wcs(nx, ny), shape=(ny, nx)).embed(str(plain), str(path))


# ---- tier 1: FITS header --------------------------------------------------


def test_fits_header_wcs_is_tier_one(tmp_path):
    path = tmp_path / "w.fits"; _fits_with_wcs(path)

    result = acquire_wcs(path)

    assert result.solved
    assert result.wcs_source == "header"
    assert result.ra_deg == pytest.approx(RA, abs=1e-3)
    assert result.dec_deg == pytest.approx(DEC, abs=1e-3)
    assert result.fov_radius_arcmin > 0


def test_pixel_scale_is_derived_from_the_wcs_not_guessed(tmp_path):
    """§2.2: the fingerprint must carry its own on-sky scale. A real solve derives
    it; it is never a caller-supplied nominal value."""
    path = tmp_path / "w.fits"; _fits_with_wcs(path)  # fixture is 1.22 arcsec/px

    result = acquire_wcs(path)

    assert result.pixel_scale_arcsec == pytest.approx(1.22, rel=1e-3)


# ---- tier 2: embedded AVM -------------------------------------------------


def test_embedded_avm_is_tier_two(tmp_path):
    path = tmp_path / "tagged.jpg"; _avm_tagged_jpg(path, tmp_path)

    result = acquire_wcs(path)

    assert result.solved
    assert result.wcs_source == "avm"
    assert result.ra_deg == pytest.approx(RA, abs=1e-2)
    assert result.dec_deg == pytest.approx(DEC, abs=1e-2)
    assert result.fov_radius_arcmin > 0


# ---- tier 3: blind solver (ImageSolver in deployment) ---------------------


def test_blind_solver_used_when_header_and_avm_absent(tmp_path):
    path = tmp_path / "plain.png"; _plain_png(path)
    solved = WcsResult(ra_deg=RA, dec_deg=DEC, fov_radius_arcmin=21.0,
                       wcs_source="blind", solved=True, detail="ImageSolver")

    result = acquire_wcs(path, blind_solver=lambda p: solved)

    assert result.solved
    assert result.wcs_source == "blind"
    assert result.ra_deg == pytest.approx(RA)


def test_blind_solver_failure_falls_through(tmp_path):
    """A solver that cannot solve returns None — we continue to manual, not crash."""
    path = tmp_path / "plain.png"; _plain_png(path)

    result = acquire_wcs(path, blind_solver=lambda p: None,
                         manual={"ra_deg": RA, "dec_deg": DEC, "fov_radius_arcmin": 30.0})

    assert result.solved
    assert result.wcs_source == "manual"


# ---- tier 4 / failure: manual annotation is first-class -------------------


def test_manual_annotation_when_all_tiers_fail(tmp_path):
    path = tmp_path / "plain.png"; _plain_png(path)

    result = acquire_wcs(path, manual={"ra_deg": RA, "dec_deg": DEC,
                                       "fov_radius_arcmin": 21.0})

    assert result.solved
    assert result.wcs_source == "manual"
    assert result.fov_radius_arcmin == 21.0


def test_unsolved_is_reported_not_raised(tmp_path):
    """§5.2: failing all tiers is the EXPECTED outcome for a share of the prettiest
    renders (starless/mosaic). It must surface as a result, never an exception."""
    path = tmp_path / "plain.png"; _plain_png(path)

    result = acquire_wcs(path)

    assert result.solved is False
    assert result.wcs_source == "unsolved"
    assert result.ra_deg is None
    assert result.detail  # explains why, so the caller can prompt for annotation


def test_fits_without_wcs_is_unsolved(tmp_path):
    path = tmp_path / "nowcs.fits"; _fits_without_wcs(path)

    result = acquire_wcs(path)

    assert result.solved is False
    assert result.wcs_source == "unsolved"
    assert "no celestial WCS" in result.detail


def test_unreadable_avm_tag_is_reported_not_buried(tmp_path, monkeypatch):
    """A *malformed* AVM tag is a real error and must surface in `detail` — it must
    not be silently indistinguishable from 'no tag present' (§12)."""
    path = tmp_path / "broken.jpg"; _plain_png(path)  # content irrelevant; we fake the parse

    import autocontrast.db.wcs as wcs_mod
    from pyavm import AVM

    def _boom(_p):
        raise ValueError("corrupt XMP packet")

    monkeypatch.setattr(AVM, "from_image", staticmethod(_boom))

    result = wcs_mod.acquire_wcs(path)

    assert result.solved is False
    assert "unreadable" in result.detail
    assert "corrupt XMP packet" in result.detail


def test_large_mosaic_does_not_trip_pillow_decompression_guard(tmp_path, monkeypatch):
    """Professional mosaics are huge (the Hubble Orion mosaic is 18000x18000 = 324 Mpx)
    and must not be rejected by Pillow's decompression-bomb guard while we read their
    AVM WCS."""
    from PIL import Image as PILImage

    path = tmp_path / "big.jpg"
    _avm_tagged_jpg(path, tmp_path, nx=200, ny=200)
    # Force the guard to trip for a 200x200 image; acquire_wcs must still solve.
    monkeypatch.setattr(PILImage, "MAX_IMAGE_PIXELS", 100)

    result = acquire_wcs(path)

    assert result.solved
    assert result.wcs_source == "avm"


def _fits_cube_with_wcs(path, nx=64, ny=48, nchan=3, scale_arcsec=1.01):
    """A 3-channel, channel-first FITS cube with a celestial WCS — the shape a
    debayered PixInsight stack actually has (e.g. (3, 4042, 6072))."""
    data = np.zeros((nchan, ny, nx), dtype=np.uint16)
    hdu = fits.PrimaryHDU(data=data)
    hdu.header.update(_wcs(nx, ny, scale_arcsec).to_header())
    hdu.writeto(path)


def test_three_channel_fits_cube_with_wcs(tmp_path):
    """Regression (real data): a 3-axis cube must not (a) blow up WCS construction,
    nor (b) have its channel count mistaken for an image dimension."""
    path = tmp_path / "cube.fits"
    _fits_cube_with_wcs(path, nx=64, ny=48, nchan=3, scale_arcsec=1.01)

    result = acquire_wcs(path)

    assert result.solved
    assert result.wcs_source == "header"
    assert result.ra_deg == pytest.approx(RA, abs=1e-3)
    assert result.dec_deg == pytest.approx(DEC, abs=1e-3)
    assert result.pixel_scale_arcsec == pytest.approx(1.01, rel=1e-3)


def test_fits_cube_with_sip_distortion_does_not_raise(tmp_path):
    """A 3-axis header carrying SIP (as PixInsight writes) makes astropy's WCS()
    raise unless the celestial axes are selected explicitly."""
    path = tmp_path / "sip.fits"
    data = np.zeros((3, 48, 64), dtype=np.uint16)
    hdu = fits.PrimaryHDU(data=data)
    hdu.header.update(_wcs(64, 48).to_header())
    hdu.header["CTYPE1"] = "RA---TAN-SIP"
    hdu.header["CTYPE2"] = "DEC--TAN-SIP"
    hdu.header["A_ORDER"] = 2
    hdu.header["B_ORDER"] = 2
    hdu.writeto(path)

    result = acquire_wcs(path)  # must not raise

    assert result.solved
    assert result.wcs_source == "header"
