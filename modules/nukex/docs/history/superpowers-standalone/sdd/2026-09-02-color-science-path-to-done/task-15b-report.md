# Task 15b Report: Camera resolution — INSTRUME normalisation + generic-camera fallback

## What I implemented

Exactly the brief, in its step order:

1. **`QEDatabase` (calibration lib):**
   - `static constexpr const char* kGenericOSCCamera = "generic_sony_imx_osc";`
   - `static std::string normalize_camera_key(const std::string&)` — lowercase, alphanumerics only.
   - `std::string resolve_camera(const std::string& instrume) const` — exact normalised match first, else the longest DB key contained in the normalised INSTRUME, else `""`.
   - `parse_and_merge` now stores `cameras_[normalize_camera_key(name)]`.
   - `has_camera`, `confidence`, `lookup_camera_qe` now normalise their `camera`/`name` argument before the map lookup.
   - Filters untouched (canonical names are exact by construction, per brief).

2. **`StackingEngine::execute` (stacking_engine.cpp):**
   - Added `std::set<std::string> unknown_instrume_warned;` next to `dual_nb_cameras`.
   - The DUAL_NB_OSC camera-tracking block now calls `qe_database_->resolve_camera(frame_filter.camera)`; on a miss it falls back to `QEDatabase::kGenericOSCCamera`, sets `result.qe_generic_camera_fallback = true`, and emits one `obs.message(...)` Process Console warning per distinct raw INSTRUME string (gated by `unknown_instrume_warned.insert(...).second`). `dual_nb_cameras` now holds resolved DB keys instead of raw header strings, so the existing mixed-camera guard and `decomposer_->build_q(q_build_camera, ...)` compare DB identities.

3. **`ExecuteResult` (stacking_engine.hpp):** added `bool qe_generic_camera_fallback = false;` after `n_frames_rejected_filter`.

4. **`NukeXInstance.cpp`:** appended a `NUKEX_QE_CONFIDENCE` FITS keyword (`"generic-fallback"` or `"database"`) to the composed-window keyword array, directly after the existing `NUKEX_GAMUT_CLIPPED` append.

5. **Fixtures:**
   - `test/fixtures/qe/override_camera_pro.json` — new file, `ASI2600MC-Pro` camera, verbatim from the brief.
   - `test/fixtures/qe/minimal_db.json` — added a `generic_sony_imx_osc` camera block, a copy of `ASI585MC`'s QE curve with `"sensor": "generic"` and `"confidence": "low"`.

6. **Tests:** appended the four brief-specified `TEST_CASE`s to `test/unit/calibration/test_qe_database.cpp` (normalisation, case-insensitive lookup, `resolve_camera` exact/substring/no-match, and longest-contained-key preference against an override).

No `CMakeLists.txt` changes were needed — no new test binaries, no new source files added to a source list (the new fixture is a data file, not a compiled source).

## What I tested and results

### TDD evidence — RED

Command:
```
cd build && make test_qe_database 2>&1 | grep -E "error" | head -3
```
Output:
```
/home/scarter4work/projects/nukex5/test/unit/calibration/test_qe_database.cpp:81:25: error: ‘normalize_camera_key’ is not a member of ‘nukex::QEDatabase’
/home/scarter4work/projects/nukex5/test/unit/calibration/test_qe_database.cpp:81:25: error: ‘normalize_camera_key’ is not a member of ‘nukex::QEDatabase’
/home/scarter4work/projects/nukex5/test/unit/calibration/test_qe_database.cpp:81:25: error: ‘normalize_camera_key’ is not a member of ‘nukex::QEDatabase’
```
Matches the brief's predicted failure (compile error on the not-yet-existing member). Expected because Step 1 added test bodies calling `normalize_camera_key`/`resolve_camera` before Step 3 implemented them.

### TDD evidence — GREEN (calibration layer, Step 4)

Command:
```
cd build && make test_qe_database test_channel_decomposer 2>&1 | grep -E "error" ; ctest -R "qe_database|decomposer" --output-on-failure 2>&1 | tail -3
```
Output:
```
100% tests passed, 0 tests failed out of 2

Total Test time (real) =   0.00 sec
```
No compile errors; both binaries pass. Also ran the binaries directly for full pristine-output confirmation:
```
./test/test_qe_database         -> All tests passed (46 assertions in 10 test cases)
./test/test_channel_decomposer  -> All tests passed (24 assertions in 7 test cases)
```
(`test_channel_decomposer` unchanged in count/behavior — confirms `build_q("ASI585MC", …)` still works through the now-normalising lookups.)

### Full build + suite (Step 7)

Command:
```
cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -3
```
`make` produced zero lines matching `error` (module `NukeX-pxm.so` linked successfully). `ctest`:
```
100% tests passed, 0 tests failed out of 67

Total Test time (real) =  60.69 sec
```
67/67 — same binary count as pre-task, per the brief's expectation.

## Files changed

- `src/lib/calibration/include/nukex/calibration/qe_database.hpp`
- `src/lib/calibration/src/qe_database.cpp`
- `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`
- `src/lib/stacker/src/stacking_engine.cpp`
- `src/module/NukeXInstance.cpp`
- `test/unit/calibration/test_qe_database.cpp`
- `test/fixtures/qe/minimal_db.json` (added `generic_sony_imx_osc` camera block)
- `test/fixtures/qe/override_camera_pro.json` (new)

Commit: `d1a2daa` — "fix(calibration): resolve FITS INSTRUME onto QE DB keys; generic OSC fallback per spec 6.3" on branch `v5-color-science`.

## Self-review findings

- **Completeness:** all 8 steps done in order. Both fixture files present and correct. Warning fires once per distinct raw INSTRUME string (`unknown_instrume_warned` keyed on `frame_filter.camera`, the un-normalised header value, so two different misspellings of the same camera each still warn once — matches "once per distinct INSTRUME" literally). The flag (`ExecuteResult::qe_generic_camera_fallback`) reaches the module and is stamped as `NUKEX_QE_CONFIDENCE`.
- **Verified no other `cameras_` access site was missed:** grepped `cameras_` in `qe_database.cpp` — all four public accessors (`has_camera`, `confidence`, `lookup_camera_qe`, plus the new `resolve_camera`) and the one write site (`parse_and_merge`) are consistent; no raw-key access remains.
- **Verified scope boundary:** the shipped production `share/qe_database.json` does not yet exist in the repo (default path `share/qe_database.json` is unopened), and populating it with a real `generic_sony_imx_osc` entry is `tools/import_qe_research.py`'s job (already shipped in a prior commit, confirmed present at `tools/import_qe_research.py:30` `GENERIC_OSC_KEY = "generic_sony_imx_osc"`) — correctly out of this task's file list, not touched.
- **Discipline:** diff is confined to the brief's file list; nothing added beyond the specified interfaces/tests/fixtures.
- **Quality:** module-side edit matches surrounding PCL style (3-space indent, spaces inside parens); lib/test edits use the existing 4-space style.

No issues found; no rework needed.

## Issues or concerns

None. The engine and module changes only touch the DUAL_NB_OSC code path, matching the brief's stated scope — broadband/single-line frames are unaffected, as before.

---

## Fix report (review round 1)

Controller review found two issues in `QEDatabase`, both traceable to the brief's own reference code, and asked for both to be fixed:

### 1. `resolve_camera` — non-deterministic tie-break on equal-length contained keys

**What changed.** The scan over `cameras_` now prefers a longer contained key as before, but on an equal-length tie it takes the lexicographically smaller key instead of whichever `unordered_map` iteration happened to visit second. Header comment for `resolve_camera` documents the tie-break rule explicitly.

`qe_database.cpp`, in `resolve_camera`:
```cpp
for (const auto& kv : cameras_) {
    if (key.find(kv.first) == std::string::npos) continue;
    if (kv.first.size() > best.size() ||
        (kv.first.size() == best.size() && kv.first < best)) {
        best = kv.first;
    }
}
```

**Covering test:** `QEDatabase: resolve_camera breaks equal-length ties lexicographically` (`test_qe_database.cpp`), using new fixture `test/fixtures/qe/override_equal_length.json` (adds `ZWO-ABD1` -> `zwoabd1` and `ZWO-ABC1` -> `zwoabc1`, both 7 chars after normalisation). Instrume `"ZWO ABC1 ZWO ABD1"` normalises to `"zwoabc1zwoabd1"`, which contains both keys as substrings; asserts `resolve_camera(...) == "zwoabc1"` (the lexicographically smaller of the tied pair).

### 2. `parse_and_merge` — same-document normalised-key collision silently overwrote

**What changed.** Cameras in one JSON document are now parsed into a local `parsed_cameras` map first, tracking `normalised key -> raw name` seen so far (`normalized_to_raw`). If a second, differently-spelled raw name normalises to a key already seen **within that same document**, `parse_and_merge` returns `{false, "<context>: camera keys '<raw A>' and '<raw B>' both normalise to '<norm>'"}` immediately, before touching `cameras_` — so a failed load never half-applies. Only after the whole document parses without a same-document collision does the local map get merged into `cameras_` (this merge step is where override-wins-on-cross-load-collision semantics still apply, unchanged).

**Covering test:** `QEDatabase: colliding normalised camera keys within one document -> loud fail` (`test_qe_database.cpp`), using new fixture `test/fixtures/qe/collision.json` (`"ASI-585MC"` and `"ASI585 MC"`, both normalising to `asi585mc`). Asserts `load_override(...).ok == false`, the error message contains both raw spellings, and — critically — that `has_camera("asi585mc")` and its QE value still reflect the shipped DB afterward (the failed override left prior state untouched).

**Confirmed the existing cross-load override test still passes unmodified:** `QEDatabase: override merge, override wins on collision` (unchanged) continues to pass — overriding `ASI585MC` from a *different* load call is still override-wins, not a same-document collision.

### TDD evidence

**RED** — command:
```
cd build && make test_qe_database 2>&1 | tail -20   # compiles clean, no new API surface
cd build && ./test/test_qe_database "[qe_database]"
```
Relevant output (both new cases failing against the pre-fix code):
```
QEDatabase: resolve_camera breaks equal-length ties lexicographically
test/unit/calibration/test_qe_database.cpp:120: FAILED:
  REQUIRE( db.resolve_camera("ZWO ABC1 ZWO ABD1") == "zwoabc1" )
with expansion:
  "zwoabd1" == "zwoabc1"

QEDatabase: colliding normalised camera keys within one document -> loud fail
test/unit/calibration/test_qe_database.cpp:132: FAILED:
  REQUIRE_FALSE( r.ok )
with expansion:
  !true

test cases: 12 | 10 passed | 2 failed
assertions: 52 | 50 passed | 2 failed
```
Matches expectation: the tie-break was previously whatever the map order produced (observed as `"zwoabd1"`, the wrong/non-deterministic pick), and the collision case silently succeeded (`r.ok == true`) with the second camera clobbering the first.

**GREEN** — command:
```
cd build && make test_qe_database test_channel_decomposer 2>&1 | grep -E "error"   # no output
cd build && ./test/test_qe_database
cd build && ./test/test_channel_decomposer
```
Output:
```
All tests passed (56 assertions in 12 test cases)   # test_qe_database
All tests passed (24 assertions in 7 test cases)    # test_channel_decomposer (unaffected)
```

**Full suite** — command:
```
cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -10
```
Output:
```
100% tests passed, 0 tests failed out of 67

Total Test time (real) =  60.52 sec
```
67/67 — same binary count, output pristine.

### Files changed (this fix)

- `src/lib/calibration/include/nukex/calibration/qe_database.hpp` (comment update only)
- `src/lib/calibration/src/qe_database.cpp` (`resolve_camera` tie-break, `parse_and_merge` collision detection)
- `test/unit/calibration/test_qe_database.cpp` (two new test cases)
- `test/fixtures/qe/override_equal_length.json` (new)
- `test/fixtures/qe/collision.json` (new)

Commit: `fix(calibration): deterministic camera tie-break; loud normalised-key collision on load` on branch `v5-color-science` (separate commit from the original Task 15b commit `d1a2daa`).
