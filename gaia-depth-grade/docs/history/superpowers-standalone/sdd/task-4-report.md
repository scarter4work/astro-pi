# Task 4 Report: Distance Source Interface, ADQL, Gaia/Bailer-Jones Query with Cache

## What Was Implemented

Created `src/gaia_depth_grade/distances.py` with:

- `DistanceSource` — abstract base class with `distances_for(footprint: FieldFootprint) -> Table`
- `build_adql(footprint)` — produces a CONTAINS/CIRCLE cone-search ADQL joining `gaiadr3.gaia_source` to `gaiadr3.distances` (Bailer-Jones geometric distances). Never uses `1/parallax`.
- `GaiaStarSource(cache_dir)` — concrete implementation that:
  - Computes a SHA-1 cache key from `(ra, dec, radius)` and reads/writes ECSV files under `cache_dir`
  - Logs `"using cached Gaia result <path>"` at WARNING level on cache hit
  - Calls `_run_query(adql)` which lazily imports `astroquery.gaia` (inside the method body, not at module top) so tests never trigger network setup
  - Raises `RuntimeError` on TAP failure, missing columns, or zero-row result — no silent fallbacks

Created `tests/test_distances.py` with 4 tests matching the brief verbatim.

## TDD RED/GREEN Evidence

### RED (before implementation)

```
pytest tests/test_distances.py -v
...
ERROR tests/test_distances.py -- ModuleNotFoundError: No module named 'gaia_depth_grade.distances'
Exit code 2
```

### GREEN (after implementation)

```
pytest tests/test_distances.py -v
tests/test_distances.py::test_build_adql_contains_join_and_cone PASSED   [ 25%]
tests/test_distances.py::test_gaiastarsource_caches PASSED               [ 50%]
tests/test_distances.py::test_empty_query_raises PASSED                  [ 75%]
tests/test_distances.py::test_is_distance_source PASSED                  [100%]
4 passed in 0.01s
```

### Full Suite

```
pytest -v
13 passed in 0.30s
```
All 13 tests across tasks 1-4 pass; no regressions.

## Files Changed

- `src/gaia_depth_grade/distances.py` — created (60 lines)
- `tests/test_distances.py` — created (60 lines)

## Self-Review Findings

- Implementation is verbatim from the brief; no extras added.
- The `astroquery.gaia` import is correctly deferred inside `_run_query`, so monkeypatching `src._run_query` in tests bypasses it entirely.
- Cache key uses 6 decimal places of precision for RA/Dec/radius — sufficient for uniqueness at arcsecond granularity.
- Column ordering in `tbl[list(_REQUIRED)]` normalizes the output regardless of what the TAP server returns.
- `os.makedirs(cache_dir, exist_ok=True)` in `__init__` means `tmp_path` (which already exists) works fine.

## Concerns

None. Implementation matches brief exactly, all tests pass, no regressions.
