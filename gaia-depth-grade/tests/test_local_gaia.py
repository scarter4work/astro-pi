import numpy as np
import pytest
from gaia_depth_grade.local_gaia import (
    sigma_parallax_from_g, parallax_to_distance_table, LocalGaiaSource)
from gaia_depth_grade.wcs import FieldFootprint


def test_sigma_grows_faintward():
    s = sigma_parallax_from_g(np.array([10.0, 17.0, 20.0]))
    assert s[0] < s[1] < s[2]           # brighter stars -> tighter parallax
    assert s[0] == pytest.approx(0.02, abs=1e-6)


def test_distance_is_inverse_parallax():
    t = parallax_to_distance_table([10.0], [20.0], [2.0], [12.0], snr_min=5.0)
    assert t["r_med_geo"][0] == pytest.approx(500.0)   # 1000/2 mas = 500 pc
    assert t["r_lo_geo"][0] < 500.0 < t["r_hi_geo"][0]  # bracketed by +/- sigma


def test_low_snr_and_nonpositive_parallax_dropped():
    # G=20 -> sigma~0.30; parallax 0.1 -> snr 0.33 (<5) dropped; negative dropped;
    # bright high-parallax kept.
    t = parallax_to_distance_table(
        ra=[1, 2, 3, 4], dec=[1, 2, 3, 4],
        parallax_mas=[5.0, 0.1, -1.0, 3.0], g_mag=[10.0, 20.0, 12.0, 11.0], snr_min=5.0)
    assert len(t) == 2                                  # only the two good ones
    assert set(np.round(t["r_med_geo"], 1)) == {200.0, 333.3}


def test_widefield_low_snr_gives_lower_confidence():
    from gaia_depth_grade.transform import confidence
    bright = parallax_to_distance_table([0], [0], [5.0], [11.0])   # high S/N
    faint = parallax_to_distance_table([0], [0], [1.0], [18.0])    # lower S/N
    cb = confidence(bright["r_med_geo"], bright["r_lo_geo"], bright["r_hi_geo"])[0]
    cf = confidence(faint["r_med_geo"], faint["r_lo_geo"], faint["r_hi_geo"])[0]
    assert cb > cf                                       # tighter parallax -> more confident


def test_parse_lines_skips_header_and_comments(tmp_path):
    from gaia_depth_grade.local_gaia import _read_pi_gaia_tsv
    p = tmp_path / "gaia.tsv"
    p.write_text("# field export\nra\tdec\tparallax\tphot_g_mean_mag\n"
                 "323.7\t57.5\t2.5\t11.3\n323.8\t57.6\t1.0\t14.0\n")
    ra, dec, plx, g = _read_pi_gaia_tsv(str(p))
    assert len(ra) == 2
    assert ra[0] == pytest.approx(323.7) and plx[1] == pytest.approx(1.0)


def test_local_source_applies_mag_cut_and_schema(monkeypatch):
    src = LocalGaiaSource("ignored.tsv", mag_limit=18.0, snr_min=5.0)
    monkeypatch.setattr("gaia_depth_grade.local_gaia._read_pi_gaia_tsv",
                        lambda p: (np.array([10.0, 11.0]), np.array([20.0, 21.0]),
                                   np.array([4.0, 3.0]), np.array([12.0, 19.0])))
    # G=19 is < 18? no -> excluded by mag cut; only the G=12 star remains.
    t = src.distances_for(FieldFootprint(10.0, 20.0, 0.05))
    assert len(t) == 1
    assert set(t.colnames) == {"ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id"}
