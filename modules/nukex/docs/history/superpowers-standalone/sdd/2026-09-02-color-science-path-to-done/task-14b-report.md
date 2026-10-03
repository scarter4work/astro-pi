# Task 14b Report: Bayer-aware broadband names

## Summary
Implemented Task 14b exactly as specified: broadband filter names (L, Luminance, L-Pro, LPS, UV-IR-cut, CLS, etc.) now resolve to `BROADBAND_OSC` on Bayer frames and `BROADBAND_L` on mono frames, with sensor-aware naming ("OSC" vs "L"). This fixes the regression where OSC frames tagged `FILTER='L'` were incorrectly classified as mono, and allows previously rejected frames like M16 (FILTER='LPro', RGGB) to be accepted.

## Implementation

### Files Changed
1. **src/lib/io/src/filter_classifier.cpp**
   - Added `#include <unordered_set>`
   - Removed `"l"` and `"luminance"` rows from `known_table()` (lines 20-21)
   - Added `broadband_any_names()` function returning a static unordered_set of broadband filter name aliases (normalized to lowercase alphanumerics)
   - Added logic in `classify()` method to check `broadband_any_names()` and branch on sensor type:
     - Bayer: `FilterClass::BROADBAND_OSC`, name `"OSC"`
     - Mono: `FilterClass::BROADBAND_L`, name `"L"`
     - Both cases: `BandwidthSpec{550.0, 300.0}`

2. **test/unit/io/test_filter_classifier.cpp**
   - Added `TEST_CASE("FilterClassifier: broadband names on Bayer resolve to BROADBAND_OSC")`
     - Tests 8 broadband filter names on Bayer camera (RGGB): L, Luminance, LPro, L-Pro, UV/IR Cut, CLS-CCD, LPS-D1, L1
     - Verifies each returns `FilterClass::BROADBAND_OSC`, name `"OSC"`, and no warning
   - Added `TEST_CASE("FilterClassifier: broadband LPR names on mono resolve to BROADBAND_L named L")`
     - Tests 4 broadband LPR-style names on mono camera: LPro, UV-IR-Cut, CLS, LPS-D2
     - Verifies each returns `FilterClass::BROADBAND_L`, name `"L"`, and no warning

### Design Details
- Broadband names cover:
  - Plain luminance: `l`, `lum`, `luminance` (moved from known_table)
  - Light-pollution filters (LPR): `lpro`, `lpr`, `lps`, `lpsd1`, `lpsd2`, `lpsd3`, `lpsv4`
  - UV-IR blocking filters: `uvir`, `uvircut`, `uvirblock`, `irblock`, `uvcut`
  - CLS filters: `cls`, `clsccd`
  - Astronomik line: `l1`, `l2`, `l3`
- Normalization (alphanumerics only) handles all input variants (e.g., "L-Pro", "UV/IR Cut", "CLS-CCD")
- Sensor detection: `is_bayer = !meta.bayer_pattern.empty()`

## Testing Evidence

### TDD: RED Phase
```
$ cd build && ./test/test_filter_classifier_io 2>&1 | tail -20
(two test cases fail initially)
FilterClassifier: broadband names on Bayer resolve to BROADBAND_OSC
  FAILED: REQUIRE( f.cls == FilterClass::BROADBAND_OSC )
  with expansion: 1 == 3
  with message: L

FilterClassifier: broadband LPR names on mono resolve to BROADBAND_L named L
  FAILED: REQUIRE( f.name == "L" )
  with expansion: "LPro" == "L"
  with message: LPro

test cases: 11 |  9 passed | 2 failed
assertions: 37 | 35 passed | 2 failed
```

Why expected:
- "L" on Bayer was returning `BROADBAND_L` (1) instead of `BROADBAND_OSC` (3)
- "LPro" on mono was returning "LPro" as name instead of "L"
- Filter names not in `known_table()` were not being recognized as broadband

### TDD: GREEN Phase
```
$ cd build && ./test/test_filter_classifier_io 2>&1 | tail -20
All tests passed (70 assertions in 11 test cases)
```

After implementation, all 11 test cases pass:
- 9 existing tests (unchanged behavior)
- 2 new test cases (both now green)
- 70 total assertions passing

### Full Suite Verification
```
$ cd build && make -j$(nproc) && ctest 2>&1 | tail -5
67/67 Test #67: test_phase_b_qsolve ...............   Passed    0.01 sec
100% tests passed, 0 tests failed out of 67
Total Test time (real) =  60.41 sec
```

Relevant subset (filter + engine integration):
```
$ ctest -R "filter|channel_config_from_filter|engine_config|cache_sig" --output-on-failure 2>&1 | tail -10
test_channel_config_from_filter ....   Passed
test_engine_config .................   Passed
test_cache_sig_dispatch ............   Passed
test_filter_classifier .............   Passed
100% tests passed, 6 tests passed out of 6
```

Key integration tests verify the broadband classifier works correctly through the channel config pipeline.

## Commit
```
7b33e6c fix(io): broadband filter names resolve by sensor type (OSC on Bayer, L on mono)
```

Full commit message includes both trailer lines as specified.

## Self-Review

### Completeness
✓ Both test cases added (Bayer broadband → OSC, mono LPR → L)
✓ `"l"` and `"luminance"` removed from `known_table()`
✓ `broadband_any_names()` function in anonymous namespace
✓ New logic placed after empty-name branch, before `lookup_known`
✓ `#include <unordered_set>` added

### Quality
✓ Code exactly matches brief specifications
✓ 4-space indentation consistent throughout
✓ Comment explains the logic and sources (research DB)
✓ Function returns static reference (efficient)
✓ Both test loops use INFO() for better failure context

### Discipline
✓ Nothing beyond the brief (no extra logic, tests, or refactoring)
✓ Warning string unchanged (as noted in brief)
✓ Existing test "L / R / G / B broadband on mono" still passes unchanged
✓ Mono L → BROADBAND_L named "L" (not "L_unnamed" for known broadband)

### Testing
✓ TDD discipline: tests shown failing first, then passing after implementation
✓ All 11 filter tests pass
✓ All 67 integration tests pass
✓ Output pristine (no warnings, no undefined references)

### Issues or Concerns
None. The implementation is complete, tested, and matches the brief exactly.

## Files Modified
- `/home/scarter4work/projects/nukex5/src/lib/io/src/filter_classifier.cpp`
- `/home/scarter4work/projects/nukex5/test/unit/io/test_filter_classifier.cpp`
