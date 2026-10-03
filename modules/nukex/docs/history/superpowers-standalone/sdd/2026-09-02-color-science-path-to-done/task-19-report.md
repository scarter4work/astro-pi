# Task 19 Implementation Report: Delete legacy module files

## What Was Implemented

**Step 1: Verified no remaining references**
- Ran grep to confirm no code outside the files being deleted referenced `filter_classifier.hpp`, `fits_metadata.hpp`, `read_fits_metadata`, or `FITSMetadata`
- Result: clean (no output)

**Step 2: Deleted six files using git rm**
- `src/module/filter_classifier.hpp`
- `src/module/filter_classifier.cpp`
- `src/module/fits_metadata.hpp`
- `src/module/fits_metadata.cpp`
- `test/unit/module/test_filter_classifier.cpp`
- `test/unit/module/test_fits_metadata.cpp`

**Step 3: Updated CMakeLists.txt**

`src/module/CMakeLists.txt`:
- Deleted the nine-line comment block about filter_classifier.cpp (lines 13-21)
- Removed `fits_metadata.cpp` from `MODULE_SOURCES` (was line 30)
- Deleted the cfitsio block (lines 64-67) since `nukex4_io` carries cfitsio transitively

`test/CMakeLists.txt`:
- Replaced header comment `# Module-layer tests (fits_metadata and future module code)` with `# Module-layer tests (auto-selector, stretch factory, Phase 8 fallback)`
- Deleted entire `nukex4_module_legacy_testlib` OBJECT library block (22 lines)
- Deleted `nukex_add_test(test_fits_metadata ...)` line
- Deleted `nukex_add_test(test_filter_classifier ...)` line
- Left `nukex4_module_testlib` and remaining three `nukex_add_test` lines untouched

**Step 4: Build and Test**
```
cd build && cmake .. && make -j$(nproc)
Result: No linker errors, no undefined references
ctest result: 100% tests passed, 65 tests passed out of 65
```

**Step 5: Committed**
```
Commit: 35f8fd1
Message: chore(module): delete module-local filter_classifier + fits_metadata
Trailing lines: Co-Authored-By + Claude-Session URL as specified
```

## What Was Tested

✅ No remaining references to deleted files/symbols (grep returned empty)
✅ All six files successfully deleted
✅ CMake configuration updates applied correctly
✅ Module builds without linker errors (no undefined references for missing cfitsio)
✅ Test count reduced from 67 to 65 (two test binaries removed)
✅ All 65 remaining tests pass
✅ Commit created with exact message format

## Files Changed / Deleted

**Deleted (6):**
- src/module/filter_classifier.hpp
- src/module/filter_classifier.cpp
- src/module/fits_metadata.hpp
- src/module/fits_metadata.cpp
- test/unit/module/test_filter_classifier.cpp
- test/unit/module/test_fits_metadata.cpp

**Modified (2):**
- src/module/CMakeLists.txt (removed 14 lines: comment block, fits_metadata.cpp, cfitsio block)
- test/CMakeLists.txt (removed 23 lines: legacy comment, nukex4_module_legacy_testlib, two test lines)

## Self-Review Findings

✅ **Completeness**: All six files deleted; no references remain
✅ **Discipline**: Edits exactly as specified in brief; nothing beyond scope
✅ **CMake Correctness**: 
   - filter_classifier.cpp was already removed by Task 14 (verified in commit)
   - fits_metadata.cpp removed from MODULE_SOURCES
   - cfitsio link removed and module still links (nukex4_io carries it)
   - Legacy testlib removed entirely (no longer needed)
   - Module testlib preserved with correct targets
✅ **Testing**: 
   - Module links without errors despite removing cfitsio link
   - 65/65 tests pass (was 67 before task, expecting 65 per brief)
   - No test regressions

## Issues or Concerns

None. Task complete as specified.
