import numpy as np
import pytest
from astropy.table import Table
from gaia_depth_grade.wcs import load_wcs
from gaia_depth_grade.match import cross_match, MatchStats


def _catalog_from_wcs(w, pixel_positions, dists):
    sky = w.pixel_to_world([p[0] for p in pixel_positions], [p[1] for p in pixel_positions])
    t = Table()
    t["ra"] = sky.ra.deg
    t["dec"] = sky.dec.deg
    t["r_med_geo"] = [d for d in dists]
    t["r_lo_geo"] = [d * 0.95 for d in dists]
    t["r_hi_geo"] = [d * 1.05 for d in dists]
    t["source_id"] = list(range(len(dists)))
    return t


def test_match_within_tolerance(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    detected = Table()
    detected["x"] = [150.0, 50.0]
    detected["y"] = [100.0, 60.0]
    detected["flux"] = [1.0, 0.5]
    detected["fwhm"] = [4.0, 4.0]
    # catalog: one ~1px from first detection, one far away
    catalog = _catalog_from_wcs(w, [(150.5, 100.2), (10.0, 10.0)], [120.0, 800.0])
    out, stats = cross_match(detected, catalog, w, tolerance_px=3.0)
    assert isinstance(stats, MatchStats)
    assert stats.n_detected == 2
    assert stats.n_matched == 1
    assert bool(out["matched"][0]) is True
    assert out["r_med_geo"][0] == pytest.approx(120.0)
    assert bool(out["matched"][1]) is False
    assert np.isnan(out["r_med_geo"][1])
    assert stats.match_rate == pytest.approx(0.5)


def test_no_catalog_zero_matches(simple_wcs_header):
    w = load_wcs(simple_wcs_header)
    detected = Table()
    detected["x"] = [150.0]; detected["y"] = [100.0]
    detected["flux"] = [1.0]; detected["fwhm"] = [4.0]
    catalog = Table(names=("ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id"))
    out, stats = cross_match(detected, catalog, w, tolerance_px=3.0)
    assert stats.n_matched == 0
    assert bool(out["matched"][0]) is False
