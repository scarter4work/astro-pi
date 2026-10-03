"""Tests for the flattening degradation and the reframed Phase 0 exit test.

The reframed criterion (what the Phase 2 optimizer actually needs): flattening a
render must move its fingerprint monotonically AWAY from a deep reference. If more
flattening ever reduces the distance, the fingerprint would point the optimizer
toward flatness — the opposite of its purpose.
"""

import numpy as np
import pytest

from autocontrast.eval.degrade import (
    DEFAULT_DEGRADATIONS,
    desaturate,
    flatten,
    flattening_monotonicity_report,
    oversaturate,
    over_sharpen,
    reference_is_local_minimum,
)
from autocontrast.fingerprint.extract import extract
from autocontrast.fingerprint.metrics import structure_to_gradient_ratio


def _structured(seed, size=96):
    rng = np.random.default_rng(seed)
    img = rng.uniform(0.0, 0.03, size=(size, size, 3))
    yy, xx = np.mgrid[0:size, 0:size]
    blob = np.exp(-(((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (2 * (size / 8) ** 2)))
    lane = np.exp(-(((xx - size * 0.4) ** 2) / (2 * (size / 20) ** 2)))
    for c, w in enumerate((0.6, 0.5, 0.3)):
        img[..., c] += 0.8 * w * blob
    img[..., 0] *= 1 - 0.4 * lane
    return np.clip(img, 0.0, 1.0)


def _fp(img):
    return extract(img, pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0,
                   palette_class="RGB")


def test_flatten_zero_is_identity():
    img = _structured(0)
    np.testing.assert_allclose(flatten(img, 0.0), img, atol=1e-9)


def test_flatten_reduces_structure():
    img = _structured(0)
    lum = img.mean(axis=-1)
    flat = flatten(img, 0.8).mean(axis=-1)
    assert structure_to_gradient_ratio(flat) < structure_to_gradient_ratio(lum)


def test_flatten_output_stays_in_unit_range():
    img = _structured(1)
    out = flatten(img, 1.0)
    assert out.min() >= 0.0 and out.max() <= 1.0


def test_flattening_moves_monotonically_away_from_reference():
    reference = _fp(_structured(100))
    images = {"a": _structured(1), "b": _structured(2), "c": _structured(3)}

    report = flattening_monotonicity_report(
        reference, images, strengths=[0.0, 0.25, 0.5, 0.75, 1.0],
        extract_kwargs=dict(pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0,
                            palette_class="RGB"),
    )

    assert report.all_monotonic
    for result in report.per_image.values():
        # distance at full flatten exceeds distance at no flatten
        assert result.distances[-1] > result.distances[0]


# ---- degradation transforms are identity at strength 0 --------------------


@pytest.mark.parametrize("fn", [over_sharpen, desaturate, oversaturate])
def test_degradation_identity_at_zero(fn):
    img = _structured(0)
    np.testing.assert_allclose(fn(img, 0.0), img, atol=1e-9)


@pytest.mark.parametrize("fn", [over_sharpen, desaturate, oversaturate])
def test_degradation_stays_in_unit_range(fn):
    out = fn(_structured(1), 1.0)
    assert out.min() >= 0.0 and out.max() <= 1.0


def test_desaturate_reduces_chroma():
    img = _structured(0)
    from autocontrast.fingerprint.color import rgb_to_lab
    c0 = np.hypot(*rgb_to_lab(img)[..., 1:].transpose(2, 0, 1)).mean()
    c1 = np.hypot(*rgb_to_lab(desaturate(img, 0.8))[..., 1:].transpose(2, 0, 1)).mean()
    assert c1 < c0


# ---- the real Phase 0 exit criterion --------------------------------------


def test_reference_is_a_strict_local_minimum_of_distance():
    """The reference must sit at the bottom of the distance valley: every known-bad
    degradation increases D monotonically from zero (§6.1 attractor property)."""
    reference = _structured(42)
    report = reference_is_local_minimum(
        reference,
        strengths=[0.0, 0.25, 0.5, 0.75, 1.0],
        degradations=DEFAULT_DEGRADATIONS,
        extract_kwargs=dict(pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0,
                            palette_class="RGB"),
    )

    assert report.is_local_minimum
    for axis in report.per_axis.values():
        assert axis.distances[0] == pytest.approx(0.0, abs=1e-9)  # identity => D=0
        assert axis.strictly_increasing
