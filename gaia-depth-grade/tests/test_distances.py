import numpy as np
import pytest
from astropy.table import Table
from gaia_depth_grade.wcs import FieldFootprint
from gaia_depth_grade.distances import build_adql, GaiaStarSource, DistanceSource


def _fake_catalog():
    t = Table()
    t["ra"] = [10.0, 10.01]
    t["dec"] = [20.0, 20.01]
    t["r_med_geo"] = [100.0, 500.0]
    t["r_lo_geo"] = [95.0, 400.0]
    t["r_hi_geo"] = [105.0, 650.0]
    t["source_id"] = [1, 2]
    return t


def test_build_adql_contains_join_and_cone():
    fp = FieldFootprint(10.0, 20.0, 0.05)
    q = build_adql(fp, 1900).lower()
    assert "external.gaiaedr3_distance" in q  # Bailer-Jones (2021); gaiadr3.distances does not exist
    assert "r_med_geo" in q
    assert "1/parallax" not in q
    assert "circle" in q and "10.0" in q and "20.0" in q
    assert "top 1900" in q                       # page bounded under the sync cap
    assert "order by g.phot_g_mean_mag asc" in q  # brightest-first for keyset paging


def test_build_adql_keyset_cursor():
    fp = FieldFootprint(10.0, 20.0, 0.05)
    assert "phot_g_mean_mag >" not in build_adql(fp, 1900)            # first page: no cursor
    assert "g.phot_g_mean_mag > 17.5" in build_adql(fp, 1900, 17.5)   # later page advances cursor


def test_build_adql_magnitude_cut():
    fp = FieldFootprint(10.0, 20.0, 0.05)
    assert "phot_g_mean_mag <" not in build_adql(fp, 1900)                  # no cut by default
    assert "g.phot_g_mean_mag < 18.0" in build_adql(fp, 1900, mag_lt=18.0)  # upper bound applied


def test_build_adql_cursor_and_cut_coexist():
    fp = FieldFootprint(10.0, 20.0, 0.05)
    q = build_adql(fp, 1900, mag_gt=15.0, mag_lt=18.0)
    assert "g.phot_g_mean_mag > 15.0" in q  # moving lower-bound cursor
    assert "g.phot_g_mean_mag < 18.0" in q  # fixed upper-bound cut


def test_mag_limit_changes_cache_path():
    # A different cut is a different result set; the cache must not collide.
    fp = FieldFootprint(10.0, 20.0, 0.05)
    a = GaiaStarSource(cache_dir="/tmp", mag_limit=18.0)._cache_path(fp)
    b = GaiaStarSource(cache_dir="/tmp", mag_limit=17.0)._cache_path(fp)
    none = GaiaStarSource(cache_dir="/tmp", mag_limit=None)._cache_path(fp)
    assert a != b != none and a != none


def test_mag_limit_threads_into_query(tmp_path, monkeypatch):
    captured = {}
    src = GaiaStarSource(cache_dir=str(tmp_path), mag_limit=18.0)

    def fake_page(adql):
        captured["adql"] = adql
        return _fake_catalog()  # short page (< _PAGE) → pagination stops after one call

    monkeypatch.setattr(src, "_sync_page", fake_page)
    src.distances_for(FieldFootprint(10.0, 20.0, 0.05))
    assert "g.phot_g_mean_mag < 18.0" in captured["adql"]


def test_gaiastarsource_caches(tmp_path, monkeypatch):
    src = GaiaStarSource(cache_dir=str(tmp_path))
    calls = {"n": 0}

    def fake_run(adql):
        calls["n"] += 1
        return _fake_catalog()

    monkeypatch.setattr(src, "_run_query", fake_run)
    fp = FieldFootprint(10.0, 20.0, 0.05)
    t1 = src.distances_for(fp)
    t2 = src.distances_for(fp)  # second call must hit cache, not _run_query
    assert calls["n"] == 1
    assert len(t1) == len(t2) == 2
    assert set(t1.colnames) >= {"ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id"}


def test_empty_query_raises(tmp_path, monkeypatch):
    src = GaiaStarSource(cache_dir=str(tmp_path))
    monkeypatch.setattr(src, "_run_query", lambda adql: Table(
        names=("ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id")))
    with pytest.raises(RuntimeError):
        src.distances_for(FieldFootprint(10.0, 20.0, 0.05))


def test_tap_failure_raises_runtimeerror(tmp_path, monkeypatch):
    src = GaiaStarSource(cache_dir=str(tmp_path))
    def boom(adql):
        raise ConnectionError("network down")
    monkeypatch.setattr(src, "_run_query", boom)
    with pytest.raises(RuntimeError) as exc:
        src.distances_for(FieldFootprint(10.0, 20.0, 0.05))
    assert "network down" in str(exc.value)


def test_is_distance_source():
    assert issubclass(GaiaStarSource, DistanceSource)
