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
    q = build_adql(fp).lower()
    assert "external.gaiaedr3_distance" in q  # Bailer-Jones (2021); gaiadr3.distances does not exist
    assert "r_med_geo" in q
    assert "1/parallax" not in q
    assert "circle" in q and "10.0" in q and "20.0" in q


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
