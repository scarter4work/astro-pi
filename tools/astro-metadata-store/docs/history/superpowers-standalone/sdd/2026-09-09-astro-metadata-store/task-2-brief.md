### Task 2: FITS header parser

**Files:**
- Create: `src/astrometa/fitsheader.py`
- Create: `tests/conftest.py`
- Test: `tests/test_fitsheader.py`

**Interfaces:**
- Consumes: nothing
- Produces: `fitsheader.read_header(path: Path) -> dict[str, str | float | bool]`, raising `fitsheader.FitsHeaderError` on malformed input

Reads only header blocks (~3–30 KB), never pixel data. This is the pass that runs over all 28,135 files, so it must not pull 450 GB through.

- [ ] **Step 1: Write the failing test**

```python
# tests/conftest.py
import pytest

def _card(key: str, value: str) -> bytes:
    return f"{key:<8}= {value:<70}"[:80].encode("ascii")

def build_fits_header(cards: dict[str, str], pad_to_blocks: int = 1) -> bytes:
    out = b"".join(_card(k, v) for k, v in cards.items())
    out += b"END".ljust(80)
    remainder = len(out) % 2880
    if remainder:
        out += b" " * (2880 - remainder)
    while len(out) < 2880 * pad_to_blocks:
        out += b" " * 2880
    return out

@pytest.fixture
def make_fits(tmp_path):
    def _make(name: str, cards: dict[str, str], pixels: bytes = b"") -> "Path":
        p = tmp_path / name
        p.write_bytes(build_fits_header(cards) + pixels)
        return p
    return _make
```

```python
# tests/test_fitsheader.py
import pytest
from astrometa import fitsheader

def test_parses_string_number_and_logical(make_fits):
    p = make_fits("a.fit", {
        "SIMPLE": "                   T",
        "OBJECT": "'IC 1848_1-1'",
        "EXPTIME": "            120.0",
        "NAXIS1": "             3840",
    })
    h = fitsheader.read_header(p)
    assert h["OBJECT"] == "IC 1848_1-1"
    assert h["EXPTIME"] == 120.0
    assert h["NAXIS1"] == 3840
    assert h["SIMPLE"] is True

def test_object_value_preserves_internal_spaces(make_fits):
    p = make_fits("b.fit", {"OBJECT": "'M 20'"})
    assert fitsheader.read_header(p)["OBJECT"] == "M 20"

def test_slash_inside_quoted_string_is_not_a_comment(make_fits):
    p = make_fits("c.fit", {"FILTER": "'Ha/O3'  / dual band"})
    assert fitsheader.read_header(p)["FILTER"] == "Ha/O3"

def test_stops_at_END_and_does_not_read_pixels(make_fits):
    p = make_fits("d.fit", {"OBJECT": "'X'"}, pixels=b"\xff" * 100000)
    h = fitsheader.read_header(p)
    assert h["OBJECT"] == "X"
    assert "\xff" not in str(h)

def test_missing_END_raises(tmp_path):
    p = tmp_path / "bad.fit"
    p.write_bytes(b" " * 2880)
    with pytest.raises(fitsheader.FitsHeaderError):
        fitsheader.read_header(p)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_fitsheader.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.fitsheader'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/fitsheader.py
from pathlib import Path

BLOCK = 2880
CARD = 80
MAX_BLOCKS = 100          # 288 KB; a real header never approaches this

class FitsHeaderError(Exception):
    pass

def _parse_value(raw: str):
    raw = raw.strip()
    if raw.startswith("'"):
        end = raw.find("'", 1)
        if end == -1:
            raise FitsHeaderError(f"unterminated string: {raw!r}")
        return raw[1:end].strip()
    raw = raw.split("/", 1)[0].strip()
    if raw in ("T", "F"):
        return raw == "T"
    try:
        return int(raw)
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        return raw

def read_header(path: Path) -> dict:
    cards: dict = {}
    with open(path, "rb") as f:
        for _ in range(MAX_BLOCKS):
            block = f.read(BLOCK)
            if len(block) < BLOCK:
                raise FitsHeaderError(f"truncated header in {path}")
            for i in range(0, BLOCK, CARD):
                card = block[i:i + CARD].decode("ascii", errors="replace")
                key = card[:8].strip()
                if key == "END":
                    return cards
                if card[8:10] != "= ":
                    continue
                cards[key] = _parse_value(card[10:])
    raise FitsHeaderError(f"no END card within {MAX_BLOCKS} blocks in {path}")
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_fitsheader.py -v`
Expected: PASS, 5 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/fitsheader.py tests/conftest.py tests/test_fitsheader.py
git commit -m "feat: raw FITS header parser"
```

---

