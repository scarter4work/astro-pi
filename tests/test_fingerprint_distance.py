"""Tests for the full fingerprint-to-fingerprint distance (§4.5).

Includes a miniature version of the Phase 0 exit criterion: a structured image
must land closer to a structured reference than a flat image does.
"""

import numpy as np
import pytest

from autocontrast.fingerprint.distance import fingerprint_distance
from autocontrast.fingerprint.extract import extract


def _blobby(seed, *, contrast, size=96):
    """A dark field with a central blob whose amplitude sets how '3D' it looks."""
    rng = np.random.default_rng(seed)
    img = rng.uniform(0.0, 0.03, size=(size, size, 3))
    yy, xx = np.mgrid[0:size, 0:size]
    blob = np.exp(-(((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (2 * (size / 8) ** 2)))
    for c, w in enumerate((0.6, 0.5, 0.3)):
        img[..., c] += contrast * w * blob
    return np.clip(img, 0.0, 1.0)


def _flat(seed, size=96):
    rng = np.random.default_rng(seed)
    return np.clip(rng.uniform(0.10, 0.16, size=(size, size, 3)), 0.0, 1.0)


def _fp(img, palette="RGB", pixel_scale=1.22, psf=2.5):
    return extract(img, pixel_scale_arcsec=pixel_scale, n_scales=6,
                   psf_fwhm_arcsec=psf, palette_class=palette)


def test_self_distance_is_zero():
    fp = _fp(_blobby(0, contrast=0.8))
    assert fingerprint_distance(fp, fp) == pytest.approx(0.0, abs=1e-9)


def test_distance_is_nonnegative():
    ref = _fp(_blobby(0, contrast=0.8))
    other = _fp(_blobby(1, contrast=0.5))
    assert fingerprint_distance(ref, other) >= 0


def test_structured_image_is_closer_to_reference_than_flat_image():
    """The core Phase 0 thesis in miniature."""
    reference = _fp(_blobby(0, contrast=0.9))
    structured = _fp(_blobby(1, contrast=0.8))
    flat = _fp(_flat(2))

    d_structured = fingerprint_distance(reference, structured)
    d_flat = fingerprint_distance(reference, flat)
    assert d_structured < d_flat


def test_non_overlapping_bands_still_return_finite_distance():
    """When the spectrum component is unusable (§4.4) the distance falls back to
    tonal/chroma/background instead of blowing up."""
    ref = _fp(_blobby(0, contrast=0.8), pixel_scale=0.05, psf=0.1)   # HST-like
    target = _fp(_blobby(0, contrast=0.8), pixel_scale=1.22, psf=8.0)  # coarse, huge PSF
    d = fingerprint_distance(ref, target)
    assert np.isfinite(d)


def test_palette_mismatch_does_not_raise_and_stays_finite():
    ref = _fp(_blobby(0, contrast=0.8), palette="SHO")
    target = _fp(_blobby(0, contrast=0.8), palette="RGB")
    d = fingerprint_distance(ref, target)
    assert np.isfinite(d)
