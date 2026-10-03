"""Tests for the top-level fingerprint schema + extract() (§4.1)."""

import json

import numpy as np
import pytest

from autocontrast.fingerprint.extract import extract
from autocontrast.fingerprint.metrics import QUANTILE_LEVELS


def _synthetic_rgb(seed=0, size=96):
    rng = np.random.default_rng(seed)
    # A dark background with a bright, slightly colored central blob.
    img = rng.uniform(0.0, 0.05, size=(size, size, 3))
    yy, xx = np.mgrid[0:size, 0:size]
    blob = np.exp(-(((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (2 * (size / 8) ** 2)))
    img[..., 0] += 0.6 * blob
    img[..., 1] += 0.5 * blob
    img[..., 2] += 0.3 * blob
    return np.clip(img, 0.0, 1.0)


def test_extract_produces_all_components():
    fp = extract(_synthetic_rgb(), pixel_scale_arcsec=1.22, n_scales=6,
                 psf_fwhm_arcsec=2.5, palette_class="RGB")

    assert fp.energy.centers_arcsec.shape == (6,)
    assert fp.energy.energy.shape == (6,)
    assert fp.tonal_quantiles.shape == (len(QUANTILE_LEVELS),)
    assert fp.saturation_quantiles.shape == (len(QUANTILE_LEVELS),)
    assert fp.chroma_hist.shape == (16, 16)
    assert set(fp.background) == {
        "floor_to_peak_ratio", "structure_to_gradient_ratio", "channel_balance"
    }


def test_extract_records_metadata():
    fp = extract(_synthetic_rgb(), pixel_scale_arcsec=1.22, n_scales=6,
                 psf_fwhm_arcsec=2.5, palette_class="RGB")
    assert fp.pixel_scale_arcsec == 1.22
    assert fp.psf_fwhm_arcsec == 2.5
    assert fp.palette_class == "RGB"


def test_fingerprint_is_json_serializable():
    fp = extract(_synthetic_rgb(), pixel_scale_arcsec=1.22, n_scales=6,
                 psf_fwhm_arcsec=2.5, palette_class="RGB")
    blob = json.dumps(fp.to_dict())
    assert isinstance(blob, str)
    # No numpy types leaked through — pure JSON round-trips.
    assert json.loads(blob)["palette_class"] == "RGB"


def test_fingerprint_round_trips_through_dict():
    fp = extract(_synthetic_rgb(), pixel_scale_arcsec=1.22, n_scales=6,
                 psf_fwhm_arcsec=2.5, palette_class="RGB")
    from autocontrast.fingerprint.extract import FingerprintData

    restored = FingerprintData.from_dict(fp.to_dict())
    np.testing.assert_allclose(restored.tonal_quantiles, fp.tonal_quantiles)
    np.testing.assert_allclose(restored.energy.energy, fp.energy.energy)
    np.testing.assert_allclose(restored.chroma_hist, fp.chroma_hist)
    assert restored.palette_class == fp.palette_class


def test_rejects_non_rgb_input():
    with pytest.raises(ValueError):
        extract(np.zeros((32, 32)), pixel_scale_arcsec=1.0, n_scales=4,
                psf_fwhm_arcsec=2.0, palette_class="RGB")
