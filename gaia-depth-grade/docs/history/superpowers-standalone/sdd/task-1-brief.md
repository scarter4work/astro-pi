### Task 1: Project scaffolding, env, and config

**Files:**
- Create: `pyproject.toml`
- Create: `src/gaia_depth_grade/__init__.py`
- Create: `src/gaia_depth_grade/config.py`
- Test: `tests/test_config.py`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `Gains(brightness: float=0.5, size: float=0.4, contrast: float=0.3, saturation: float=0.3)` — dataclass.
  - `GradeConfig(gains: Gains, p_low: float=5.0, p_high: float=95.0, match_tolerance_px: float=3.0, min_match_rate: float=0.3, detect_fwhm: float=3.0, detect_threshold_sigma: float=5.0, base_sigma_px: float=2.0, cache_dir: str=".cache", neutral_strength: float=0.0)` — dataclass.
  - `load_config(path: str | None) -> GradeConfig` — returns defaults if `path is None`, else parses TOML (`tomllib`), overriding any provided keys; unknown keys raise `ValueError`.

- [ ] **Step 1: Create the virtualenv and install deps**

Run:
```bash
cd /home/scarter4work/projects/gaia-depth-grade
uv venv --python 3.14 .venv
. .venv/bin/activate
uv pip install "numpy>=2" scipy "astropy>=7" photutils astroquery pytest
```
Expected: all install without error. If `photutils` has no 3.14 wheel, fall back: `uv venv --python 3.13 .venv` and re-run (note the chosen version in the commit message).

- [ ] **Step 2: Write `pyproject.toml`**

```toml
[project]
name = "gaia-depth-grade"
version = "0.1.0"
description = "Physically-grounded depth grade for astrophotographs using Gaia distances"
requires-python = ">=3.13"
dependencies = ["numpy>=2", "scipy", "astropy>=7", "photutils", "astroquery"]

[build-system]
requires = ["setuptools>=68"]
build-backend = "setuptools.build_meta"

[tool.setuptools.packages.find]
where = ["src"]

[tool.pytest.ini_options]
pythonpath = ["src"]
testpaths = ["tests"]
```

- [ ] **Step 3: Write the failing test**

`tests/test_config.py`:
```python
import textwrap
import pytest
from gaia_depth_grade.config import Gains, GradeConfig, load_config


def test_defaults_when_no_path():
    cfg = load_config(None)
    assert isinstance(cfg, GradeConfig)
    assert cfg.gains.brightness == 0.5
    assert cfg.p_low == 5.0 and cfg.p_high == 95.0


def test_toml_overrides(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text(textwrap.dedent("""
        p_low = 10.0
        [gains]
        brightness = 1.0
        saturation = 0.0
    """))
    cfg = load_config(str(p))
    assert cfg.p_low == 10.0
    assert cfg.gains.brightness == 1.0
    assert cfg.gains.saturation == 0.0
    assert cfg.gains.size == 0.4  # untouched default


def test_unknown_key_raises(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text("bogus_key = 1\n")
    with pytest.raises(ValueError):
        load_config(str(p))
```

- [ ] **Step 4: Run test to verify it fails**

Run: `pytest tests/test_config.py -v`
Expected: FAIL — `ModuleNotFoundError: gaia_depth_grade.config`.

- [ ] **Step 5: Implement `config.py`**

`src/gaia_depth_grade/__init__.py`:
```python
__all__ = ["__version__"]
__version__ = "0.1.0"
```

`src/gaia_depth_grade/config.py`:
```python
from __future__ import annotations

import tomllib
from dataclasses import dataclass, fields, replace


@dataclass(frozen=True)
class Gains:
    brightness: float = 0.5
    size: float = 0.4
    contrast: float = 0.3
    saturation: float = 0.3


@dataclass(frozen=True)
class GradeConfig:
    gains: Gains = Gains()
    p_low: float = 5.0
    p_high: float = 95.0
    match_tolerance_px: float = 3.0
    min_match_rate: float = 0.3
    detect_fwhm: float = 3.0
    detect_threshold_sigma: float = 5.0
    base_sigma_px: float = 2.0
    cache_dir: str = ".cache"
    neutral_strength: float = 0.0


def _apply(obj, data: dict):
    valid = {f.name for f in fields(obj)}
    unknown = set(data) - valid
    if unknown:
        raise ValueError(f"Unknown config keys: {sorted(unknown)}")
    return data


def load_config(path: str | None) -> GradeConfig:
    if path is None:
        return GradeConfig()
    with open(path, "rb") as fh:
        raw = tomllib.load(fh)
    gains_raw = raw.pop("gains", {})
    _apply(GradeConfig(), raw)
    _apply(Gains(), gains_raw)
    gains = replace(Gains(), **gains_raw)
    return replace(GradeConfig(), gains=gains, **raw)
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `pytest tests/test_config.py -v`
Expected: PASS (3 passed).

- [ ] **Step 7: Commit**

```bash
git add pyproject.toml src/gaia_depth_grade/__init__.py src/gaia_depth_grade/config.py tests/test_config.py
git commit -m "feat: scaffold package, env, and config loader"
```

---

