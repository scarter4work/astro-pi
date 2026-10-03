# Task 2 Report: FITS Header Parser

## Status: DONE

## Implementation Summary

Created a hand-rolled FITS header parser that reads only header blocks (2880-byte chunks), never pixel data. The parser correctly handles:

1. **Quoted strings with internal spaces** (e.g., `'M 20'`, `'IC 1848_1-1'`)
2. **Numeric values** (integers and floats)
3. **Logical values** (`T` → True, `F` → False)
4. **Comments** (text after `/` in unquoted values)
5. **Edge case: slashes inside quoted strings** are preserved as part of the value, not treated as comment delimiters
6. **Early termination** at END card, never reading pixel data
7. **Error handling** with explicit `FitsHeaderError` for malformed input

## Files Created

- `src/astrometa/fitsheader.py` — Main parser implementation
- `tests/conftest.py` — Fixtures for FITS file creation
- `tests/test_fitsheader.py` — Five test cases covering all parsing scenarios

## TDD Evidence

### Step 1: RED — Write failing test

Command:
```bash
cd /home/scarter4work/projects/astro-metadata-store/.worktrees/metadata-store
.venv/bin/python -m pytest tests/test_fitsheader.py -v
```

Output:
```
ERROR collecting tests/test_fitsheader.py
ImportError while importing test module ...
from astrometa import fitsheader
E   ImportError: cannot import name 'fitsheader' from 'astrometa'
```

**Expected failure reason:** Module `fitsheader.py` does not exist yet.

### Step 2: GREEN — Implement and run

After implementing `src/astrometa/fitsheader.py`:

```bash
.venv/bin/python -m pytest tests/test_fitsheader.py -v
```

Output:
```
tests/test_fitsheader.py::test_parses_string_number_and_logical PASSED   [ 20%]
tests/test_fitsheader.py::test_object_value_preserves_internal_spaces PASSED [ 40%]
tests/test_fitsheader.py::test_slash_inside_quoted_string_is_not_a_comment PASSED [ 60%]
tests/test_fitsheader.py::test_stops_at_END_and_does_not_read_pixels PASSED [ 80%]
tests/test_fitsheader.py::test_missing_END_raises PASSED                 [100%]

============================== 5 passed in 0.01s ===============================
```

### Step 3: Full test suite

```bash
.venv/bin/python -m pytest tests/ -v
```

Output:
```
tests/test_db.py::test_init_schema_creates_expected_tables PASSED
tests/test_db.py::test_init_schema_is_idempotent PASSED
tests/test_db.py::test_frames_primary_key_is_content_hash PASSED
tests/test_fitsheader.py::test_parses_string_number_and_logical PASSED
tests/test_fitsheader.py::test_object_value_preserves_internal_spaces PASSED
tests/test_fitsheader.py::test_slash_inside_quoted_string_is_not_a_comment PASSED
tests/test_fitsheader.py::test_stops_at_END_and_does_not_read_pixels PASSED
tests/test_fitsheader.py::test_missing_END_raises PASSED

============================== 8 passed in 0.01s ===============================
```

All tests pass. No regressions introduced.

## Commits Created

1. `2c2c301` — feat: raw FITS header parser
2. `c860a03` — refactor: add precise type hints including int

## Self-Review Findings

### Correctness

1. **Header block structure**: Correctly reads 2880-byte blocks and parses 80-byte FITS cards per specification
2. **Key-value extraction**: Keys extracted from bytes 0–7 (stripped), value indicator "= " at bytes 8–9, values start at byte 10
3. **Value parsing**:
   - Quoted strings: Extracts content between single quotes, preserving internal spaces while stripping padding
   - Comments: Unquoted values split on "/" and take first part; slashes inside quotes are preserved
   - Logicals: "T" → True, "F" → False
   - Numerics: Tries int, then float, falls back to string
4. **Memory efficiency**: Processes cards one block at a time, stops at END, never buffers pixel data
5. **Error handling**: Raises `FitsHeaderError` for truncated blocks, missing END, unterminated strings

### Type Safety

- Added proper type hints: `read_header(path: Path) -> dict[str, str | int | float | bool]`
- Matches deployed Python 3.13.5 capabilities (PEP 604 union syntax)
- Includes `int` type because tests verify integer values (e.g., `NAXIS1 == 3840`)

### Test Coverage

All five test cases from the brief pass:
- ✓ Parses strings, numbers, and logicals
- ✓ Preserves internal spaces in quoted strings
- ✓ Handles "/" inside quoted strings as part of value, not comment
- ✓ Stops at END and does not read 100 KB of pixel data
- ✓ Raises error on missing END card

### Edge Cases Handled

1. **Padding**: Correctly handles left-padding in card fields
2. **Comments after quoted strings**: `'Ha/O3'  / dual band` correctly extracts `Ha/O3`
3. **Spaces before values**: Leading whitespace in unquoted values stripped correctly
4. **Card skipping**: Non-key-value cards (missing "= ") skipped gracefully
5. **ASCII robustness**: Uses `errors="replace"` when decoding (though FITS is always ASCII)

### Design Decisions

1. **MAX_BLOCKS = 100**: Conservative 288 KB limit per brief; real headers never exceed a few KB
2. **Early return at END**: Stops processing immediately, prevents reading pixel data
3. **Strict value parsing**: No defaults for missing values; errors are loud (raises `FitsHeaderError`)
4. **String fallback**: Unrecognized numeric formats returned as strings (fail-open, not fail-silent)

## No Concerns

Implementation matches the brief exactly. All tests pass. No regressions in existing tests.

---

**Test Summary**: 5/5 FITS parser tests pass; 8/8 total suite pass
**Quality**: Type-safe, efficient, error-handling is correct and loud

---

# Fix Round 1: Loud Errors and FITS Escaped Quotes

## Findings Addressed

### Finding 1: Decode ASCII strictly, never silently replace

**Issue**: `decode("ascii", errors="replace")` silently replaced non-ASCII bytes with U+FFFD, producing garbled-but-valid-looking dict instead of recording the failure.

**Fix**: 
- Changed to strict ASCII decoding: `decode("ascii")` (raises on non-ASCII)
- Wrapped in try-except, catching `UnicodeDecodeError` and re-raising as `FitsHeaderError` with path
- Task 5's inventory pass catches exceptions and records them in read_error column, so loud failures are handled correctly

**Code change** (lines 40-45):
```python
try:
    card = block[i:i + CARD].decode("ascii")
except UnicodeDecodeError as e:
    raise FitsHeaderError(
        f"non-ASCII byte in header card in {path}: {e}"
    ) from e
```

### Finding 2: Handle FITS escaped quotes (`''` → `'`)

**Issue**: Parser failed on values with apostrophes. For `OBJECT = 'Barnard''s Loop'`, the parser stopped at the first quote of the doubled pair, returning `Barnard` and silently truncating `'s Loop`. This is a silent-object-name-truncation bug identical to what the project has already been burned by.

**Fix**:
- Rewrote quote-finding logic to scan forward and detect escaped quotes
- Only treat a quote as closing if it's NOT followed by another quote
- After extraction, unescape all `''` to `'` per FITS standard
- Real targets in archive carry apostrophes: Barnard's Loop, Bode's Galaxy, Hind's Variable Nebula

**Code change** (lines 14-29):
```python
if raw.startswith("'"):
    # Find closing quote, handling FITS escaped quotes ('')
    i = 1
    while i < len(raw):
        if raw[i] == "'":
            # Check if this quote is escaped (doubled)
            if i + 1 < len(raw) and raw[i + 1] == "'":
                # Escaped quote: skip both characters
                i += 2
            else:
                # This is the closing quote
                value = raw[1:i]
                # Unescape: '' -> '
                value = value.replace("''", "'")
                return value.strip()
        else:
            i += 1
    # No closing quote found
    raise FitsHeaderError(f"unterminated string: {raw!r}")
```

## Test Evidence

### New tests added

1. `test_escaped_quotes_in_string_value`: Verifies `'Barnard''s Loop'` → `Barnard's Loop`
2. `test_non_ascii_byte_raises`: Verifies non-ASCII byte raises `FitsHeaderError` with "non-ASCII" in message

### Full test run after fixes

Command:
```bash
.venv/bin/python -m pytest tests/ -v
```

Output:
```
tests/test_db.py::test_init_schema_creates_expected_tables PASSED        [ 10%]
tests/test_db.py::test_init_schema_is_idempotent PASSED                  [ 20%]
tests/test_db.py::test_frames_primary_key_is_content_hash PASSED         [ 30%]
tests/test_fitsheader.py::test_parses_string_number_and_logical PASSED   [ 40%]
tests/test_fitsheader.py::test_object_value_preserves_internal_spaces PASSED [ 50%]
tests/test_fitsheader.py::test_slash_inside_quoted_string_is_not_a_comment PASSED [ 60%]
tests/test_fitsheader.py::test_stops_at_END_and_does_not_read_pixels PASSED [ 70%]
tests/test_fitsheader.py::test_missing_END_raises PASSED                 [ 80%]
tests/test_fitsheader.py::test_escaped_quotes_in_string_value PASSED     [ 90%]
tests/test_fitsheader.py::test_non_ascii_byte_raises PASSED              [100%]

============================== 10 passed in 0.01s ==============================
```

**All original 5 tests still pass.** No regressions introduced.

## Commit Created

- `221ea47` — fix: loud ASCII validation and FITS escaped quotes handling

## Verification

- Original test `test_slash_inside_quoted_string_is_not_a_comment` still passes (closing-quote scan logic correctly handles edge cases)
- New test verifies escaped quotes: `'Barnard''s Loop'` returns exactly `Barnard's Loop`
- New test verifies ASCII errors: non-ASCII bytes raise `FitsHeaderError` with path
