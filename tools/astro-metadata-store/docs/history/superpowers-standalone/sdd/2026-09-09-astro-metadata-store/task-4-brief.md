### Task 4: Content hash, perceptual fingerprint, and pixel statistics

**Files:**
- Create: `src/astrometa/imagekeys.py`
- Test: `tests/test_imagekeys.py`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `imagekeys.content_hash(path: Path) -> str` (BLAKE2b hex, 32 chars)
  - `imagekeys.fingerprint(pixels: np.ndarray, size: int = 16) -> str` (hex dHash)
  - `imagekeys.hamming(a: str, b: str) -> int`
  - `imagekeys.pixel_stats(pixels: np.ndarray) -> tuple[float, float]` returning `(bg_median, saturated_frac)`

Percentile normalisation is load-bearing, not cosmetic: raw astronomical frames vary enormously in absolute level with exposure, gain and sky brightness, so an un-normalised hash would cluster frames by *exposure* rather than by *field*.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_imagekeys.py
import numpy as np
from astrometa import imagekeys

def test_content_hash_is_stable_and_distinct(tmp_path):
    a = tmp_path / "a"; a.write_bytes(b"hello")
    b = tmp_path / "b"; b.write_bytes(b"hello")
    c = tmp_path / "c"; c.write_bytes(b"world")
    assert imagekeys.content_hash(a) == imagekeys.content_hash(b)
    assert imagekeys.content_hash(a) != imagekeys.content_hash(c)

def _field(seed: int, scale: float = 1.0, offset: float = 0.0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    img = rng.normal(100, 5, (256, 256))
    for _ in range(40):                      # deterministic "stars"
        y, x = rng.integers(0, 250, 2)
        img[y:y+3, x:x+3] += 3000
    return img * scale + offset

def test_same_field_at_different_exposure_matches():
    base = _field(1)
    brighter = _field(1, scale=4.0, offset=500.0)
    d = imagekeys.hamming(imagekeys.fingerprint(base),
                          imagekeys.fingerprint(brighter))
    assert d <= 8, f"same field should survive exposure change, got {d}"

def test_different_fields_do_not_match():
    d = imagekeys.hamming(imagekeys.fingerprint(_field(1)),
                          imagekeys.fingerprint(_field(2)))
    assert d > 40, f"different fields should differ, got {d}"

def test_fingerprint_is_deterministic():
    img = _field(7)
    assert imagekeys.fingerprint(img) == imagekeys.fingerprint(img)

def test_flat_image_does_not_crash():
    assert isinstance(imagekeys.fingerprint(np.full((64, 64), 5.0)), str)

def test_pixel_stats_reports_background_and_saturation():
    img = np.full((100, 100), 200.0)
    img[:10, :] = 65535.0
    bg, sat = imagekeys.pixel_stats(img)
    assert bg == 200.0
    assert abs(sat - 0.10) < 0.001
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_imagekeys.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.imagekeys'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/imagekeys.py
import hashlib
from pathlib import Path
import numpy as np

_CHUNK = 8 * 1024 * 1024
SATURATION_LEVEL = 65000.0

def content_hash(path: Path) -> str:
    h = hashlib.blake2b(digest_size=16)
    with open(path, "rb") as f:
        while chunk := f.read(_CHUNK):
            h.update(chunk)
    return h.hexdigest()

def _block_mean(a: np.ndarray, out_h: int, out_w: int) -> np.ndarray:
    h, w = a.shape
    a = a[: h - h % out_h, : w - w % out_w]
    return a.reshape(out_h, a.shape[0] // out_h,
                     out_w, a.shape[1] // out_w).mean(axis=(1, 3))

def fingerprint(pixels: np.ndarray, size: int = 16) -> str:
    a = np.asarray(pixels, dtype=np.float64)
    lo, hi = np.percentile(a, (1.0, 99.0))
    if not np.isfinite(lo) or not np.isfinite(hi) or hi <= lo:
        return "0" * ((size * (size + 1) + 3) // 4)
    a = np.clip((a - lo) / (hi - lo), 0.0, 1.0)
    small = _block_mean(a, size, size + 1)
    bits = (small[:, 1:] > small[:, :-1]).flatten()
    value = 0
    for bit in bits:
        value = (value << 1) | int(bit)
    return f"{value:0{(bits.size + 3) // 4}x}"

def hamming(a: str, b: str) -> int:
    return bin(int(a, 16) ^ int(b, 16)).count("1")

def pixel_stats(pixels: np.ndarray) -> tuple[float, float]:
    a = np.asarray(pixels, dtype=np.float64)
    return float(np.median(a)), float((a >= SATURATION_LEVEL).mean())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_imagekeys.py -v`
Expected: PASS, 7 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/imagekeys.py tests/test_imagekeys.py
git commit -m "feat: content hash, perceptual fingerprint, pixel statistics"
```

---

