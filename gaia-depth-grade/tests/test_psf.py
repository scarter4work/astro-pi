import logging

import numpy as np
from astropy.table import Table
from gaia_depth_grade.config import GradeConfig
from gaia_depth_grade.psf import build_halo_profile, moffat_profile


def _moffat_star(h, w, cx, cy, alpha, beta, peak):
    yy, xx = np.mgrid[0:h, 0:w]
    r2 = (xx - cx) ** 2 + (yy - cy) ** 2
    return peak * (1.0 + r2 / alpha**2) ** (-beta)


def _frame_with_refs(n, peak=0.6, spacing=40, alpha=3.0, beta=2.5):
    """A frame of well-separated, bright, unsaturated Moffat stars."""
    cols = 4
    ny = nx = spacing * (max(n, 1) // cols + 2)
    img = np.zeros((ny, nx), dtype=float)
    xs, ys = [], []
    for i in range(n):
        cx = spacing * (1 + i % cols)
        cy = spacing * (1 + i // cols)
        img += _moffat_star(ny, nx, cx, cy, alpha, beta, peak)
        xs.append(float(cx)); ys.append(float(cy))
    det = Table()
    det["x"] = xs; det["y"] = ys
    det["flux"] = [peak] * n
    det["fwhm"] = [2.0 * alpha * np.sqrt(2 ** (1 / beta) - 1)] * n
    return img, det


def test_empirical_profile_has_heavier_wings_than_gaussian():
    cfg = GradeConfig()
    img, det = _frame_with_refs(12, beta=2.0)  # heavy-winged input
    prof = build_halo_profile(img, det, base_sigma_px=2.0, cfg=cfg)
    assert prof.is_empirical
    # normalized at the centre
    assert prof.sample(np.array([0.0]))[0] == 1.0
    # at several sigma out, a Moffat-derived profile must exceed a matched Gaussian
    r = np.array([6.0])
    gauss = np.exp(-(r**2) / (2 * 2.0**2))
    assert prof.sample(r)[0] > gauss[0]
    # monotonically decreasing outward
    rr = np.linspace(0, 12, 13)
    vals = prof.sample(rr)
    assert np.all(np.diff(vals) <= 1e-6)


def test_too_few_refs_falls_back_to_moffat_and_warns(caplog):
    cfg = GradeConfig()  # halo_min_refs default 8
    img, det = _frame_with_refs(3)
    with caplog.at_level(logging.WARNING):
        prof = build_halo_profile(img, det, base_sigma_px=2.0, cfg=cfg)
    assert prof.is_empirical is False
    assert any("moffat" in r.message.lower() for r in caplog.records)
    assert prof.sample(np.array([0.0]))[0] == 1.0


def test_saturated_stars_are_rejected_as_references():
    cfg = GradeConfig()
    img, det = _frame_with_refs(12, peak=0.99)  # all clipped/saturated
    prof = build_halo_profile(img, det, base_sigma_px=2.0, cfg=cfg)
    assert prof.is_empirical is False  # none usable -> fallback


def test_moffat_profile_normalized_and_decreasing():
    prof = moffat_profile(base_sigma_px=2.0, beta=2.5, radius_px=16.0)
    assert prof.is_empirical is False
    assert prof.sample(np.array([0.0]))[0] == 1.0
    assert prof.sample(np.array([4.0]))[0] > prof.sample(np.array([10.0]))[0]
    assert prof.sample(np.array([100.0]))[0] == 0.0  # zero beyond max radius
