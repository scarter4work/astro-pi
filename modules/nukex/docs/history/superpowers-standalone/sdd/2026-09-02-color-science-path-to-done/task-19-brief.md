### Task 19: Delete the module-local classifier and FITS-metadata reader

**State today.** After Task 14 nothing in `src/module` includes `filter_classifier.hpp` or `fits_metadata.hpp`. Both pairs, their two unit tests, and the module's direct cfitsio link exist only to serve each other.

**Files:**
- Delete: `src/module/filter_classifier.hpp`, `src/module/filter_classifier.cpp`, `src/module/fits_metadata.hpp`, `src/module/fits_metadata.cpp`, `test/unit/module/test_filter_classifier.cpp`, `test/unit/module/test_fits_metadata.cpp`
- Modify: `src/module/CMakeLists.txt:21-22,56-59`, `test/CMakeLists.txt` (module testlib block)

- [ ] **Step 1: Confirm no remaining includes**

Run: `grep -rnE '"filter_classifier.hpp"|"fits_metadata.hpp"|read_fits_metadata|FITSMetadata' src/ test/ | grep -vE "src/module/(filter_classifier|fits_metadata)\.|test/unit/module/test_(filter_classifier|fits_metadata)\.cpp"`
Expected: no output.

- [ ] **Step 2: Delete**

```bash
git rm src/module/filter_classifier.hpp src/module/filter_classifier.cpp \
       src/module/fits_metadata.hpp src/module/fits_metadata.cpp \
       test/unit/module/test_filter_classifier.cpp test/unit/module/test_fits_metadata.cpp
```

- [ ] **Step 3: CMake**

`src/module/CMakeLists.txt`: remove `fits_metadata.cpp` from `MODULE_SOURCES` (`filter_classifier.cpp` was already removed by Task 14); remove the cfitsio block at lines 56–59 (`# cfitsio: fits_metadata.cpp …` through `target_link_libraries(NukeX-pxm PRIVATE cfitsio)`) — `nukex4_io` carries cfitsio transitively.

`test/CMakeLists.txt`: Task 14 already split the module test library. Delete the ENTIRE `nukex4_module_legacy_testlib` OBJECT library block (its `add_library`, `target_include_directories`, `target_link_libraries` and the comment above it) — it exists only to keep the old classifier's objects out of links that hold `nukex4_core`; with the sources gone it has no purpose. Leave `nukex4_module_testlib` (stretch_auto_selector.cpp + stretch_factory.cpp, links nukex4_stretch nukex4_io) exactly as it is. Delete the `nukex_add_test(test_fits_metadata …)` and `nukex_add_test(test_filter_classifier …)` lines. Update the comment `# Module-layer tests (fits_metadata and future module code)` → `# Module-layer tests (auto-selector, stretch factory, Phase 8 fallback)`.

- [ ] **Step 4: Build + ctest**

Run: `cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error|undefined reference" ; ctest 2>&1 | tail -2`
Expected: no errors; total test count drops by 2; all pass.

- [ ] **Step 5: Commit**

```bash
git add -A src/module test/CMakeLists.txt test/unit/module
git commit -m "$(cat <<'EOF'
chore(module): delete module-local filter_classifier + fits_metadata

Both were superseded by lib/io FilterClassifier over FrameMetadata
(Task 14). Their unit tests, the module's direct cfitsio link, and the
test library sources go with them. Spec 4.3 dead-code list closeout.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

