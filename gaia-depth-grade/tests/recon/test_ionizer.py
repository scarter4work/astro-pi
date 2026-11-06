import numpy as np
from astropy.table import Table

from gaia_depth_grade.recon.ionizer import ionizer_from_xy, pick_ionizer
from gaia_depth_grade.wcs import FieldFootprint

SHAPE = (100, 200)  # (ny, nx)
FP = FieldFootprint(center_ra=83.8, center_dec=-5.4, radius_deg=0.5)


def _table(rows, bp_rp=True):
    t = Table()
    t["x"] = [r[0] for r in rows]
    t["y"] = [r[1] for r in rows]
    t["flux"] = [r[2] for r in rows]
    t["matched"] = [r[4] is not None for r in rows]
    t["r_med_geo"] = [r[4] if r[4] is not None else np.nan for r in rows]
    if bp_rp:
        t["bp_rp"] = [r[3] for r in rows]
    return t


def test_picks_blue_luminous_star():
    t = _table([(10, 10, 1000.0, 2.5, 500.0),    # red bright
                (50, 40, 800.0, -0.2, 1200.0),   # blue bright
                (90, 60, 50.0, -0.3, 900.0)])    # blue faint
    ion = pick_ionizer(t, SHAPE, FP)
    assert (ion.x, ion.y) == (50.0, 40.0)
    assert ion.distance_pc == 1200.0
    assert ion.source == "gaia-blue"


def test_falls_back_to_brightest_without_colour():
    t = _table([(10, 10, 100.0, np.nan, 500.0), (20, 30, 900.0, np.nan, 700.0)])
    ion = pick_ionizer(t, SHAPE, FP)
    assert (ion.x, ion.y) == (20.0, 30.0)
    assert ion.distance_pc == 700.0
    assert ion.source == "gaia-bright"


def test_falls_back_when_bp_rp_column_absent():
    t = _table([(10, 10, 100.0, 0.0, 500.0), (20, 30, 900.0, 0.0, 700.0)], bp_rp=False)
    assert "bp_rp" not in t.colnames
    ion = pick_ionizer(t, SHAPE, FP)
    assert (ion.x, ion.y) == (20.0, 30.0)
    assert ion.source == "gaia-bright"


def test_unmatched_pick_has_no_distance():
    t = _table([(20, 30, 900.0, np.nan, None), (10, 10, 100.0, np.nan, 500.0)])
    ion = pick_ionizer(t, SHAPE, FP)
    assert ion.distance_pc is None


def test_stars_outside_image_bounds_ignored():
    t = _table([(500, 10, 9999.0, -0.5, 100.0), (20, 30, 10.0, 0.5, 700.0)])
    ion = pick_ionizer(t, SHAPE, FP)
    assert (ion.x, ion.y) == (20.0, 30.0)


def test_empty_table_returns_field_center():
    ion = pick_ionizer(_table([]), SHAPE, FP)
    assert ion.source == "field-center"
    assert (ion.x, ion.y) == (100.0, 50.0)
    assert ion.distance_pc is None


def test_override_returns_specified_point():
    ion = ionizer_from_xy(12.5, 7.0, 1500.0)
    assert (ion.x, ion.y, ion.distance_pc, ion.source) == (12.5, 7.0, 1500.0, "override")
