from __future__ import annotations

import abc
import hashlib
import logging
import os

from astropy.table import Table

from .wcs import FieldFootprint

log = logging.getLogger(__name__)

_REQUIRED = ("ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo", "source_id")


class DistanceSource(abc.ABC):
    @abc.abstractmethod
    def distances_for(self, footprint: FieldFootprint) -> Table: ...


def build_adql(footprint: FieldFootprint) -> str:
    ra, dec, r = footprint.center_ra, footprint.center_dec, footprint.radius_deg
    return (
        "SELECT g.ra, g.dec, d.r_med_geo, d.r_lo_geo, d.r_hi_geo, g.source_id "
        "FROM gaiadr3.gaia_source AS g "
        "JOIN gaiadr3.distances AS d ON g.source_id = d.source_id "
        "WHERE 1 = CONTAINS(POINT('ICRS', g.ra, g.dec), "
        f"CIRCLE('ICRS', {ra}, {dec}, {r})) "
        "AND d.r_med_geo IS NOT NULL"
    )


class GaiaStarSource(DistanceSource):
    def __init__(self, cache_dir: str):
        self.cache_dir = cache_dir
        os.makedirs(cache_dir, exist_ok=True)

    def _cache_path(self, footprint: FieldFootprint) -> str:
        key = f"{footprint.center_ra:.6f}_{footprint.center_dec:.6f}_{footprint.radius_deg:.6f}"
        digest = hashlib.sha1(key.encode()).hexdigest()[:16]
        return os.path.join(self.cache_dir, f"gaia_{digest}.ecsv")

    def _run_query(self, adql: str) -> Table:
        from astroquery.gaia import Gaia

        job = Gaia.launch_job_async(adql)
        return job.get_results()

    def distances_for(self, footprint: FieldFootprint) -> Table:
        path = self._cache_path(footprint)
        if os.path.exists(path):
            log.warning("using cached Gaia result %s", path)
            return Table.read(path, format="ascii.ecsv")
        try:
            tbl = self._run_query(build_adql(footprint))
        except Exception as exc:  # surface the real failure, never mock
            raise RuntimeError(f"Gaia TAP query failed: {exc}") from exc
        missing = set(_REQUIRED) - set(tbl.colnames)
        if missing:
            raise RuntimeError(f"Gaia result missing columns: {sorted(missing)}")
        if len(tbl) == 0:
            raise RuntimeError("Gaia query returned zero rows for this field")
        tbl = tbl[list(_REQUIRED)]
        tbl.write(path, format="ascii.ecsv", overwrite=True)
        return tbl
