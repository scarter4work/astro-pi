"""Tests for the standalone FITS loader (§3.1 — must run with no PixInsight)."""

import numpy as np
import pytest
from astropy.io import fits
from astropy.wcs import WCS

from autocontrast.io.loaders import load_fits


def _write_fits(path, data, *, pixel_scale_arcsec=None, filter_name=None):
    """Write a minimal FITS file, optionally with a TAN WCS and FILTER keyword."""
    hdu = fits.PrimaryHDU(data=data)
    if pixel_scale_arcsec is not None:
        wcs = WCS(naxis=2)
        deg = pixel_scale_arcsec / 3600.0
        wcs.wcs.ctype = ["RA---TAN", "DEC--TAN"]
        wcs.wcs.crpix = [data.shape[-1] / 2, data.shape[-2] / 2]
        wcs.wcs.crval = [83.822, -5.391]
        wcs.wcs.cdelt = [-deg, deg]
        hdu.header.update(wcs.to_header())
    if filter_name is not None:
        hdu.header["FILTER"] = filter_name
    hdu.writeto(path)


def test_loads_pixel_data_as_float(tmp_path):
    path = tmp_path / "mono.fits"
    _write_fits(path, np.arange(64, dtype=np.uint16).reshape(8, 8))

    img = load_fits(path)

    assert img.data.dtype == np.float64
    assert img.data.shape == (8, 8)


def test_derives_pixel_scale_from_wcs(tmp_path):
    path = tmp_path / "wcs.fits"
    _write_fits(path, np.zeros((16, 16), dtype=np.float32), pixel_scale_arcsec=1.22)

    img = load_fits(path)

    assert img.pixel_scale_arcsec == pytest.approx(1.22, rel=1e-4)


def test_pixel_scale_is_none_without_wcs(tmp_path):
    path = tmp_path / "nowcs.fits"
    _write_fits(path, np.zeros((16, 16), dtype=np.float32))

    img = load_fits(path)

    assert img.pixel_scale_arcsec is None


def test_captures_filter_keyword(tmp_path):
    path = tmp_path / "ha.fits"
    _write_fits(path, np.zeros((8, 8), dtype=np.float32), filter_name="Ha")

    img = load_fits(path)

    assert img.filters == ["Ha"]


def test_no_filter_keyword_gives_empty_list(tmp_path):
    path = tmp_path / "plain.fits"
    _write_fits(path, np.zeros((8, 8), dtype=np.float32))

    img = load_fits(path)

    assert img.filters == []
