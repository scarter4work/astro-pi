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


def build_adql(footprint: FieldFootprint, top: int,
               mag_gt: float | None = None, mag_lt: float | None = None) -> str:
    ra, dec, r = footprint.center_ra, footprint.center_dec, footprint.radius_deg
    # `top` keeps each page within the sync endpoint's server-side cap. Two
    # constraints share phot_g_mean_mag: mag_gt is the keyset cursor (a moving
    # lower bound — see GaiaStarSource._run_query — so pages walk brightest-first
    # without the unreliable async API), and mag_lt is the fixed magnitude cut
    # (an upper bound that drops faint, distance-unreliable sources up front).
    mag_clause = f"AND g.phot_g_mean_mag > {mag_gt} " if mag_gt is not None else ""
    if mag_lt is not None:
        mag_clause += f"AND g.phot_g_mean_mag < {mag_lt} "
    return (
        f"SELECT TOP {top} "
        "g.ra, g.dec, d.r_med_geo, d.r_lo_geo, d.r_hi_geo, g.source_id, g.phot_g_mean_mag "
        "FROM gaiadr3.gaia_source AS g "
        # Bailer-Jones (2021) geometric distances: published as external.gaiaedr3_distance
        # on the ESA Gaia archive (keyed by DR3 source_id). There is no gaiadr3.distances.
        "JOIN external.gaiaedr3_distance AS d ON g.source_id = d.source_id "
        "WHERE 1 = CONTAINS(POINT('ICRS', g.ra, g.dec), "
        f"CIRCLE('ICRS', {ra}, {dec}, {r})) "
        "AND d.r_med_geo IS NOT NULL "
        f"AND g.phot_g_mean_mag IS NOT NULL {mag_clause}"
        "ORDER BY g.phot_g_mean_mag ASC"
    )


class GaiaStarSource(DistanceSource):
    def __init__(self, cache_dir: str, mag_limit: float | None = None):
        self.cache_dir = cache_dir
        self.mag_limit = mag_limit
        os.makedirs(cache_dir, exist_ok=True)

    def _cache_path(self, footprint: FieldFootprint) -> str:
        # mag_limit is part of the cache identity: a different cut is a different
        # result set, so changing it must NOT silently return a stale file.
        key = (f"{footprint.center_ra:.6f}_{footprint.center_dec:.6f}"
               f"_{footprint.radius_deg:.6f}_mag{self.mag_limit}")
        digest = hashlib.sha1(key.encode(), usedforsecurity=False).hexdigest()[:16]
        return os.path.join(self.cache_dir, f"gaia_{digest}.ecsv")

    # Page size, kept under the anonymous synchronous-TAP server cap (~2000 rows).
    _PAGE = 1900
    # Safety bound on pages so a pathologically dense field can't loop forever.
    _MAX_PAGES = 200

    def _sync_page(self, adql: str) -> Table:
        from astroquery.gaia import Gaia

        # SYNCHRONOUS endpoint only: launch_job_async intermittently returns HTTP
        # 500 "Cannot find result" (the server loses the async job's output under
        # load/maintenance) and is slow even when it works. The sync endpoint
        # returns results inline and is reliable. Retry to ride out transient hiccups.
        last: Exception | None = None
        for attempt in range(1, 4):
            try:
                return Gaia.launch_job(adql).get_results()
            except Exception as exc:  # noqa: BLE001 - retry then surface the real error
                last = exc
                log.warning("Gaia sync query attempt %d/3 failed: %s", attempt, exc)
        raise RuntimeError(f"Gaia TAP query failed after 3 attempts: {last}") from last

    def _run_query(self, footprint: FieldFootprint) -> Table:
        # Keyset pagination by magnitude: the sync endpoint caps each response, so
        # we walk the catalogue brightest-first, advancing the cursor to the last
        # page's faintest magnitude, until a short page signals exhaustion. This
        # gets full-field coverage from the reliable endpoint (a dense field that
        # used to need the unlimited async query now just takes a few more pages).
        # LOUD about the cut (no silent cap): announce the policy up front. We
        # can't count what we deliberately never fetch, so the honest signal is
        # to state the limit in effect, not a post-hoc "excluded N" tally.
        if self.mag_limit is not None:
            log.warning("Gaia: magnitude cut G<%.1f in effect — fainter sources "
                        "excluded by design (Bailer-Jones distances unreliable "
                        "faintward). Set gaia_mag_limit=None to include them.",
                        self.mag_limit)
        pages: list[Table] = []
        seen: set[int] = set()
        cursor: float | None = None
        for _ in range(self._MAX_PAGES):
            page = self._sync_page(
                build_adql(footprint, self._PAGE, cursor, self.mag_limit))
            fresh = [r for r in page if int(r["source_id"]) not in seen]
            for r in fresh:
                seen.add(int(r["source_id"]))
            if fresh:
                pages.append(Table(rows=fresh, names=page.colnames))
            if len(page) < self._PAGE or not fresh:
                break
            cursor = float(max(page["phot_g_mean_mag"]))
        else:
            log.warning("Gaia pagination hit the %d-page cap; field may be undersampled.",
                        self._MAX_PAGES)
        if not pages:
            return Table(names=("ra", "dec", "r_med_geo", "r_lo_geo", "r_hi_geo",
                                "source_id", "phot_g_mean_mag"))
        from astropy.table import vstack
        combined = vstack(pages)
        log.info("Gaia: %d sources over %d page(s)", len(combined), len(pages))
        return combined

    def distances_for(self, footprint: FieldFootprint) -> Table:
        path = self._cache_path(footprint)
        if os.path.exists(path):
            log.warning("using cached Gaia result %s", path)
            return Table.read(path, format="ascii.ecsv")
        try:
            tbl = self._run_query(footprint)
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
