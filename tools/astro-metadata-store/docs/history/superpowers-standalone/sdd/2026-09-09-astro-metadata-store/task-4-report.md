# Task 4 Report: Content hash, perceptual fingerprint, pixel statistics

## Status: DONE

## What was implemented

`src/astrometa/imagekeys.py` — four functions, exactly the brief's interface:

- `content_hash(path: Path) -> str` — chunked BLAKE2b (8 MiB chunks, stdlib `hashlib`, `digest_size=16` → 32 hex chars). Never reads a whole file into memory.
- `fingerprint(pixels: np.ndarray, size: int = 16) -> str` — percentile-normalised (1st/99th) dHash: block-mean pool to a `(size, size+1)` grid, compare adjacent columns, pack `size*size` bits into a hex string.
- `hamming(a: str, b: str) -> int` — XOR the two hex values as integers, popcount.
- `pixel_stats(pixels: np.ndarray) -> tuple[float, float]` — `(median, fraction >= SATURATION_LEVEL)`, `SATURATION_LEVEL = 65000.0`.

## Correction applied (per team lead's pre-flight review)

The brief's flat-image early return used `(size * (size + 1) + 3) // 4`, which is **68** hex chars for `size=16`. The normal code path always emits `size * size = 256` bits = **64** hex chars, because `_block_mean(a, size, size+1)` yields a `(16, 17)` array and the column-gradient comparison `small[:, 1:] > small[:, :-1]` collapses it to `(16, 16)`.

Fixed to `(size * size + 3) // 4` (= 64 for size=16), so every fingerprint this function returns is the same bit width regardless of which branch produced it. `hamming()` would otherwise silently compare mismatched-length hex values whenever one input hit the degenerate path.

Added `test_flat_fingerprint_matches_length_of_normal_fingerprint`, asserting `len(fingerprint(flat_image)) == len(fingerprint(normal_image))`, as instructed.

## TDD evidence

**RED** — `tests/test_imagekeys.py` written first (brief's 6 tests + the length-parity test), run before `imagekeys.py` existed:

```
$ .venv/bin/python -m pytest tests/test_imagekeys.py -v
ImportError while importing test module '.../tests/test_imagekeys.py'.
tests/test_imagekeys.py:2: in <module>
    from astrometa import imagekeys
E   ImportError: cannot import name 'imagekeys' from 'astrometa'
```

Failed for the expected reason — module didn't exist yet (import-time `ImportError`, the module-not-found variant of what the brief predicted).

**GREEN** — after implementing `src/astrometa/imagekeys.py` with the corrected formula:

```
$ .venv/bin/python -m pytest tests/test_imagekeys.py -v
tests/test_imagekeys.py::test_content_hash_is_stable_and_distinct PASSED
tests/test_imagekeys.py::test_same_field_at_different_exposure_matches PASSED
tests/test_imagekeys.py::test_different_fields_do_not_match PASSED
tests/test_imagekeys.py::test_fingerprint_is_deterministic PASSED
tests/test_imagekeys.py::test_flat_image_does_not_crash PASSED
tests/test_imagekeys.py::test_flat_fingerprint_matches_length_of_normal_fingerprint PASSED
tests/test_imagekeys.py::test_pixel_stats_reports_background_and_saturation PASSED
============================== 7 passed in 0.05s ===============================
```

Full suite, run once before committing:

```
$ .venv/bin/python -m pytest
tests/test_classify.py ................                                  [ 48%]
tests/test_db.py ...                                                     [ 57%]
tests/test_fitsheader.py .......                                         [ 78%]
tests/test_imagekeys.py .......                                          [100%]
============================== 33 passed in 0.06s ===============================
```

## Measured Hamming distances (not tuned, no thresholds touched)

Isolated verification script, same `_field()` generator as the tests:

- **Same field, different exposure** (`_field(1)` vs `_field(1, scale=4.0, offset=500.0)`): **0** (threshold: `<= 8`)
- **Different fields** (`_field(1)` vs `_field(2)`): **123** (threshold: `> 40`)
- `fingerprint(base)` length: 64 hex chars; flat-image fingerprint length: 64 hex chars (parity confirmed)

Both statistical tests passed on the algorithm as specified — no threshold, test-data, or percentile adjustment was made. The 0-distance result is a strong signal the percentile normalisation is doing exactly its job (killing the scale+offset exposure difference entirely), and the 123-distance result (out of a max possible 256) shows real fields are far apart in this space.

## Files changed

- `src/astrometa/imagekeys.py` (new)
- `tests/test_imagekeys.py` (new)

Commit: `40b4b0b` — "feat: content hash, perceptual fingerprint, pixel statistics"

## Self-review

Read the diff fresh after committing:

- **Correctness**: formula fix verified by direct computation (`_block_mean` output shape math) and by the new length-parity test, not just by the fix "looking right."
- **Constraints honored**: BLAKE2b from stdlib only (no BLAKE3/third-party dep); chunked file read (8 MiB, never whole-file); no silent fallback except the one the brief explicitly sanctions (flat-image dHash early return, which is a legitimate degenerate result, not a swallowed error); never writes to the archive (read-only `open(path, "rb")`); no syntax newer than 3.13 (walrus operator is 3.8+, `tuple[float, float]` is 3.9+, both already used elsewhere in this codebase e.g. `fitsheader.py`).
- **Style**: matches existing modules — flat functions, no unnecessary classes, minimal docstrings only where the brief's original code lacked one and the behavior (degenerate-image handling) benefited from an explanation the plain code doesn't otherwise convey (matches `classify.py`'s one docstring-bearing function).
- **Test honesty**: did not touch the two threshold assertions (`<= 8`, `> 40`); ran them against the unmodified spec algorithm and they passed with wide margins (0 and 123). No test data or percentile bounds were adjusted to force a pass.
- **YAGNI**: implemented exactly the four functions the interface specifies, nothing more — no input validation beyond what the brief's code already had (e.g., no explicit `size <= 0` guard), since the brief didn't call for it and Task 5/6 callers control their own inputs.

## Concerns

None blocking. Two minor observations, not acted on because they're outside this task's specified scope:

1. `pixel_stats` has no NaN/degenerate-input handling (unlike `fingerprint`, which explicitly guards via `np.isfinite`). `np.median`/`.mean()` on NaN-containing pixel data will silently propagate `NaN` rather than raising. The brief didn't request this and no test exercises it; flagging in case Task 5 (inventory pass) feeds it real-world degenerate frames.
2. `fingerprint`'s `size` parameter has no validation (e.g., `size=0` or `size=1` would misbehave in `_block_mean`/bit-packing). Not exercised by any test and the brief doesn't call for it; default is always used by callers so far.

---

## Fix report — review round 1 (2026-09-09)

Review came back "Needs fixes" with two Important findings — both are exactly the two concerns I self-flagged above, plus a matching `hamming()` gap the reviewer added. Team lead ruled both must-fix (not deferrable): they're silent-wrong-answer paths, which the project's "errors are loud, no silent fallbacks" constraint exists to prevent.

**Finding 1 — `hamming()` did no width validation.** Two hex strings of different bit-width XOR'd as raw integers, producing a numerically plausible but meaningless distance instead of raising. Concretely dangerous: Task 6 clusters frames by this distance, so a mismatched-width comparison would silently produce wrong sky-field groupings, which then propagate into which frames get plate-solved and what object they're filed under.

**Finding 2 — `pixel_stats()` had no NaN guard.** `np.median`/`.mean()` propagate NaN silently into `bg_median`/`saturated_frac` for any frame containing NaN pixels. That value is stored per-frame and becomes `sky_background`, which Task 11 feeds into session cull thresholds — a NaN there makes every `<`/`>` comparison against it silently `False`, so nothing gets culled and nothing reports why.

### What changed

`src/astrometa/imagekeys.py`:

- `hamming(a, b)`: raises `ValueError` when `len(a) != len(b)`, before doing the XOR.
- `pixel_stats(pixels)`: raises `ValueError` when `np.isfinite(a).all()` is false (any NaN or Inf pixel), before computing median/saturation.

### NaN policy chosen, and why

Chose **raise**, not a sentinel value, for `pixel_stats`. This is a deliberate asymmetry with `fingerprint()`'s degenerate-image handling, not an inconsistency:

- `fingerprint()`'s all-zero sentinel for a flat/degenerate image is a *legitimate result* — "no gradient found" is a real, meaningful answer for a dHash, and it's a fixed, recognizable bit pattern the caller can compare against.
- `pixel_stats()` has no analogous safe sentinel. A background level and saturated fraction are continuous floats; there's no reserved value that means "invalid" without risking being mistaken for a real reading. Returning `NaN` itself would not fix anything — that's exactly the bug being fixed, since `NaN` doesn't raise on comparison, it just makes every threshold check silently `False`. The only option that is actually loud is to raise.

Team lead's framing matches this: "do not return a quietly-wrong number — either raise, or return a clearly-signalled absent value." For `pixel_stats` there is no clearly-signalled absent value in float space that downstream threshold comparisons can't silently swallow, so raise was the only choice that satisfies the constraint.

No changes made to `_block_mean` or `fingerprint`'s `size` handling — team lead confirmed real archive frames are never smaller than 320×240 (planetary captures), far above the 16×17 block grid, so a small-input guard there would be dead code; and all current callers use `fingerprint`'s default `size`, so no validation was added there either.

### Covering tests added

`tests/test_imagekeys.py`:

```python
def test_hamming_raises_on_mismatched_width():
    with pytest.raises(ValueError):
        imagekeys.hamming("ab", "abcd")

def test_pixel_stats_raises_on_nan_pixels():
    img = np.full((10, 10), 200.0)
    img[0, 0] = np.nan
    with pytest.raises(ValueError):
        imagekeys.pixel_stats(img)
```

### TDD evidence

**RED** — confirmed against the pre-fix code by stashing only `src/astrometa/imagekeys.py` (keeping the new tests in place), then running just the two new tests:

```
$ git stash push --keep-index -- src/astrometa/imagekeys.py
$ .venv/bin/python -m pytest tests/test_imagekeys.py -v -k "mismatched_width or nan_pixels"
tests/test_imagekeys.py::test_hamming_raises_on_mismatched_width FAILED
tests/test_imagekeys.py::test_pixel_stats_raises_on_nan_pixels FAILED

FAILED ... test_hamming_raises_on_mismatched_width - Failed: DID NOT RAISE ValueError
FAILED ... test_pixel_stats_raises_on_nan_pixels - Failed: DID NOT RAISE ValueError
======================= 2 failed, 7 deselected in 0.05s ========================
```

Failed for the expected reason — the old code silently accepted both a mismatched-width comparison and NaN pixel data instead of raising.

```
$ git stash pop
```

restored the fix.

**GREEN** — full `test_imagekeys.py` after restoring the fix:

```
$ .venv/bin/python -m pytest tests/test_imagekeys.py -v
tests/test_imagekeys.py::test_content_hash_is_stable_and_distinct PASSED
tests/test_imagekeys.py::test_same_field_at_different_exposure_matches PASSED
tests/test_imagekeys.py::test_different_fields_do_not_match PASSED
tests/test_imagekeys.py::test_fingerprint_is_deterministic PASSED
tests/test_imagekeys.py::test_flat_image_does_not_crash PASSED
tests/test_imagekeys.py::test_flat_fingerprint_matches_length_of_normal_fingerprint PASSED
tests/test_imagekeys.py::test_pixel_stats_reports_background_and_saturation PASSED
tests/test_imagekeys.py::test_hamming_raises_on_mismatched_width PASSED
tests/test_imagekeys.py::test_pixel_stats_raises_on_nan_pixels PASSED
============================== 9 passed in 0.06s ===============================
```

Full suite, run once before committing:

```
$ .venv/bin/python -m pytest
tests/test_classify.py ................                                  [ 45%]
tests/test_db.py ...                                                     [ 54%]
tests/test_fitsheader.py .......                                         [ 74%]
tests/test_imagekeys.py .........                                        [100%]
============================== 35 passed in 0.06s ==============================
```

### Files changed

- `src/astrometa/imagekeys.py` (modified)
- `tests/test_imagekeys.py` (modified)

Commit: `96c50ce` — "fix: raise loudly on hamming width mismatch and NaN pixel stats"

### Remaining concerns

None. Both Important findings are fixed and covered by tests that were shown RED against the pre-fix code and GREEN after. `_block_mean`/`fingerprint size` were confirmed out of scope by the team lead based on real archive frame dimensions.
