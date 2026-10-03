# Task 1 Report: Project Scaffolding, Env, and Config

## What Was Implemented

- Created Python 3.14 virtualenv at `.venv` using `uv venv --python 3.14 .venv`
- Installed all dependencies: numpy 2.5.0, scipy 1.18.0, astropy 8.0.0, photutils 3.0.0, astroquery 0.4.11, pytest 9.1.1 — all succeeded with Python 3.14 wheels (no fallback needed)
- Created `pyproject.toml` with setuptools build backend, `src/` layout, and pytest config
- Created `src/gaia_depth_grade/__init__.py` with `__version__ = "0.1.0"`
- Created `src/gaia_depth_grade/config.py` with `Gains`, `GradeConfig` frozen dataclasses and `load_config()`
- Created `tests/test_config.py` with 3 tests per the brief

## TDD RED/GREEN Evidence

### RED (Step 4)
```
$ pytest tests/test_config.py -v
ERROR collecting tests/test_config.py
ImportError while importing test module ...
E   ModuleNotFoundError: No module named 'gaia_depth_grade.config'
```
Exit code 2. Module did not exist yet.

### GREEN (Step 6)
```
$ pytest tests/test_config.py -v
tests/test_config.py::test_defaults_when_no_path PASSED  [ 33%]
tests/test_config.py::test_toml_overrides PASSED         [ 66%]
tests/test_config.py::test_unknown_key_raises PASSED     [100%]
3 passed in 0.01s
```

### Full suite (pre-commit)
```
$ pytest
3 passed in 0.01s
```

## Files Changed

- `pyproject.toml` (created)
- `src/gaia_depth_grade/__init__.py` (created)
- `src/gaia_depth_grade/config.py` (created)
- `tests/test_config.py` (created)

## Self-Review Findings

- `GradeConfig` default `gains: Gains = Gains()` works correctly with Python 3.14 frozen dataclasses — the dataclass instance default is safe (not a mutable container).
- `_apply()` in `load_config` instantiates fresh objects just to get field names; this is a minor but acceptable cost for clarity. Alternatively `{f.name for f in fields(GradeConfig)}` would avoid instantiation, but matching the brief exactly is correct here.
- `tomllib` is stdlib in Python 3.11+, so no extra dep needed.
- The `replace(GradeConfig(), gains=gains, **raw)` pattern correctly sets `gains` first then overrides top-level fields. No issue with ordering since keyword args are distinct.

## Concerns

None. Python 3.14 venv with photutils wheel succeeded without fallback. All 3 tests pass. Implementation is exact to the brief.
