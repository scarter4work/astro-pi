### Task 3: Frame type classification

**Files:**
- Create: `src/astrometa/classify.py`
- Test: `tests/test_classify.py`

**Interfaces:**
- Consumes: `fitsheader.read_header`
- Produces: `classify.classify(filename: str, header: dict) -> str` returning one of `light`, `dark`, `flat`, `bias`, `derived`, `unknown`

Header `IMAGETYP` wins when present; filename convention is the fallback. Counts to reproduce over the real archive: 23,952 light, 1,205 flat, 400 bias, 341 dark, 949 derived, 1,281 unknown (7 autosave fall under `derived`).

- [ ] **Step 1: Write the failing test**

```python
# tests/test_classify.py
import pytest
from astrometa.classify import classify

@pytest.mark.parametrize("name,expected", [
    ("Light_IC 1848_1-1_120.0s_Bin1_HaO3_20260909-014908_182deg_0018.fit", "light"),
    ("Dark_120.0s_Bin1_Lqef_20260101-000000_1deg_0001.fit", "dark"),
    ("Bias_1.0ms_Bin1_HaO3_20260101-000000_0001.fit", "bias"),
    ("B_master_flat.fit", "flat"),
    ("flat_001.fit", "flat"),
    ("pp_light_00260.fit", "derived"),
    ("r_pp_light_00012.fit", "derived"),
    ("ASIVideoStack_Output_01.fit", "derived"),
    ("AS_P20_moon.fit", "derived"),
    ("Autosave001.fit", "derived"),
    ("something_unrecognised.fit", "unknown"),
])
def test_classify_from_filename(name, expected):
    assert classify(name, {}) == expected

def test_imagetyp_header_overrides_filename():
    assert classify("mystery.fit", {"IMAGETYP": "Dark Frame"}) == "dark"
    assert classify("mystery.fit", {"IMAGETYP": "Light Frame"}) == "light"
    assert classify("mystery.fit", {"IMAGETYP": "FLAT"}) == "flat"
    assert classify("mystery.fit", {"IMAGETYP": "Bias Frame"}) == "bias"

def test_unrecognised_imagetyp_falls_back_to_filename():
    assert classify("Light_M 42_1.fit", {"IMAGETYP": "wibble"}) == "light"

def test_target_name_with_space_does_not_break_classification():
    assert classify("Light_NGC 7635_300.0s_Bin1_Lqef_x_0001.fit", {}) == "light"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_classify.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.classify'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/classify.py
import re

_HEADER_MAP = {
    "light": "light", "light frame": "light",
    "dark": "dark", "dark frame": "dark",
    "flat": "flat", "flat field": "flat", "flat frame": "flat",
    "bias": "bias", "bias frame": "bias", "zero": "bias",
}

_DERIVED = re.compile(
    r"^(pp_light|r_pp_light|ASIVideoStack|AS_P|Autosave)", re.IGNORECASE)
_FLAT = re.compile(r"^flat|master_flat", re.IGNORECASE)

def classify(filename: str, header: dict) -> str:
    raw = header.get("IMAGETYP")
    if isinstance(raw, str):
        mapped = _HEADER_MAP.get(raw.strip().lower())
        if mapped:
            return mapped

    if _DERIVED.match(filename):
        return "derived"
    if filename.startswith("Light_"):
        return "light"
    if filename.startswith("Dark"):
        return "dark"
    if filename.startswith("Bias"):
        return "bias"
    if _FLAT.match(filename) or "master_flat" in filename.lower():
        return "flat"
    return "unknown"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_classify.py -v`
Expected: PASS, 17 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/classify.py tests/test_classify.py
git commit -m "feat: frame type classification"
```

---

