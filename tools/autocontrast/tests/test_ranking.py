"""Tests for the offline ranking harness (§10, Phase 0 exit criterion)."""

import numpy as np

from autocontrast.eval.ranking import rank_against_reference, separation_report
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


def test_rank_is_sorted_ascending_by_distance():
    ref = _fp(_structured(0))
    candidates = {"a": _fp(_structured(1)), "b": _fp(_flat(2)), "c": _fp(_structured(3))}
    ranked = rank_against_reference(ref, candidates)

    distances = [entry.distance for entry in ranked]
    assert distances == sorted(distances)
    assert {entry.name for entry in ranked} == set(candidates)


def test_pros_separate_cleanly_from_amateurs():
    """The Phase 0 thesis: structured (pro-like) renders rank ahead of flat
    (amateur-like) ones, with a positive margin."""
    ref = _fp(_structured(100))
    pros = {f"pro{i}": _fp(_structured(i)) for i in range(1, 6)}
    amateurs = {f"am{i}": _fp(_flat(i)) for i in range(1, 6)}
    candidates = {**pros, **amateurs}

    ranked = rank_against_reference(ref, candidates)
    report = separation_report(ranked, pro_names=set(pros), amateur_names=set(amateurs))

    assert report.cleanly_separated
    assert report.margin > 0
    # Every professional outranks every amateur.
    top_five = [entry.name for entry in ranked[:5]]
    assert set(top_five) == set(pros)
