import numpy as np
from astropy.table import Table

from gaia_depth_grade.cache import load_prep, save_prep


def _table():
    t = Table()
    t["x"] = [1.0, 2.5, 9.0]
    t["y"] = [4.0, 5.0, 6.0]
    t["flux"] = [0.1, 0.2, 0.3]
    t["r_med_geo"] = [100.0, np.nan, 2000.0]
    t["r_lo_geo"] = [98.0, np.nan, 1900.0]
    t["r_hi_geo"] = [102.0, np.nan, 2100.0]
    return t


def test_roundtrip(tmp_path):
    qa = {"n_detected": 3, "n_matched": 2, "match_rate": 0.6667,
          "median_offset_px": 1.2, "low_match_warning": False}
    save_prep(str(tmp_path), _table(), qa)
    table, qa2 = load_prep(str(tmp_path))
    for col in ("x", "y", "flux", "r_med_geo", "r_lo_geo", "r_hi_geo"):
        a = np.asarray(table[col], dtype=float)
        b = np.asarray(_table()[col], dtype=float)
        assert np.allclose(a, b, equal_nan=True)
    assert qa2 == qa
