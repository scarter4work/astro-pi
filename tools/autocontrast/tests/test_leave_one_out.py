"""Tests for leave-one-out separation — the robust Phase 0 exit-criterion metric."""

import numpy as np

from autocontrast.eval.ranking import leave_one_out_report
from autocontrast.fingerprint.extract import extract


def _structured(seed, size=96):
    rng = np.random.default_rng(seed)
    img = rng.uniform(0.0, 0.03, size=(size, size, 3))
    yy, xx = np.mgrid[0:size, 0:size]
    blob = np.exp(-(((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (2 * (size / 8) ** 2)))
    for c, w in enumerate((0.6, 0.5, 0.3)):
        img[..., c] += 0.8 * w * blob
    return np.clip(img, 0.0, 1.0)


def _flat(seed, size=96):
    rng = np.random.default_rng(seed)
    return np.clip(rng.uniform(0.10, 0.16, size=(size, size, 3)), 0.0, 1.0)


def _fp(img):
    return extract(img, pixel_scale_arcsec=1.0, n_scales=6, psf_fwhm_arcsec=2.0,
                   palette_class="RGB")


def test_all_pros_as_reference_separate_from_amateurs():
    pros = {f"pro{i}": _fp(_structured(i)) for i in range(1, 6)}
    amateurs = {f"am{i}": _fp(_flat(i)) for i in range(1, 6)}

    report = leave_one_out_report(pros, amateurs)

    assert report.all_clean
    assert report.min_margin > 0
    assert len(report.per_reference) == len(pros)
    assert all(r.cleanly_separated for r in report.per_reference.values())
