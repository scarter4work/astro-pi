# Task 14 Report: Migrate `lastRun.filter_class` + `stretch_auto_selector` to the 5-class enum; rating DB `user_version` 1 → 2

## What I implemented

Followed the brief step-by-step (Steps 1–13), plus one unplanned but team-lead-approved fix (see "Unplanned deviation" below).

1. **Rating DB schema versioning + migration** (`src/lib/learning/include/nukex/learning/rating_db.hpp`, `src/lib/learning/src/rating_db.cpp`):
   - Added `kRatingDbSchemaVersion = 2` and `rating_db_schema_version(sqlite3*)`.
   - Removed the `PRAGMA user_version = 1;` re-stamp from `kSchemaV1` (it was undoing migrations on every open).
   - `apply_pragmas_and_schema` now reads the current `user_version` before applying schema DDL, and dispatches: `0` → stamp `kRatingDbSchemaVersion` (brand-new file), `1` → `migrate_v1_to_v2` (one `BEGIN IMMEDIATE; UPDATE ... CASE ...; PRAGMA user_version=2; COMMIT;` transaction), anything else → left untouched.
   - Added the public `rating_db_schema_version` accessor after `close_rating_db`.

2. **Module migrated to the 5-class enum + lib classifier**:
   - `src/module/stretch_auto_selector.{hpp,cpp}`: now takes `nukex::FrameMetadata` and uses `nukex::FilterClassifier` (`nukex/io/filter_classifier.hpp`) instead of the module-local `FITSMetadata`/`classify_filter`.
   - `src/module/stretch_factory.{hpp,cpp}`: `build_primary` now takes `const FrameMetadata&`.
   - `src/module/NukeXInstance.cpp`: includes swapped to `nukex/io/filter_classifier.hpp` and `nukex/io/fits_reader.hpp`; `filter_class_to_rating_int` rewritten for the 5-class enum (1 BROADBAND_L .. 5 DUAL_NB_OSC, 0 UNKNOWN); `ExecuteGlobal` now reads headers via `FITSReader::read_headers` and classifies via `FilterClassifier`.
   - `src/module/RatingDialog.{h,cpp}`: added `static bool has_color_axis(int)` (true for 2 BROADBAND_RGB / 3 BROADBAND_OSC), replacing the old `filter_class_ == 1` check.

3. **Tests migrated**: `test_stretch_auto_selector.cpp` rewritten per the brief (7 cases covering both overloads and classifier-driven classification, including the "unknown filter" warning path); `test_stretch_factory.cpp` had its include and all `FITSMetadata` → `FrameMetadata` via the brief's exact `sed`.

## Unplanned deviation (approved by team lead)

Step 12's full build/ctest run surfaced a genuine ODR symbol collision, not an implementation mistake: `src/module/filter_classifier.cpp` (old, untouched per the brief) and `src/lib/core/src/filter.cpp` (new) both define an externally-linked, non-static `nukex::filter_class_name(nukex::FilterClass)`. Despite being different enums, C++ name mangling only encodes the qualified type name, so both compile to the identical symbol `_ZN5nukex17filter_class_nameENS_11FilterClassE`. Whichever object file the linker sees first in a link wins silently — no error, no warning.

Verified with `nm`/`objdump` (not guessed):
- `test_stretch_auto_selector` (6 of 7 cases failing): the built binary's `filter_class_name` disassembled to the OLD 4-case switch (`cmp $0x1/2/3`), because `nukex4_module_testlib` compiled `filter_classifier.cpp` (old) and `stretch_auto_selector.cpp` (new-header caller) into the same OBJECT library / test binary link, and the old object file (given directly to the linker) pre-empted the new definition that would otherwise have been pulled from the `nukex4_core` static archive.
- **Production**: `src/module/CMakeLists.txt`'s `MODULE_SOURCES` still listed `filter_classifier.cpp` directly alongside the migrated `stretch_auto_selector.cpp`. Disassembling the actual built `NukeX-pxm.so` showed the same old 4-case body winning — meaning the shipped module would have printed a wrong "Auto: classified as ..." string in the Process Console (e.g. "LRGB_COLOR" instead of "BROADBAND_L"). This did not corrupt the persisted rating-DB `filter_class` int, since `filter_class_to_rating_int` switches on the enum value directly with no call through the colliding symbol.
- Confirmed nothing else in the module still called the old `classify_filter`/`filter_class_name` except `filter_classifier.cpp`'s own file and its own untouched unit test.

I stopped and messaged the team lead with this evidence and three options rather than guessing at a fix that touched files outside the brief's list. Ruling: do both —
- **(b)** Removed `filter_classifier.cpp` from `MODULE_SOURCES` in `src/module/CMakeLists.txt` (file/header left untouched on disk; nothing else calls it after this task; Task 19 deletes it).
- **(a)** Split `nukex4_module_testlib` in `test/CMakeLists.txt` into `nukex4_module_legacy_testlib` (`fits_metadata.cpp` + `filter_classifier.cpp`, links `cfitsio`; used only by `test_fits_metadata` / `test_filter_classifier`) and `nukex4_module_testlib` (`stretch_auto_selector.cpp` + `stretch_factory.cpp`, links `nukex4_stretch nukex4_io`; used by `test_stretch_auto_selector` / `test_stretch_factory`). `test_phase8_fallback` was already independent of this library and needed no change.

Post-fix verification (again via `nm`/`objdump`, not assumed): both the test binary and the rebuilt `NukeX-pxm.so` now disassemble `filter_class_name` to the new 6-case implementation (`cmp $0x5` + jump table + `.cold` `abort()` clone for the unmatched case).

## What I tested and results

### TDD evidence — rating DB migration (Steps 1–6)

**RED** — `cd build && cmake .. > /dev/null && make test_rating_db_migration 2>&1 | grep -E "error" | head -3`:
```
/home/scarter4work/projects/nukex5/test/unit/learning/test_rating_db_migration.cpp:86:13: error: ‘rating_db_schema_version’ was not declared in this scope
/home/scarter4work/projects/nukex5/test/unit/learning/test_rating_db_migration.cpp:86:45: error: ‘kRatingDbSchemaVersion’ was not declared in this scope
/home/scarter4work/projects/nukex5/test/unit/learning/test_rating_db_migration.cpp:86:13: error: ‘rating_db_schema_version’ was not declared in this scope
```
Expected exactly this (the API didn't exist yet).

**GREEN** — `cd build && make test_rating_db test_rating_db_migration test_train_model -j$(nproc) 2>&1 | grep -E "error|warning: unused" ; ctest -R "rating_db|train_model" --output-on-failure`:
```
    Start 52: test_rating_db
1/3 Test #52: test_rating_db ...................   Passed    0.00 sec
    Start 53: test_rating_db_migration
2/3 Test #53: test_rating_db_migration .........   Passed    0.00 sec
    Start 54: test_train_model
3/3 Test #54: test_train_model .................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 3
```

### ODR-collision diagnosis evidence (ad hoc, not part of the brief's steps)

- `nm -C` on `filter_classifier.cpp.o`, `stretch_auto_selector.cpp.o`, and `filter.cpp.o` showed two `T`-type (strong, external) definitions of `nukex::filter_class_name(nukex::FilterClass)` feeding the same test binary.
- `objdump -d` on the pre-fix `test_stretch_auto_selector` binary and pre-fix `NukeX-pxm.so` both showed the OLD 4-case switch body at the resolved symbol address.
- After the CMake fix: `objdump -d` on the rebuilt `test_stretch_auto_selector` and rebuilt `NukeX-pxm.so` both showed the NEW 6-case switch (`cmp $0x5`, jump table, `.cold` abort clone).

### Full build + ctest (Step 12, re-run twice: once immediately after the CMake fix, once after a forced touch-rebuild of every file I edited)

`cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error|Error" ; ctest 2>&1 | tail -3`:
```
100% tests passed, 0 tests failed out of 67

Total Test time (real) =  61.77 sec
```
No `error`/`Error` lines from `make`. Repeated after touching every file I modified to force a clean rebuild of just those translation units — zero warnings from any touched file (confirmed via `grep -B2 "warning:"` excluding the two pre-existing, untouched old-module test files which already carried unrelated `-Wrange-loop-construct` warnings from before this task).

## TDD evidence summary
- RED: command + failing compiler output captured above, matches the brief's expected error exactly.
- GREEN: command + passing ctest output captured above, matches the brief's expected "3 passed" and, at the end, the brief's expected "67/67 passed".

## Files changed
- `src/lib/learning/include/nukex/learning/rating_db.hpp`
- `src/lib/learning/src/rating_db.cpp`
- `src/module/CMakeLists.txt` (unplanned, team-lead-approved: drop `filter_classifier.cpp` from `MODULE_SOURCES`)
- `src/module/NukeXInstance.cpp`
- `src/module/RatingDialog.cpp`, `src/module/RatingDialog.h`
- `src/module/stretch_auto_selector.cpp`, `src/module/stretch_auto_selector.hpp`
- `src/module/stretch_factory.cpp`, `src/module/stretch_factory.hpp`
- `test/CMakeLists.txt` (register migration test; unplanned, team-lead-approved: split `nukex4_module_testlib` into legacy + new OBJECT libraries)
- `test/unit/learning/test_rating_db.cpp`
- `test/unit/learning/test_rating_db_migration.cpp` (new)
- `test/unit/module/test_stretch_auto_selector.cpp`
- `test/unit/module/test_stretch_factory.cpp`

Untouched, as instructed: `src/module/filter_classifier.{hpp,cpp}`, `src/module/fits_metadata.{hpp,cpp}`, `test/unit/module/test_filter_classifier.cpp`, `test/unit/module/test_fits_metadata.cpp`.

## Self-review

- **Completeness**: every step of the brief (1–13) executed; all cited line numbers verified against HEAD before editing and matched exactly. Old `FITSMetadata`/old-classifier includes are gone from all four migrated files (`NukeXInstance.cpp`, `stretch_auto_selector.{hpp,cpp}`, `stretch_factory.{hpp,cpp}`).
- **Quality**: names and structure follow the brief verbatim; module files kept PCL 3-space style, lib/test files kept 4-space style, matching each file's existing convention.
- **Discipline**: no changes beyond the brief's file list except the two CMake files, which were explicitly proposed, evidenced, and approved by the team lead before I touched them. `src/module/CMakeLists.txt` was not on my original file list but the ODR-collision fix required it and was pre-approved.
- **Testing**: TDD RED/GREEN captured for the migration piece; the module-side migration was verified by full-suite ctest (67/67) plus targeted `nm`/`objdump` verification that the *correct* implementation (not just "a passing test") is now linked into both the test binary and the actual `NukeX-pxm.so`. No warnings introduced in any touched file.

## Issues or concerns

None outstanding. The ODR collision is now fully resolved in both test and production artifacts, confirmed by disassembly rather than assumed from green tests alone. `filter_classifier.{hpp,cpp}` and `fits_metadata.{hpp,cpp}` remain on disk exactly as before, per Task 19's ownership of their deletion.
