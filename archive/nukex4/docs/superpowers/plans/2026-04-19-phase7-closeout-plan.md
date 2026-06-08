# Phase 7 Close-Out Implementation Plan — v4.0.0.4

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **PJSR validation:** For every PJSR snippet in this plan, validate the API calls against the `pjsr` MCP server's `pjsr-analyze` tool before running. This catches missing classes, wrong argument counts, and renamed methods without waiting for PI to fail at runtime. The MCP is configured in `.mcp.json` at the repo root.

**Goal:** Ship NukeX v4.0.0.4 — wire the stretch pipeline into the module output with metadata-driven auto-selection, profile Phase B and land one measured ≥ 1.5× optimization, and build an automated PJSR-driven E2E validation harness that enforces bitwise regression across a 4-stack corpus.

**Architecture:** Four phases executed sequentially. Phase A introduces new parameter schema (`primaryStretch` / `finishingStretch`) and wires `StretchPipeline` into `NukeXInstance::ExecuteGlobal()` behind a new stretch factory + metadata-driven auto-selector. Phase B adds one `std::chrono` timer around `execute_phase_b()`, profiles via Linux `perf record`, and lands a measured optimization. Phase C creates a PJSR test harness driven by `PixInsight.sh --automation-mode --force-exit -r=tools/validate_e2e.js`, a 4-stack gating corpus, golden SHA-256 fixtures for bitwise regression, and a `make e2e` CMake target. Phase D performs the standard PI release workflow: version bump, build, sign, package, push.

**Tech Stack:** C++17, CMake, Catch2 (tests), PCL SDK (`MetaEnumeration`, `MetaBoolean`, `ImageWindow`, `ProcessImplementation`), Ceres Solver, OpenCL, cfitsio (vendored), PJSR (for E2E), Linux `perf` + FlameGraph scripts.

**Spec reference:** `docs/superpowers/specs/2026-04-19-phase7-closeout-design.md` (commit `8dc3cd3`).

**Worktree note:** This plan can run in the main working tree or a dedicated worktree. If you want isolation (safer for a multi-phase release), create one with `git worktree add ../nukex4-phase7 -b phase7-closeout` before starting. The plan references absolute paths so either works.

---

## Phase A — Stretch wiring + metadata-driven Auto

New files this phase creates:
- `src/module/fits_metadata.hpp` + `.cpp` — FITS header extraction (`FILTER`, `BAYERPAT`, `NAXIS3`) from a single light frame path.
- `src/module/filter_classifier.hpp` + `.cpp` — classifies metadata into `LRGB-mono | LRGB-color | Bayer-RGB | Narrowband`.
- `src/module/stretch_auto_selector.hpp` + `.cpp` — dispatches filter class → champion `StretchOp`.
- `src/module/stretch_factory.hpp` + `.cpp` — builds primary / finishing ops from the new enum parameters.
- `test/unit/module/test_fits_metadata.cpp`, `test_filter_classifier.cpp`, `test_stretch_auto_selector.cpp`, `test_stretch_factory.cpp`.

Files this phase modifies:
- `src/module/NukeXParameters.h` — replace `NXStretchType` + `NXAutoStretch` with `NXPrimaryStretch` + `NXFinishingStretch`.
- `src/module/NukeXParameters.cpp` — matching implementations.
- `src/module/NukeXProcess.cpp` — new parameter registration.
- `src/module/NukeXInstance.h` — replace storage fields `stretchType` + `autoStretch` with `primaryStretch` + `finishingStretch`.
- `src/module/NukeXInstance.cpp` — update `Assign`, `LockParameter`; insert stretch pipeline in `ExecuteGlobal`; add third `NukeX_stretched` output window.
- `src/module/NukeXInterface.cpp` — swap dropdown bindings and add a second dropdown for finishing.
- `src/module/NukeXInterface.h` — add second `ComboBox` member.
- `src/module/CMakeLists.txt` — add new `.cpp` files to the `NukeX` target.
- `test/CMakeLists.txt` — register the four new test executables.

---

### Task A1: FITSMetadata struct + extraction header

**Files:**
- Create: `src/module/fits_metadata.hpp`

- [ ] **Step 1: Write the header.**

Create `src/module/fits_metadata.hpp`:

```cpp
// NukeX v4 — FITS metadata extraction for module-layer stretch selection.
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeX_fits_metadata_h
#define __NukeX_fits_metadata_h

#include <string>

namespace nukex {

struct FITSMetadata {
    std::string filter;     // FITS "FILTER" keyword, uppercased, trimmed. Empty if absent.
    std::string bayer_pat;  // FITS "BAYERPAT" keyword, uppercased, trimmed. Empty if absent.
    int naxis3 = 1;         // FITS "NAXIS3" keyword. Defaults to 1 (mono) if absent.
    bool read_ok = false;   // True if the file opened and header read without error.
};

/// Read the primary HDU header of a FITS file at `path` and return its
/// metadata. On I/O or parse error, returns a default-constructed
/// FITSMetadata with read_ok=false.
FITSMetadata read_fits_metadata(const std::string& path);

} // namespace nukex

#endif // __NukeX_fits_metadata_h
```

- [ ] **Step 2: Commit.**

```bash
cd /home/scarter4work/projects/nukex4
git add src/module/fits_metadata.hpp
git commit -m "feat(module): FITSMetadata struct + read API declaration"
```

---

### Task A2: FITSMetadata implementation + tests

**Files:**
- Create: `src/module/fits_metadata.cpp`
- Create: `test/unit/module/test_fits_metadata.cpp`
- Modify: `src/module/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing test first.**

Create `test/unit/module/test_fits_metadata.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "fits_metadata.hpp"
#include <cstdlib>

using nukex::FITSMetadata;
using nukex::read_fits_metadata;

TEST_CASE("read_fits_metadata: missing file returns read_ok=false", "[module][fits_metadata]") {
    auto meta = read_fits_metadata("/nonexistent/path/does_not_exist.fits");
    REQUIRE(meta.read_ok == false);
    REQUIRE(meta.filter.empty());
    REQUIRE(meta.bayer_pat.empty());
    REQUIRE(meta.naxis3 == 1);
}

TEST_CASE("read_fits_metadata: live FITS file parses OK", "[module][fits_metadata]") {
    const char* env = std::getenv("NUKEX_TEST_FITS_LRGB_MONO");
    if (!env) {
        WARN("NUKEX_TEST_FITS_LRGB_MONO unset — skipping live-read test");
        return;
    }
    auto meta = read_fits_metadata(env);
    REQUIRE(meta.read_ok == true);
    REQUIRE(meta.naxis3 >= 1);
    REQUIRE(meta.naxis3 <= 3);
}
```

- [ ] **Step 2: Run it to verify it fails.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . --target test_fits_metadata 2>&1 | tail -10
```

Expected: link error `undefined reference to nukex::read_fits_metadata`.

- [ ] **Step 3: Implement `fits_metadata.cpp`.**

Create `src/module/fits_metadata.cpp`:

```cpp
// NukeX v4 — FITS metadata extraction via cfitsio (already vendored).
// Copyright (c) 2026 Scott Carter. MIT License.

#include "fits_metadata.hpp"

#include <fitsio.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace nukex {

namespace {

std::string normalize(const char* raw) {
    if (!raw) return {};
    std::string s(raw);
    auto is_junk = [](char c) {
        return c == '\'' || c == '"' || std::isspace(static_cast<unsigned char>(c));
    };
    while (!s.empty() && is_junk(s.front())) s.erase(s.begin());
    while (!s.empty() && is_junk(s.back()))  s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

} // namespace

FITSMetadata read_fits_metadata(const std::string& path) {
    FITSMetadata meta;
    fitsfile* fptr = nullptr;
    int status = 0;

    if (fits_open_file(&fptr, path.c_str(), READONLY, &status) != 0) {
        return meta;  // read_ok stays false
    }

    char value[FLEN_VALUE]    = {0};
    char comment[FLEN_COMMENT] = {0};

    int local_status = 0;
    if (fits_read_key(fptr, TSTRING, "FILTER", value, comment, &local_status) == 0) {
        meta.filter = normalize(value);
    }

    local_status = 0;
    std::memset(value, 0, sizeof(value));
    if (fits_read_key(fptr, TSTRING, "BAYERPAT", value, comment, &local_status) == 0) {
        meta.bayer_pat = normalize(value);
    }

    long naxis3 = 1;
    local_status = 0;
    if (fits_read_key(fptr, TLONG, "NAXIS3", &naxis3, comment, &local_status) == 0) {
        meta.naxis3 = static_cast<int>(naxis3);
    } else {
        meta.naxis3 = 1;
    }

    fits_close_file(fptr, &status);
    meta.read_ok = true;
    return meta;
}

} // namespace nukex
```

- [ ] **Step 4: Add to CMake.**

Edit `src/module/CMakeLists.txt` — append `fits_metadata.cpp` to the module source list.

Edit `test/CMakeLists.txt` — add a module test lib OBJECT target and register the test:

```cmake
add_library(nukex4_module_testlib OBJECT
    "${CMAKE_SOURCE_DIR}/src/module/fits_metadata.cpp"
)
target_include_directories(nukex4_module_testlib PUBLIC
    "${CMAKE_SOURCE_DIR}/src/module"
    "${CMAKE_SOURCE_DIR}/src/lib/io/include"
)
target_link_libraries(nukex4_module_testlib PUBLIC cfitsio)

nukex_add_test(test_fits_metadata unit/module/test_fits_metadata.cpp nukex4_module_testlib)
```

- [ ] **Step 5: Build + test.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. && cmake --build . --target test_fits_metadata -j$(nproc)
ctest --output-on-failure -R test_fits_metadata
```

Expected: PASS.

- [ ] **Step 6: Set env var and re-run for live coverage.**

```bash
export NUKEX_TEST_FITS_LRGB_MONO="$(ls ~/projects/processing/*/L/*.fit 2>/dev/null | head -1)"
echo "Using: $NUKEX_TEST_FITS_LRGB_MONO"
cd /home/scarter4work/projects/nukex4/build && ctest --output-on-failure -R test_fits_metadata
```

Expected: PASS with live-read test actually running.

- [ ] **Step 7: Commit.**

```bash
cd /home/scarter4work/projects/nukex4
git add src/module/fits_metadata.cpp src/module/CMakeLists.txt \
        test/unit/module/test_fits_metadata.cpp test/CMakeLists.txt
git commit -m "feat(module): FITSMetadata implementation + unit tests"
```

---

### Task A3: FilterClassifier

**Files:**
- Create: `src/module/filter_classifier.hpp`
- Create: `src/module/filter_classifier.cpp`
- Create: `test/unit/module/test_filter_classifier.cpp`
- Modify: `src/module/CMakeLists.txt`, `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing test.**

Create `test/unit/module/test_filter_classifier.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "filter_classifier.hpp"

using nukex::FITSMetadata;
using nukex::FilterClass;
using nukex::classify_filter;

TEST_CASE("classify_filter: NAXIS3=3 no BAYERPAT -> LRGB_COLOR", "[module][filter_classifier]") {
    FITSMetadata m;
    m.naxis3 = 3;
    REQUIRE(classify_filter(m) == FilterClass::LRGB_COLOR);
}

TEST_CASE("classify_filter: BAYERPAT present -> BAYER_RGB", "[module][filter_classifier]") {
    FITSMetadata m;
    m.naxis3 = 1;
    m.bayer_pat = "RGGB";
    REQUIRE(classify_filter(m) == FilterClass::BAYER_RGB);
}

TEST_CASE("classify_filter: narrowband names -> NARROWBAND", "[module][filter_classifier]") {
    for (const std::string& name : {"HA", "H-ALPHA", "HALPHA", "OIII", "O3", "SII", "S2", "NARROWBAND"}) {
        FITSMetadata m;
        m.filter = name;
        REQUIRE(classify_filter(m) == FilterClass::NARROWBAND);
    }
}

TEST_CASE("classify_filter: mono LRGB filters -> LRGB_MONO", "[module][filter_classifier]") {
    for (const std::string& name : {"L", "LUM", "R", "G", "B", "RED", "GREEN", "BLUE"}) {
        FITSMetadata m;
        m.filter = name;
        REQUIRE(classify_filter(m) == FilterClass::LRGB_MONO);
    }
}

TEST_CASE("classify_filter: empty metadata -> LRGB_MONO (safe default)",
          "[module][filter_classifier]") {
    FITSMetadata m;
    REQUIRE(classify_filter(m) == FilterClass::LRGB_MONO);
}

TEST_CASE("classify_filter: BAYERPAT wins over NAXIS3=3",
          "[module][filter_classifier]") {
    FITSMetadata m;
    m.naxis3 = 3;
    m.bayer_pat = "RGGB";
    REQUIRE(classify_filter(m) == FilterClass::BAYER_RGB);
}
```

- [ ] **Step 2: Write the header.**

Create `src/module/filter_classifier.hpp`:

```cpp
// NukeX v4 — Classifies FITS metadata into one of four filter classes
// used for stretch Auto-selection.
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeX_filter_classifier_h
#define __NukeX_filter_classifier_h

#include "fits_metadata.hpp"

namespace nukex {

enum class FilterClass {
    LRGB_MONO,
    LRGB_COLOR,
    BAYER_RGB,
    NARROWBAND,
};

FilterClass classify_filter(const FITSMetadata& meta);
const char* filter_class_name(FilterClass c);

} // namespace nukex

#endif
```

- [ ] **Step 3: Write the implementation.**

Create `src/module/filter_classifier.cpp`:

```cpp
#include "filter_classifier.hpp"
#include <set>

namespace nukex {

namespace {

bool is_narrowband_name(const std::string& filter) {
    static const std::set<std::string> names{
        "HA", "H-ALPHA", "HALPHA", "H_ALPHA",
        "OIII", "O3", "O-III", "O_III",
        "SII", "S2", "S-II", "S_II",
        "NARROWBAND", "NB",
    };
    return names.find(filter) != names.end();
}

} // namespace

FilterClass classify_filter(const FITSMetadata& meta) {
    if (!meta.bayer_pat.empty()) return FilterClass::BAYER_RGB;
    if (is_narrowband_name(meta.filter)) return FilterClass::NARROWBAND;
    if (meta.naxis3 == 3) return FilterClass::LRGB_COLOR;
    return FilterClass::LRGB_MONO;
}

const char* filter_class_name(FilterClass c) {
    switch (c) {
        case FilterClass::LRGB_MONO:  return "LRGB-mono";
        case FilterClass::LRGB_COLOR: return "LRGB-color";
        case FilterClass::BAYER_RGB:  return "Bayer-RGB";
        case FilterClass::NARROWBAND: return "Narrowband";
    }
    return "unknown";
}

} // namespace nukex
```

- [ ] **Step 4: Add to build + test.**

Edit `src/module/CMakeLists.txt` — add `filter_classifier.cpp`.

Edit `test/CMakeLists.txt` — extend the `nukex4_module_testlib` OBJECT target to include `filter_classifier.cpp` and add:

```cmake
nukex_add_test(test_filter_classifier unit/module/test_filter_classifier.cpp nukex4_module_testlib)
```

- [ ] **Step 5: Build + run.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. && cmake --build . --target test_filter_classifier -j$(nproc)
ctest --output-on-failure -R test_filter_classifier
```

Expected: all 6 cases PASS.

- [ ] **Step 6: Commit.**

```bash
git add src/module/filter_classifier.hpp src/module/filter_classifier.cpp \
        src/module/CMakeLists.txt \
        test/unit/module/test_filter_classifier.cpp test/CMakeLists.txt
git commit -m "feat(module): FilterClass + classify_filter with tests"
```

---

### Task A4: StretchAutoSelector

**Files:**
- Create: `src/module/stretch_auto_selector.hpp`
- Create: `src/module/stretch_auto_selector.cpp`
- Create: `test/unit/module/test_stretch_auto_selector.cpp`
- Modify: `src/module/CMakeLists.txt`, `test/CMakeLists.txt`

- [ ] **Step 1: Write the failing test.**

Create `test/unit/module/test_stretch_auto_selector.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "stretch_auto_selector.hpp"
#include "filter_classifier.hpp"
#include "nukex/stretch/veralux_stretch.hpp"

using namespace nukex;

TEST_CASE("select_auto: LRGB_MONO picks VeraLux and logs class name", "[module][auto_selector]") {
    auto sel = select_auto(FilterClass::LRGB_MONO);
    REQUIRE(sel.op != nullptr);
    REQUIRE(dynamic_cast<VeraluxStretch*>(sel.op.get()) != nullptr);
    REQUIRE(sel.log_line.find("LRGB-mono") != std::string::npos);
    REQUIRE(sel.log_line.find("VeraLux") != std::string::npos);
}

TEST_CASE("select_auto: all classes produce non-null op + non-empty log", "[module][auto_selector]") {
    for (FilterClass c : {FilterClass::LRGB_MONO, FilterClass::LRGB_COLOR,
                          FilterClass::BAYER_RGB, FilterClass::NARROWBAND}) {
        auto sel = select_auto(c);
        REQUIRE(sel.op != nullptr);
        REQUIRE(!sel.log_line.empty());
    }
}
```

- [ ] **Step 2: Header.**

Create `src/module/stretch_auto_selector.hpp`:

```cpp
#ifndef __NukeX_stretch_auto_selector_h
#define __NukeX_stretch_auto_selector_h

#include "filter_classifier.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include <memory>
#include <string>

namespace nukex {

struct AutoSelection {
    std::unique_ptr<StretchOp> op;
    std::string log_line;
};

AutoSelection select_auto(FilterClass cls);

} // namespace nukex

#endif
```

- [ ] **Step 3: Implementation.**

Create `src/module/stretch_auto_selector.cpp`:

```cpp
#include "stretch_auto_selector.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include <sstream>

namespace nukex {

namespace {

std::unique_ptr<StretchOp> make_champion(FilterClass /*cls*/) {
    // Phase 5 champion for all classes is VeraLux. Dispatch table structure
    // supports per-class champions when future phases change the table.
    return std::make_unique<VeraluxStretch>();
}

const char* champion_name(FilterClass /*cls*/) {
    return "VeraLux";
}

} // namespace

AutoSelection select_auto(FilterClass cls) {
    AutoSelection sel;
    sel.op = make_champion(cls);
    std::ostringstream oss;
    oss << "Auto: classified as " << filter_class_name(cls)
        << " -> " << champion_name(cls);
    sel.log_line = oss.str();
    return sel;
}

} // namespace nukex
```

- [ ] **Step 4: Build wiring.**

Edit `src/module/CMakeLists.txt` — add `stretch_auto_selector.cpp`.

Edit `test/CMakeLists.txt` — extend `nukex4_module_testlib` with `stretch_auto_selector.cpp`, link it against `nukex4_stretch`, and add:

```cmake
target_include_directories(nukex4_module_testlib PUBLIC
    "${CMAKE_SOURCE_DIR}/src/lib/stretch/include"
)
target_link_libraries(nukex4_module_testlib PUBLIC nukex4_stretch)
nukex_add_test(test_stretch_auto_selector unit/module/test_stretch_auto_selector.cpp nukex4_module_testlib)
```

- [ ] **Step 5: Build + test.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. && cmake --build . --target test_stretch_auto_selector -j$(nproc)
ctest --output-on-failure -R test_stretch_auto_selector
```

Expected: PASS.

- [ ] **Step 6: Commit.**

```bash
git add src/module/stretch_auto_selector.hpp src/module/stretch_auto_selector.cpp \
        src/module/CMakeLists.txt \
        test/unit/module/test_stretch_auto_selector.cpp test/CMakeLists.txt
git commit -m "feat(module): StretchAutoSelector — FilterClass to champion StretchOp"
```

---

### Task A5: StretchFactory

**Files:**
- Create: `src/module/stretch_factory.hpp`
- Create: `src/module/stretch_factory.cpp`
- Create: `test/unit/module/test_stretch_factory.cpp`
- Modify: `src/module/CMakeLists.txt`, `test/CMakeLists.txt`

- [ ] **Step 1: Test.**

Create `test/unit/module/test_stretch_factory.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "stretch_factory.hpp"
#include "fits_metadata.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"

using namespace nukex;

TEST_CASE("build_primary: Auto fills log_line", "[module][stretch_factory]") {
    FITSMetadata meta;
    meta.filter = "L";
    std::string log;
    auto op = build_primary(PrimaryStretch::Auto, meta, log);
    REQUIRE(op != nullptr);
    REQUIRE(!log.empty());
}

TEST_CASE("build_primary: explicit values return empty log_line", "[module][stretch_factory]") {
    FITSMetadata meta;
    std::string log = "prior";
    auto op = build_primary(PrimaryStretch::GHS, meta, log);
    REQUIRE(op != nullptr);
    REQUIRE(log.empty());
    REQUIRE(dynamic_cast<GHSStretch*>(op.get()) != nullptr);
}

TEST_CASE("build_primary: all named enums produce the correct op type", "[module][stretch_factory]") {
    FITSMetadata meta;
    std::string log;
    REQUIRE(dynamic_cast<VeraluxStretch*>(build_primary(PrimaryStretch::VeraLux, meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<GHSStretch*>    (build_primary(PrimaryStretch::GHS,     meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<MTFStretch*>    (build_primary(PrimaryStretch::MTF,     meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<ArcSinhStretch*>(build_primary(PrimaryStretch::ArcSinh, meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<LogStretch*>    (build_primary(PrimaryStretch::Log,     meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<LuptonStretch*> (build_primary(PrimaryStretch::Lupton,  meta, log).get()) != nullptr);
    REQUIRE(dynamic_cast<CLAHEStretch*>  (build_primary(PrimaryStretch::CLAHE,   meta, log).get()) != nullptr);
}

TEST_CASE("build_finishing: None returns nullptr", "[module][stretch_factory]") {
    REQUIRE(build_finishing(FinishingStretch::None) == nullptr);
}
```

- [ ] **Step 2: Header.**

Create `src/module/stretch_factory.hpp`:

```cpp
#ifndef __NukeX_stretch_factory_h
#define __NukeX_stretch_factory_h

#include "fits_metadata.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include <memory>
#include <string>

namespace nukex {

enum class PrimaryStretch {
    Auto = 0, VeraLux, GHS, MTF, ArcSinh, Log, Lupton, CLAHE,
};

enum class FinishingStretch {
    None = 0,
};

std::unique_ptr<StretchOp> build_primary(PrimaryStretch e,
                                         const FITSMetadata& meta,
                                         std::string& out_log_line);

std::unique_ptr<StretchOp> build_finishing(FinishingStretch e);

} // namespace nukex

#endif
```

- [ ] **Step 3: Implementation.**

Create `src/module/stretch_factory.cpp`:

```cpp
#include "stretch_factory.hpp"
#include "stretch_auto_selector.hpp"

#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"

namespace nukex {

std::unique_ptr<StretchOp> build_primary(PrimaryStretch e,
                                         const FITSMetadata& meta,
                                         std::string& out_log_line) {
    out_log_line.clear();
    if (e == PrimaryStretch::Auto) {
        auto sel = select_auto(classify_filter(meta));
        out_log_line = std::move(sel.log_line);
        return std::move(sel.op);
    }
    switch (e) {
        case PrimaryStretch::VeraLux: return std::make_unique<VeraluxStretch>();
        case PrimaryStretch::GHS:     return std::make_unique<GHSStretch>();
        case PrimaryStretch::MTF:     return std::make_unique<MTFStretch>();
        case PrimaryStretch::ArcSinh: return std::make_unique<ArcSinhStretch>();
        case PrimaryStretch::Log:     return std::make_unique<LogStretch>();
        case PrimaryStretch::Lupton:  return std::make_unique<LuptonStretch>();
        case PrimaryStretch::CLAHE:   return std::make_unique<CLAHEStretch>();
        case PrimaryStretch::Auto:    break;
    }
    return std::make_unique<VeraluxStretch>();
}

std::unique_ptr<StretchOp> build_finishing(FinishingStretch /*e*/) {
    return nullptr;
}

} // namespace nukex
```

- [ ] **Step 4: Verify all stretch headers exist.**

```bash
cd /home/scarter4work/projects/nukex4
for h in veralux ghs mtf arcsinh log lupton clahe; do
    test -f "src/lib/stretch/include/nukex/stretch/${h}_stretch.hpp" && echo "OK: $h" || echo "MISSING: $h"
done
```

Expected: all print `OK`.

- [ ] **Step 5: Build wiring + test.**

Edit `src/module/CMakeLists.txt` — add `stretch_factory.cpp`.

Edit `test/CMakeLists.txt` — add `stretch_factory.cpp` to the testlib and register:

```cmake
nukex_add_test(test_stretch_factory unit/module/test_stretch_factory.cpp nukex4_module_testlib)
```

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. && cmake --build . --target test_stretch_factory -j$(nproc)
ctest --output-on-failure -R test_stretch_factory
```

Expected: all 4 tests PASS.

- [ ] **Step 6: Commit.**

```bash
git add src/module/stretch_factory.hpp src/module/stretch_factory.cpp \
        src/module/CMakeLists.txt \
        test/unit/module/test_stretch_factory.cpp test/CMakeLists.txt
git commit -m "feat(module): StretchFactory for primary + finishing ops"
```

---

### Task A6: Parameter schema — replace old enums with new ones

**Files:**
- Modify: `src/module/NukeXParameters.h`
- Modify: `src/module/NukeXParameters.cpp`
- Modify: `src/module/NukeXProcess.cpp`

- [ ] **Step 1: Rewrite `NukeXParameters.h` stretch section.**

Replace the `NXStretchType` + `NXAutoStretch` classes (roughly lines 62-84) with:

```cpp
// ── Stretch Configuration ────────────────────────────────────────

class NXPrimaryStretch : public MetaEnumeration
{
public:
   NXPrimaryStretch( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;

   enum { Auto, VeraLux, GHS, MTF, ArcSinh, Log, Lupton, CLAHE, NumberOfItems };
};

class NXFinishingStretch : public MetaEnumeration
{
public:
   NXFinishingStretch( MetaProcess* );
   IsoString Id() const override;
   size_type NumberOfElements() const override;
   IsoString ElementId( size_type ) const override;
   int ElementValue( size_type ) const override;
   size_type DefaultValueIndex() const override;

   enum { None, NumberOfItems };
};
```

Replace the `extern` lines 114-115:

```cpp
extern NXPrimaryStretch*   TheNXPrimaryStretchParameter;
extern NXFinishingStretch* TheNXFinishingStretchParameter;
```

- [ ] **Step 2: Rewrite `NukeXParameters.cpp` stretch section.**

Replace lines 17-18 (global pointers):

```cpp
NXPrimaryStretch*   TheNXPrimaryStretchParameter = nullptr;
NXFinishingStretch* TheNXFinishingStretchParameter = nullptr;
```

Replace lines 72-120 (`NXStretchType` + `NXAutoStretch` definitions):

```cpp
// ── Stretch Configuration ────────────────────────────────────────

NXPrimaryStretch::NXPrimaryStretch( MetaProcess* p ) : MetaEnumeration( p )
{
   TheNXPrimaryStretchParameter = this;
}

IsoString NXPrimaryStretch::Id() const { return "primaryStretch"; }
size_type NXPrimaryStretch::NumberOfElements() const { return NumberOfItems; }

IsoString NXPrimaryStretch::ElementId( size_type i ) const
{
   switch ( i )
   {
   case Auto:    return "Auto";
   case VeraLux: return "VeraLux";
   case GHS:     return "GHS";
   case MTF:     return "MTF";
   case ArcSinh: return "ArcSinh";
   case Log:     return "Log";
   case Lupton:  return "Lupton";
   case CLAHE:   return "CLAHE";
   default:      return IsoString();
   }
}

int NXPrimaryStretch::ElementValue( size_type i ) const { return int( i ); }
size_type NXPrimaryStretch::DefaultValueIndex() const { return Auto; }

NXFinishingStretch::NXFinishingStretch( MetaProcess* p ) : MetaEnumeration( p )
{
   TheNXFinishingStretchParameter = this;
}

IsoString NXFinishingStretch::Id() const { return "finishingStretch"; }
size_type NXFinishingStretch::NumberOfElements() const { return NumberOfItems; }

IsoString NXFinishingStretch::ElementId( size_type i ) const
{
   switch ( i )
   {
   case None: return "None";
   default:   return IsoString();
   }
}

int NXFinishingStretch::ElementValue( size_type i ) const { return int( i ); }
size_type NXFinishingStretch::DefaultValueIndex() const { return None; }
```

- [ ] **Step 3: Update `NukeXProcess.cpp` registration.**

Around lines 30-31, replace `new NXStretchType( this );` and `new NXAutoStretch( this );` with:

```cpp
   new NXPrimaryStretch( this );
   new NXFinishingStretch( this );
```

- [ ] **Step 4: Verify the module fails to compile (expected).**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . --target NukeX -j$(nproc) 2>&1 | tail -30
```

Expected: errors in `NukeXInstance.cpp` and `NukeXInterface.cpp` referencing removed names.

- [ ] **Step 5: Commit.**

```bash
git add src/module/NukeXParameters.h src/module/NukeXParameters.cpp src/module/NukeXProcess.cpp
git commit -m "feat(params): primaryStretch + finishingStretch replace flat stretchType"
```

---

### Task A7: NukeXInstance storage + LockParameter updates

**Files:**
- Modify: `src/module/NukeXInstance.h`
- Modify: `src/module/NukeXInstance.cpp`

- [ ] **Step 1: Replace storage fields in `NukeXInstance.h`.**

Lines 43-44:

```cpp
   pcl_enum    primaryStretch    = 0;  // NXPrimaryStretch::Auto
   pcl_enum    finishingStretch  = 0;  // NXFinishingStretch::None
```

- [ ] **Step 2: Update `Assign()` in `NukeXInstance.cpp`.**

Lines 36-37:

```cpp
      primaryStretch   = x->primaryStretch;
      finishingStretch = x->finishingStretch;
```

- [ ] **Step 3: Update `LockParameter()` in `NukeXInstance.cpp`.**

Lines 211-212:

```cpp
   if ( p == TheNXPrimaryStretchParameter )    return &primaryStretch;
   if ( p == TheNXFinishingStretchParameter )  return &finishingStretch;
```

- [ ] **Step 4: Build.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . --target NukeX -j$(nproc) 2>&1 | tail -15
```

Expected: errors now only from `NukeXInterface.cpp`.

- [ ] **Step 5: Commit.**

```bash
git add src/module/NukeXInstance.h src/module/NukeXInstance.cpp
git commit -m "feat(instance): rename stretch fields + rewire LockParameter"
```

---

### Task A8: NukeXInterface UI bindings

**Files:**
- Modify: `src/module/NukeXInterface.h`
- Modify: `src/module/NukeXInterface.cpp`

- [ ] **Step 1: Locate widgets to replace.**

```bash
cd /home/scarter4work/projects/nukex4
grep -n "StretchType_ComboBox\|AutoStretch_CheckBox" src/module/NukeXInterface.h src/module/NukeXInterface.cpp
```

- [ ] **Step 2: Update `NukeXInterface.h`.**

In the `GUIData` struct, replace the `StretchType_ComboBox` and `AutoStretch_CheckBox` members with:

```cpp
      ComboBox   PrimaryStretch_ComboBox;
      ComboBox   FinishingStretch_ComboBox;
```

If there were paired labels, rename them to `PrimaryStretch_Label` and `FinishingStretch_Label`.

- [ ] **Step 3: Update widget construction in `NukeXInterface.cpp`.**

In the `GUIData::GUIData(NukeXInterface& w)` constructor, replace the old stretch widget population block with:

```cpp
   PrimaryStretch_ComboBox.AddItem( "Auto" );
   PrimaryStretch_ComboBox.AddItem( "VeraLux" );
   PrimaryStretch_ComboBox.AddItem( "GHS" );
   PrimaryStretch_ComboBox.AddItem( "MTF" );
   PrimaryStretch_ComboBox.AddItem( "ArcSinh" );
   PrimaryStretch_ComboBox.AddItem( "Log" );
   PrimaryStretch_ComboBox.AddItem( "Lupton" );
   PrimaryStretch_ComboBox.AddItem( "CLAHE" );
   PrimaryStretch_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::__ItemSelected, w );

   FinishingStretch_ComboBox.AddItem( "None" );
   FinishingStretch_ComboBox.OnItemSelected( (ComboBox::item_event_handler)&NukeXInterface::__ItemSelected, w );
```

Update the sizer layout to lay out the two combo boxes alongside their labels, following the existing pattern.

- [ ] **Step 4: Update `UpdateControls()`.**

Replace the existing two lines (around 243-244):

```cpp
   GUI->PrimaryStretch_ComboBox.SetCurrentItem( instance.primaryStretch );
   GUI->FinishingStretch_ComboBox.SetCurrentItem( instance.finishingStretch );
```

- [ ] **Step 5: Update event handlers.**

Rewrite `__ItemSelected` around line 391:

```cpp
void NukeXInterface::__ItemSelected( ComboBox& sender, int itemIndex )
{
   if ( sender == GUI->PrimaryStretch_ComboBox )
      instance.primaryStretch = itemIndex;
   else if ( sender == GUI->FinishingStretch_ComboBox )
      instance.finishingStretch = itemIndex;
}
```

Delete any `AutoStretch_CheckBox.OnCheck` handler — no replacement.

- [ ] **Step 6: Build the full module.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . --target NukeX -j$(nproc) 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 7: Grep for stragglers.**

```bash
cd /home/scarter4work/projects/nukex4
grep -rn "stretchType\|autoStretch\|StretchType_ComboBox\|AutoStretch_CheckBox\|TheNXStretchType\|TheNXAutoStretch" src/ test/ 2>/dev/null
```

Expected: empty output.

- [ ] **Step 8: Full ctest run.**

```bash
cd /home/scarter4work/projects/nukex4/build
ctest --output-on-failure
```

Expected: all existing tests PASS.

- [ ] **Step 9: Commit.**

```bash
git add src/module/NukeXInterface.h src/module/NukeXInterface.cpp
git commit -m "feat(ui): primary + finishing stretch dropdowns"
```

---

### Task A9: Wire StretchPipeline into ExecuteGlobal + third window

**Files:**
- Modify: `src/module/NukeXInstance.cpp`

- [ ] **Step 1: Verify `nukex::Image` copy semantics.**

```bash
cd /home/scarter4work/projects/nukex4
grep -n "Image(const Image" src/lib/io/include/nukex/io/image.hpp src/lib/io/src/image.cpp 2>/dev/null
grep -n "clone\|deep_copy" src/lib/io/include/nukex/io/image.hpp 2>/dev/null
```

If a deep-copy constructor or `clone()` method exists, use it in Step 2. If not, construct a new `Image(w, h, nc)` and memcpy channel buffers explicitly. Do NOT proceed until copy semantics are confirmed — an aliasing copy would stretch the stacked window in-place.

- [ ] **Step 2: Add includes and wire the pipeline.**

At the top of `src/module/NukeXInstance.cpp`, alongside the existing stacker include:

```cpp
#include "nukex/stretch/stretch_pipeline.hpp"
#include "fits_metadata.hpp"
#include "stretch_factory.hpp"
```

In `ExecuteGlobal()`, after the stacked-window block (around line 171) and before the noise-map block (around line 173), insert:

```cpp
   // ── Stretch pipeline (Phase 7 wiring) ─────────────────────────
   if ( !result.stacked.empty() && !light_paths.empty() )
   {
      nukex::FITSMetadata meta = nukex::read_fits_metadata( light_paths.front() );
      std::string auto_log;
      auto primary_op   = nukex::build_primary(
          static_cast<nukex::PrimaryStretch>( primaryStretch ), meta, auto_log );
      auto finishing_op = nukex::build_finishing(
          static_cast<nukex::FinishingStretch>( finishingStretch ) );

      if ( !auto_log.empty() )
         progress.message( auto_log.c_str() );

      // Deep copy — stretch is in-place, we must not mutate result.stacked.
      // If nukex::Image's copy constructor is shallow, substitute the
      // clone path confirmed in Step 1.
      nukex::Image stretched = result.stacked;

      nukex::StretchPipeline pipeline;
      if ( primary_op )
      {
         primary_op->enabled = true;
         primary_op->position = 0;
         pipeline.ops.push_back( std::move( primary_op ) );
      }
      if ( finishing_op )
      {
         finishing_op->enabled = true;
         finishing_op->position = 1;
         pipeline.ops.push_back( std::move( finishing_op ) );
      }
      pipeline.execute( stretched );

      int sw = stretched.width();
      int sh = stretched.height();
      int snc = stretched.n_channels();

      ImageWindow sw_win( sw, sh, snc, 32, true, snc >= 3, true, "NukeX_stretched" );
      View sv = sw_win.MainView();
      ImageVariant svi = sv.Image();
      if ( svi.IsFloatSample() && svi.BitsPerSample() == 32 )
      {
         pcl::Image& si = static_cast<pcl::Image&>( *svi );
         for ( int ch = 0; ch < snc; ch++ )
         {
            const float* src = stretched.channel_data( ch );
            float* dst = si.PixelData( ch );
            ::memcpy( dst, src, sw * sh * sizeof( float ) );
         }
      }
      sw_win.Show();
      progress.message( "Stretched image opened." );
   }
```

- [ ] **Step 3: Build + ctest.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . --target NukeX -j$(nproc) && ctest --output-on-failure
```

Expected: clean build, all tests PASS.

- [ ] **Step 4: Commit.**

```bash
git add src/module/NukeXInstance.cpp
git commit -m "feat(instance): wire StretchPipeline into ExecuteGlobal"
```

---

## Phase B — Profile Phase B, land one ≥ 1.5× optimization

### Task B1: Wall-time timer around `execute_phase_b`

**Files:**
- Modify: `src/lib/stacker/src/stacking_engine.cpp`

- [ ] **Step 1: Add `#include <chrono>` if not present, then wrap the call.**

In `src/lib/stacker/src/stacking_engine.cpp` replace the single `gpu.execute_phase_b(...)` call (around line 310) with:

```cpp
    auto phase_b_start = std::chrono::steady_clock::now();
    gpu.execute_phase_b(cube, cache, frame_stats, config_.weight_config,
                         fitting_fn, stacked, noise_map, &obs);
    auto phase_b_end = std::chrono::steady_clock::now();
    long phase_b_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          phase_b_end - phase_b_start ).count();
    obs.message("Phase B: " + std::to_string(phase_b_ms) + " ms");
```

- [ ] **Step 2: Build + test.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . -j$(nproc) && ctest --output-on-failure -R test_robust_stats
```

Expected: clean, PASS.

- [ ] **Step 3: Commit.**

```bash
git add src/lib/stacker/src/stacking_engine.cpp
git commit -m "feat(stacker): log Phase B wall-time"
```

---

### Task B2: Add `NUKEX_PROFILING` CMake option

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the option.**

Near the top of the top-level `CMakeLists.txt`, after `project(...)`:

```cmake
option(NUKEX_PROFILING "Add -fno-omit-frame-pointer for perf flamegraph capture" OFF)
if(NUKEX_PROFILING)
    message(STATUS "NUKEX_PROFILING=ON: adding -fno-omit-frame-pointer")
    add_compile_options(-fno-omit-frame-pointer)
endif()
```

- [ ] **Step 2: Verify release build unchanged.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. && cmake --build . -j$(nproc) 2>&1 | tail -3
```

Expected: clean.

- [ ] **Step 3: Verify profiling build turns the flag on.**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build_profile && mkdir build_profile && cd build_profile
cmake -DNUKEX_PROFILING=ON .. 2>&1 | grep -i NUKEX_PROFILING
cmake --build . -j$(nproc) 2>&1 | tail -3
```

Expected: the status line appears; clean build.

- [ ] **Step 4: Commit.**

```bash
cd /home/scarter4work/projects/nukex4
git add CMakeLists.txt
git commit -m "build: NUKEX_PROFILING option adds -fno-omit-frame-pointer"
```

---

### Task B3: Capture Phase B baseline wall-time

**Files:**
- `tools/capture_baseline.js` — already exists as of commit `41eb057` (Phase 7 A9), updated for PJSR quirks.
- Create: `test/fixtures/phaseB_baseline_ms.txt`

> **PJSR headless quirks (discovered 2026-04-20, see memory `reference_pjsr_automation_quirks.md`):**
> 1. `File.environmentVariable()` does **NOT** see shell env. Use `jsArguments` via comma-separated `-r=<path>,key=val`.
> 2. `Console.writeln` and C++ `Console()` output do **NOT** reach shell stdout under `--automation-mode`. Wrap in `Console.beginLog()` / `Console.endLog()` and write captured bytes to a file.
> 3. Enum values with same integer collide on the prototype (`Auto` and `None` both = 0 → only one name exists). Use integer literals: `P.primaryStretch = 0; // Auto`.
> 4. `File.appendFile` doesn't exist. Only `File.writeTextFile(path, str)` or `new File; openForAppending(...)`.
>
> The checked-in `tools/capture_baseline.js` already applies all of these. Refer to it for the canonical headless harness pattern.

- [ ] **Step 1: (Done as of v4.0.0.4 + patch.) Capture harness is at `tools/capture_baseline.js`.**

- [ ] **Step 2: Run against the baseline dataset.**

```bash
cd /home/scarter4work/projects/nukex4
BASELINE="/mnt/qnap/astro_data/NGC7635/L/Lights"   # 65 LRGB mono, 200s Bin1x1
test -d "$BASELINE" || { echo "no baseline found, pick manually"; exit 1; }
rm -f /tmp/nukex_baseline_console.log /tmp/nukex_baseline_meta.txt
/opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    "-r=$(pwd)/tools/capture_baseline.js,stack=$BASELINE" 2>&1 | tee /tmp/baseline.log
```

Expected runtime: several minutes on RTX-class GPU. On completion, check `/tmp/nukex_baseline_meta.txt` has `STATUS ok`.

- [ ] **Step 3: Extract Phase B time and write the fixture.**

```bash
# The C++ Phase B timer is captured in the PJSR log file, not shell stdout.
PHASE_B_MS=$(grep -oE "Phase B: [0-9]+ ms" /tmp/nukex_baseline_console.log | head -1 | grep -oE "[0-9]+")
echo "Baseline Phase B: ${PHASE_B_MS} ms"
test -n "$PHASE_B_MS" || { echo "no Phase B line in log — run failed"; tail /tmp/nukex_baseline_console.log; exit 1; }
mkdir -p test/fixtures
echo "${PHASE_B_MS}" > test/fixtures/phaseB_baseline_ms.txt
```

- [ ] **Step 4: Commit.**

```bash
git add tools/capture_baseline.js test/fixtures/phaseB_baseline_ms.txt
git commit -m "test: Phase B baseline wall-time + PJSR-headless capture harness"
```

---

### Task B4: Run perf + generate flamegraph + document findings

**Files:**
- Create: `docs/superpowers/plans/2026-04-19-phase7-perf-findings.md`

- [ ] **Step 1: Prereqs.**

```bash
which perf || { echo "install perf (sudo dnf install perf)"; exit 1; }
cat /proc/sys/kernel/perf_event_paranoid
# If > 1, run once:
# echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
FG="${HOME}/projects/FlameGraph"
test -d "$FG" || git clone https://github.com/brendangregg/FlameGraph "$FG"
```

- [ ] **Step 2: Build with profiling flag.**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build_profile && mkdir build_profile && cd build_profile
cmake -DNUKEX_PROFILING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo .. \
    && cmake --build . -j$(nproc)
```

- [ ] **Step 3: Install the profiling .so into PI (signing if required).**

Follow the signing command from global CLAUDE.md if PI rejects unsigned modules. Copy the freshly built `NukeX-pxm.so` (and `.xsgn`) into PI's module dir (typically `/opt/PixInsight/bin/` or equivalent).

- [ ] **Step 4: Record a profile.**

```bash
cd /home/scarter4work/projects/nukex4
BASELINE="/mnt/qnap/astro_data/NGC7635/L/Lights"   # same as Task B3
rm -f /tmp/nukex_baseline_console.log /tmp/nukex_baseline_meta.txt
perf record -F 99 -g --call-graph dwarf -o /tmp/nukex_phaseB.data \
    /opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
        "-r=$(pwd)/tools/capture_baseline.js,stack=$BASELINE" 2>&1 | tee /tmp/perf_run.log
```

Expected: `~50-200 MB` `nukex_phaseB.data`, and `/tmp/nukex_baseline_console.log` contains `Phase B: N ms`.

- [ ] **Step 5: Generate the flamegraph.**

```bash
perf script -i /tmp/nukex_phaseB.data \
    | "${HOME}/projects/FlameGraph/stackcollapse-perf.pl" \
    | "${HOME}/projects/FlameGraph/flamegraph.pl" > /tmp/nukex_phaseB.svg
firefox /tmp/nukex_phaseB.svg &
```

If Ceres stacks truncate at `ceres::Solve()`, rebuild Ceres from source with `-fno-omit-frame-pointer` and repeat Steps 2–5.

- [ ] **Step 6: Write findings to a committed file.**

Create `docs/superpowers/plans/2026-04-19-phase7-perf-findings.md`:

```markdown
# Phase 7 Perf Findings

## Baseline
- Dataset: <describe: camera, filter, count, dimensions>
- Phase B wall-time: <N> ms
- Flamegraph: /tmp/nukex_phaseB.svg (not committed; reproducible)

## Top 5 exclusive-time functions (% of Phase B)
1. <function> — <n>%
2. ...

## Bottleneck assessment (spec criteria)
Pre-committed bar: one section >= 50%, OR one function >= 3x siblings,
OR visible redundancy.

- Result: <PASS / FAIL and which criterion met, or "none met" with
  escalation decision per spec>

## Selected optimization
<one sentence>

## Expected speedup
<estimate based on literature or analogous changes>
```

Fill in with real observations.

- [ ] **Step 7: Commit the findings.**

```bash
git add docs/superpowers/plans/2026-04-19-phase7-perf-findings.md
git commit -m "docs(phase7): perf findings + selected optimization"
```

---

### Task B5: Implement the selected optimization

**Files:** depends on findings. Most likely: `src/lib/fitting/src/student_t_fitter.cpp`, `contamination_fitter.cpp`, or the GPU kernel path.

- [ ] **Step 1: Implement the optimization.**

Apply the change identified in Task B4. Keep the diff narrow — only the file where the bottleneck lives.

- [ ] **Step 2: Build + full ctest.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . -j$(nproc) && ctest --output-on-failure
```

Expected: every test PASS. If any fitting/stacker test breaks, revert — correctness wins over perf.

- [ ] **Step 3: Commit.**

```bash
git add <files changed>
git commit -m "perf(fitting): <one-sentence description>"
```

---

### Task B6: Verify ≥ 1.5× speedup

**Files:** none modified (appends to findings doc).

- [ ] **Step 1: Re-run baseline harness on the optimized release build.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . -j$(nproc)
# Install the rebuilt module into PI first — see CLAUDE.md signing steps or re-run `make package`.
cd /home/scarter4work/projects/nukex4
BASELINE="/mnt/qnap/astro_data/NGC7635/L/Lights"   # same as Task B3
rm -f /tmp/nukex_baseline_console.log /tmp/nukex_baseline_meta.txt
/opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    "-r=$(pwd)/tools/capture_baseline.js,stack=$BASELINE" 2>&1 | tee /tmp/postopt.log
cp /tmp/nukex_baseline_console.log /tmp/postopt_console.log
```

- [ ] **Step 2: Check the speedup.**

```bash
POST=$(grep -oE "Phase B: [0-9]+ ms" /tmp/postopt_console.log | head -1 | grep -oE "[0-9]+")
BASE=$(cat test/fixtures/phaseB_baseline_ms.txt)
python3 -c "base=${BASE}; post=${POST}; s=base/post; print(f'Base {base} ms, Post {post} ms, Speedup {s:.2f}x — {\"PASS\" if s>=1.5 else \"FAIL\"}')"
```

Expected: PASS. Otherwise iterate on B5 or revert.

- [ ] **Step 3: Append actual results to findings doc.**

Add a section:

```markdown
## Actual result
- Baseline Phase B: <N> ms
- Post-optimization Phase B: <M> ms
- Speedup: <S>x
- Ship bar (>= 1.5x): PASS
```

- [ ] **Step 4: Commit.**

```bash
git add docs/superpowers/plans/2026-04-19-phase7-perf-findings.md
git commit -m "docs(phase7): post-optimization speedup verified at <S>x"
```

---

## Phase C — E2E validation harness

### Task C1: E2E manifest

**Files:**
- Create: `test/fixtures/e2e_manifest.json`

- [ ] **Step 1: Write the manifest (placeholder paths).**

Create `test/fixtures/e2e_manifest.json`:

```json
{
  "schema_version": 1,
  "description": "Phase 7 E2E validation corpus for v4.0.0.4.",
  "cases": [
    {
      "name": "lrgb_mono_medium",
      "filter_class_expected": "LRGB-mono",
      "light_dir": "REPLACE_WITH_PATH",
      "flat_dir": "",
      "primary_stretch": "Auto",
      "finishing_stretch": "None",
      "wall_time_budget_s": 600,
      "golden_stacked": "lrgb_mono_medium.sha256",
      "golden_noise": "lrgb_mono_medium_noise.sha256",
      "dropdown_sweep": ["GHS", "MTF", "ArcSinh"]
    },
    {
      "name": "bayer_rgb_medium",
      "filter_class_expected": "Bayer-RGB",
      "light_dir": "REPLACE_WITH_PATH",
      "flat_dir": "",
      "primary_stretch": "Auto",
      "finishing_stretch": "None",
      "wall_time_budget_s": 600,
      "golden_stacked": "bayer_rgb_medium.sha256",
      "golden_noise": "bayer_rgb_medium_noise.sha256",
      "dropdown_sweep": []
    },
    {
      "name": "narrowband_medium",
      "filter_class_expected": "Narrowband",
      "light_dir": "REPLACE_WITH_PATH",
      "flat_dir": "",
      "primary_stretch": "Auto",
      "finishing_stretch": "None",
      "wall_time_budget_s": 600,
      "golden_stacked": "narrowband_medium.sha256",
      "golden_noise": "narrowband_medium_noise.sha256",
      "dropdown_sweep": []
    },
    {
      "name": "lrgb_mono_large_stress",
      "filter_class_expected": "LRGB-mono",
      "light_dir": "REPLACE_WITH_PATH",
      "flat_dir": "",
      "primary_stretch": "Auto",
      "finishing_stretch": "None",
      "wall_time_budget_s": 1800,
      "golden_stacked": "lrgb_mono_large_stress.sha256",
      "golden_noise": "lrgb_mono_large_stress_noise.sha256",
      "dropdown_sweep": []
    }
  ],
  "phaseB_baseline_file": "phaseB_baseline_ms.txt",
  "phaseB_speedup_min": 1.5
}
```

- [ ] **Step 2: Fill in real paths from user data.**

```bash
cd /home/scarter4work/projects/nukex4
ls -d ~/projects/processing/*/L 2>/dev/null | head -3
ls -d /mnt/qnap/astro_data/*/L 2>/dev/null | head -3
ls -d /mnt/qnap/astro_data/*/OSC 2>/dev/null | head -3
ls -d /mnt/qnap/astro_data/*/Ha /mnt/qnap/astro_data/*/OIII 2>/dev/null | head -3
```

Replace each `"REPLACE_WITH_PATH"` with a real directory. If a filter class is unavailable, set `"skip": true` on that case and note the reason in the manifest's `description`.

- [ ] **Step 3: Commit.**

```bash
git add test/fixtures/e2e_manifest.json
git commit -m "test: E2E corpus manifest for 4-stack Phase 7 validation"
```

---

### Task C2: Harness scaffolding

**Files:**
- Create: `tools/validate_e2e.js`

Validate every PJSR call against the `pjsr` MCP's `pjsr-analyze` tool before committing.

- [ ] **Step 1: Write the skeleton.**

Create `tools/validate_e2e.js`:

```javascript
// NukeX v4 — Phase 7 E2E validation harness.
// Runs headless: PixInsight.sh --automation-mode --force-exit -r=tools/validate_e2e.js

function getEnv(name) {
   try {
      if (typeof File.environmentVariable === "function")
         return File.environmentVariable(name) || "";
   } catch (e) {}
   return "";
}

// Derive repo root from an env var if set, else best-effort from script path.
var REPO = getEnv("NUKEX_REPO");
if (!REPO) REPO = "/home/scarter4work/projects/nukex4";

var MANIFEST   = REPO + "/test/fixtures/e2e_manifest.json";
var GOLDEN_DIR = REPO + "/test/fixtures/golden";
var REPORT     = REPO + "/build/e2e_report.json";

function fail(msg) {
   Console.criticalln("E2E FAIL: " + msg);
   throw new Error(msg);
}

function writeText(path, text) {
   var f = new File();
   f.createForWriting(path);
   f.outText(text);
   f.close();
}

function readManifest() {
   if (!File.exists(MANIFEST)) fail("missing manifest: " + MANIFEST);
   var text = File.readTextFile(MANIFEST);
   return JSON.parseString(text);
}

function writeReport(obj) {
   writeText(REPORT, JSON.stringify(obj, null, 2) + "\n");
}

function runCase(tc, manifest) {
   fail("runCase not yet implemented - fill in Tasks C3-C6");
}

function main() {
   Console.writeln("=== NukeX v4 E2E validation ===");
   var manifest = readManifest();
   var results = [];
   for (var i = 0; i < manifest.cases.length; i++) {
      var tc = manifest.cases[i];
      if (tc.skip) { Console.writeln("SKIP: " + tc.name); continue; }
      Console.writeln("--- case: " + tc.name + " ---");
      results.push(runCase(tc, manifest));
   }
   writeReport({ status: "PASS", cases: results });
   Console.writeln("=== ALL CASES PASS ===");
}

try {
   main();
} catch (e) {
   Console.criticalln("HARNESS FAILURE: " + e.message);
   try { writeReport({ status: "FAIL", error: e.message }); } catch (e2) {}
   throw e;  // non-zero exit under --force-exit
}
```

- [ ] **Step 2: Smoke-test.**

```bash
cd /home/scarter4work/projects/nukex4
/opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    -r=tools/validate_e2e.js 2>&1 | tail -20
cat build/e2e_report.json 2>/dev/null
```

Expected: log ends with `HARNESS FAILURE: runCase not yet implemented`; report shows `status: FAIL`.

- [ ] **Step 3: Commit.**

```bash
git add tools/validate_e2e.js
git commit -m "test: E2E harness scaffolding"
```

---

### Task C3: Harness — checks 1-3 (execute + wall-time)

**Files:**
- Modify: `tools/validate_e2e.js`

- [ ] **Step 1: Replace `runCase` body.**

```javascript
function collectLights(dir) {
   var pats = ["*.fit", "*.fits", "*.FIT", "*.FITS"];
   var all = [];
   for (var i = 0; i < pats.length; i++) {
      var found = searchDirectory(dir + "/" + pats[i], false);
      for (var j = 0; j < found.length; j++) all.push(found[j]);
   }
   return all;
}

function primaryStretchEnum(name) {
   switch (name) {
      case "Auto":    return NukeX.prototype.primaryStretch_Auto;
      case "VeraLux": return NukeX.prototype.primaryStretch_VeraLux;
      case "GHS":     return NukeX.prototype.primaryStretch_GHS;
      case "MTF":     return NukeX.prototype.primaryStretch_MTF;
      case "ArcSinh": return NukeX.prototype.primaryStretch_ArcSinh;
      case "Log":     return NukeX.prototype.primaryStretch_Log;
      case "Lupton":  return NukeX.prototype.primaryStretch_Lupton;
      case "CLAHE":   return NukeX.prototype.primaryStretch_CLAHE;
   }
   fail("unknown primaryStretch: " + name);
}

function finishingStretchEnum(name) {
   if (name === "None") return NukeX.prototype.finishingStretch_None;
   fail("unknown finishingStretch: " + name);
}

function runCase(tc, manifest) {
   if (!tc.light_dir || tc.light_dir.indexOf("REPLACE_WITH") === 0)
      fail(tc.name + ": light_dir not configured");
   var lights = collectLights(tc.light_dir);
   if (lights.length === 0) fail(tc.name + ": no FITS found in " + tc.light_dir);
   Console.writeln(tc.name + ": " + lights.length + " lights");

   var t0 = new Date().getTime();

   var P = new NukeX;
   var arr = [];
   for (var i = 0; i < lights.length; i++) arr.push([true, lights[i]]);
   P.lightFrames = arr;
   P.flatFrames = [];
   P.primaryStretch = primaryStretchEnum(tc.primary_stretch);
   P.finishingStretch = finishingStretchEnum(tc.finishing_stretch);
   P.enableGPU = true;
   P.cacheDirectory = "/tmp";

   var ok;
   try { ok = P.executeGlobal(); }
   catch (e) { fail(tc.name + ": executeGlobal threw: " + e.message); }
   if (!ok) fail(tc.name + ": executeGlobal returned false");

   var elapsedS = (new Date().getTime() - t0) / 1000.0;
   if (elapsedS > tc.wall_time_budget_s)
      fail(tc.name + ": wall-time " + elapsedS + "s > budget " + tc.wall_time_budget_s + "s");
   Console.writeln(tc.name + ": OK (1-3 in " + elapsedS.toFixed(1) + "s)");

   return { name: tc.name, wall_time_s: elapsedS, status: "PASS_PARTIAL" };
}
```

- [ ] **Step 2: Run against one configured case (mark others `skip`).**

```bash
/opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    -r=tools/validate_e2e.js 2>&1 | tee /tmp/e2e_c3.log
tail -20 /tmp/e2e_c3.log
```

Expected: first case prints `OK (1-3 in Ns)`.

- [ ] **Step 3: Commit.**

```bash
git add tools/validate_e2e.js
git commit -m "test(e2e): checks 1-3 (load, execute, wall-time)"
```

---

### Task C4: Harness — checks 4-7 (stacked + bitwise)

**Files:**
- Modify: `tools/validate_e2e.js`

- [ ] **Step 1: Add helpers at top of file.**

```javascript
function sha256File(path) {
   var f = new File(path, FileMode_Read | FileMode_Open);
   var bytes = f.read(DataType_ByteArray, f.size);
   f.close();
   var h = new CryptographicHash(CryptographicHash_SHA256);
   h.update(bytes);
   return h.finalize().toHex().toLowerCase();
}
```

- [ ] **Step 2: Append check code to `runCase`** (before the `return` statement):

```javascript
   // Check #4: stacked window exists with expected shape.
   var stackedWin = ImageWindow.windowById("NukeX_stacked");
   if (stackedWin.isNull) fail(tc.name + ": NukeX_stacked missing");
   var stacked = stackedWin.mainView.image;

   // Check #5: all pixel values in [0,1].
   for (var ch = 0; ch < stacked.numberOfChannels; ch++) {
      var mn = stacked.minimum(ch), mx = stacked.maximum(ch);
      if (mn < 0.0 || mx > 1.0)
         fail(tc.name + ": stacked ch " + ch + " range [" + mn + "," + mx + "] outside [0,1]");
   }

   // Check #6: no NaN/Inf (mean is NaN if any NaN pixel present).
   for (var ch = 0; ch < stacked.numberOfChannels; ch++) {
      var m = stacked.mean(ch);
      if (isNaN(m) || !isFinite(m)) fail(tc.name + ": stacked ch " + ch + " has NaN/Inf");
   }

   // Check #7: bitwise regression.
   var tmpFits = "/tmp/e2e_" + tc.name + "_stacked.fit";
   if (!stackedWin.saveAs(tmpFits, false, false, false, false))
      fail(tc.name + ": saveAs stacked failed");
   var actual = sha256File(tmpFits);
   var goldenPath = GOLDEN_DIR + "/" + tc.golden_stacked;
   if (getEnv("NUKEX_REGENERATE_GOLDEN") === "1") {
      writeText(goldenPath, actual + "\n");
      Console.warningln(tc.name + ": regenerated " + goldenPath);
   } else {
      if (!File.exists(goldenPath)) fail(tc.name + ": missing golden " + goldenPath);
      var expected = File.readTextFile(goldenPath).split("\n")[0].trim();
      if (actual !== expected)
         fail(tc.name + ": stacked SHA mismatch\n  expected: " + expected + "\n  actual:   " + actual);
   }
   Console.writeln(tc.name + ": checks 4-7 PASS (" + actual.substring(0,12) + ")");
```

- [ ] **Step 3: Regenerate golden, then verify.**

```bash
mkdir -p test/fixtures/golden
NUKEX_REGENERATE_GOLDEN=1 /opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    -r=tools/validate_e2e.js 2>&1 | tail -20
/opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    -r=tools/validate_e2e.js 2>&1 | tail -20
```

Expected: second run passes checks 4-7.

- [ ] **Step 4: Commit (do not commit goldens yet — Task C7 does).**

```bash
git add tools/validate_e2e.js
git commit -m "test(e2e): checks 4-7 (stacked shape, range, NaN, bitwise)"
```

---

### Task C5: Harness — checks 8-15 (noise + stretched + dropdown sweep)

**Files:**
- Modify: `tools/validate_e2e.js`

- [ ] **Step 1: Append check code + helper to `runCase`.**

Add helper at top of file:

```javascript
function closeWindowIfOpen(id) {
   var w = ImageWindow.windowById(id);
   if (!w.isNull) w.forceClose();
}
```

Append to `runCase` (before the final `return`):

```javascript
   // Checks #8-10: noise map.
   var noiseWin = ImageWindow.windowById("NukeX_noise");
   if (noiseWin.isNull) fail(tc.name + ": NukeX_noise missing");
   var noise = noiseWin.mainView.image;
   if (noise.width !== stacked.width || noise.height !== stacked.height)
      fail(tc.name + ": noise dims mismatch");
   for (var ch = 0; ch < noise.numberOfChannels; ch++) {
      var mn = noise.minimum(ch), mx = noise.maximum(ch);
      if (mn < 0.0 || mx > 1.0) fail(tc.name + ": noise ch " + ch + " range outside [0,1]");
   }
   var tmpNoise = "/tmp/e2e_" + tc.name + "_noise.fit";
   if (!noiseWin.saveAs(tmpNoise, false, false, false, false))
      fail(tc.name + ": saveAs noise failed");
   var noiseHash = sha256File(tmpNoise);
   var noiseGolden = GOLDEN_DIR + "/" + tc.golden_noise;
   if (getEnv("NUKEX_REGENERATE_GOLDEN") === "1") {
      writeText(noiseGolden, noiseHash + "\n");
   } else {
      if (!File.exists(noiseGolden)) fail(tc.name + ": missing noise golden");
      var expN = File.readTextFile(noiseGolden).split("\n")[0].trim();
      if (noiseHash !== expN) fail(tc.name + ": noise SHA mismatch");
   }

   // Checks #11-13: stretched window.
   var stretchedWin = ImageWindow.windowById("NukeX_stretched");
   if (stretchedWin.isNull) fail(tc.name + ": NukeX_stretched missing");
   var stretched = stretchedWin.mainView.image;
   if (stretched.width !== stacked.width || stretched.height !== stacked.height)
      fail(tc.name + ": stretched dims mismatch");
   for (var ch = 0; ch < stretched.numberOfChannels; ch++) {
      var mn = stretched.minimum(ch), mx = stretched.maximum(ch);
      if (mn < 0.0 || mx > 1.0) fail(tc.name + ": stretched ch " + ch + " range outside [0,1]");
   }
   for (var ch = 0; ch < stretched.numberOfChannels; ch++) {
      if (stretched.median(ch) <= stacked.median(ch))
         fail(tc.name + ": ch " + ch + " stretched median not > stacked median");
   }

   // Check #14: Auto log line present.
   if (tc.primary_stretch === "Auto" && typeof Console.text === "function") {
      var log = Console.text();
      if (log.indexOf("Auto: classified as") < 0)
         fail(tc.name + ": Auto log line missing");
      if (log.indexOf(tc.filter_class_expected) < 0)
         fail(tc.name + ": expected filter class '" + tc.filter_class_expected + "' not in log");
   }

   // Check #15: dropdown sweep — pairwise-distinct stretched hashes.
   if (tc.dropdown_sweep && tc.dropdown_sweep.length > 0) {
      var hashes = {};
      var tmpAuto = "/tmp/e2e_" + tc.name + "_stretched_auto.fit";
      if (!stretchedWin.saveAs(tmpAuto, false, false, false, false))
         fail(tc.name + ": saveAs stretched (auto) failed");
      hashes["Auto"] = sha256File(tmpAuto);
      for (var k = 0; k < tc.dropdown_sweep.length; k++) {
         var name = tc.dropdown_sweep[k];
         closeWindowIfOpen("NukeX_stretched");
         closeWindowIfOpen("NukeX_stacked");
         closeWindowIfOpen("NukeX_noise");
         var P2 = new NukeX;
         P2.lightFrames = arr;
         P2.flatFrames = [];
         P2.primaryStretch = primaryStretchEnum(name);
         P2.finishingStretch = finishingStretchEnum("None");
         P2.enableGPU = true;
         P2.cacheDirectory = "/tmp";
         if (!P2.executeGlobal()) fail(tc.name + " [sweep=" + name + "]: failed");
         var sw2 = ImageWindow.windowById("NukeX_stretched");
         if (sw2.isNull) fail(tc.name + " [sweep=" + name + "]: missing stretched");
         var tmp2 = "/tmp/e2e_" + tc.name + "_stretched_" + name + ".fit";
         if (!sw2.saveAs(tmp2, false, false, false, false))
            fail(tc.name + " [sweep=" + name + "]: saveAs failed");
         hashes[name] = sha256File(tmp2);
      }
      var keys = Object.keys(hashes);
      for (var a = 0; a < keys.length; a++)
         for (var b = a + 1; b < keys.length; b++)
            if (hashes[keys[a]] === hashes[keys[b]])
               fail(tc.name + " [sweep]: " + keys[a] + " == " + keys[b] + " — dropdown inert");
   }

   Console.writeln(tc.name + ": checks 8-15 PASS");
```

- [ ] **Step 2: Regenerate + verify.**

```bash
NUKEX_REGENERATE_GOLDEN=1 /opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    -r=tools/validate_e2e.js 2>&1 | tail -20
/opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    -r=tools/validate_e2e.js 2>&1 | tail -20
```

Expected: second run prints `checks 8-15 PASS` for each configured case.

- [ ] **Step 3: Commit.**

```bash
git add tools/validate_e2e.js
git commit -m "test(e2e): checks 8-15 (noise, stretched, dropdown sweep)"
```

---

### Task C6: Harness — checks 16-18 (progress, cancel, perf)

**Files:**
- Modify: `tools/validate_e2e.js`
- Create: `tools/manual_cancel_test.md`

- [ ] **Step 1: Append check code to `runCase`.**

```javascript
   // Check #16: progress monotonicity — parse percentages from Console log.
   if (typeof Console.text === "function") {
      var log = Console.text();
      var pcts = [];
      var lines = log.split("\n");
      var re = /(\d+(?:\.\d+)?)\s*%/;
      for (var i = 0; i < lines.length; i++) {
         var m = re.exec(lines[i]);
         if (m) pcts.push(parseFloat(m[1]));
      }
      if (pcts.length >= 2) {
         for (var i = 1; i < pcts.length; i++) {
            if (pcts[i] + 0.001 < pcts[i-1])
               fail(tc.name + ": progress regressed " + pcts[i-1] + "% -> " + pcts[i] + "%");
         }
      } else {
         Console.warningln(tc.name + ": <2 progress %'s parsed; skipping monotonicity");
      }
   }

   // Check #17: cancellation — deferred to manual procedure (see
   // tools/manual_cancel_test.md). PJSR bindings for progress observer
   // not exposed yet.
   Console.writeln(tc.name + ": check 17 deferred (see manual_cancel_test.md)");

   // Check #18: Phase B perf gate — first case only.
   if (tc.name === "lrgb_mono_medium") {
      var baseText = File.readTextFile(REPO + "/test/fixtures/" + manifest.phaseB_baseline_file);
      var baseMs = parseInt(baseText.split("\n")[0].trim(), 10);
      var phaseBMs = null;
      if (typeof Console.text === "function") {
         var log = Console.text();
         var mm = /Phase B:\s*(\d+)\s*ms/.exec(log);
         if (mm) phaseBMs = parseInt(mm[1], 10);
      }
      if (phaseBMs === null) fail(tc.name + ": Phase B ms not found in log");
      var bar = baseMs / manifest.phaseB_speedup_min;
      if (phaseBMs > bar)
         fail(tc.name + ": Phase B " + phaseBMs + " ms > bar " + bar.toFixed(0) + " ms");
      Console.writeln(tc.name + ": Phase B " + phaseBMs + " ms vs bar " + bar.toFixed(0) + " ms PASS");
   }
```

- [ ] **Step 2: Create the manual cancel test doc.**

Create `tools/manual_cancel_test.md`:

```markdown
# Manual cancellation test — required before v4.0.0.4 release

1. Launch PI: `/opt/PixInsight/bin/PixInsight.sh`
2. Open the NukeX process from the Process menu.
3. Load the same lights as the E2E `lrgb_mono_medium` fixture.
4. Click Apply.
5. When the progress dialog shows "Phase B:" activity, click Cancel.
6. Verify within ~10 seconds: dialog closes, no output windows appear,
   PI returns to idle, no zombie threads in `htop`.

Automation lands when the NukeX PJSR bindings expose a progress observer
in a future release.
```

- [ ] **Step 3: Change end-of-run status from `PASS_PARTIAL` to `PASS`.**

At the end of `runCase`, change the return to:

```javascript
   return { name: tc.name, wall_time_s: elapsedS, status: "PASS" };
```

- [ ] **Step 4: Run full harness.**

```bash
/opt/PixInsight/bin/PixInsight.sh --automation-mode --force-exit \
    -r=tools/validate_e2e.js 2>&1 | tail -40
cat build/e2e_report.json
```

Expected: `"status": "PASS"`, `ALL CASES PASS`.

- [ ] **Step 5: Commit.**

```bash
git add tools/validate_e2e.js tools/manual_cancel_test.md
git commit -m "test(e2e): checks 16-18 (progress, perf gate); cancel is manual"
```

---

### Task C7: Commit goldens + CMake `e2e` target

**Files:**
- Commit: `test/fixtures/golden/*.sha256`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Verify goldens present.**

```bash
ls test/fixtures/golden/
```

Expected: `<case>.sha256` + `<case>_noise.sha256` for every non-skipped case.

- [ ] **Step 2: Commit goldens.**

```bash
git add test/fixtures/golden/*.sha256
git commit -m "test: E2E golden fixtures for bitwise regression"
```

- [ ] **Step 3: Add CMake target.**

Append to top-level `CMakeLists.txt`:

```cmake
find_program(PIXINSIGHT_SH PixInsight.sh HINTS /opt/PixInsight/bin)
find_program(XVFB_RUN xvfb-run)
if(PIXINSIGHT_SH)
    if(XVFB_RUN AND NOT DEFINED ENV{DISPLAY})
        add_custom_target(e2e
            COMMAND ${XVFB_RUN} -a -s "-screen 0 1280x1024x24"
                    ${PIXINSIGHT_SH} --automation-mode --force-exit
                    -r=${CMAKE_SOURCE_DIR}/tools/validate_e2e.js
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "E2E validation (xvfb)"
            USES_TERMINAL
        )
    else()
        add_custom_target(e2e
            COMMAND ${PIXINSIGHT_SH} --automation-mode --force-exit
                    -r=${CMAKE_SOURCE_DIR}/tools/validate_e2e.js
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "E2E validation"
            USES_TERMINAL
        )
    endif()
else()
    message(WARNING "PixInsight.sh not found — 'e2e' target unavailable")
endif()
```

- [ ] **Step 4: Test the target.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake .. && cmake --build . --target e2e 2>&1 | tail -10
```

Expected: returns 0.

- [ ] **Step 5: Commit.**

```bash
git add CMakeLists.txt
git commit -m "build: 'e2e' CMake target runs PJSR validation harness"
```

---

## Phase D — Release v4.0.0.4

### Task D1: Bump version

**Files:**
- Modify: `src/module/NukeXModule.cpp`

- [ ] **Step 1: Update macros.**

In `src/module/NukeXModule.cpp` (around lines 4-12):

```cpp
#define MODULE_VERSION_BUILD     4
#define MODULE_RELEASE_YEAR      2026
#define MODULE_RELEASE_MONTH     <ship month>
#define MODULE_RELEASE_DAY       <ship day>
```

- [ ] **Step 2: Build to confirm.**

```bash
cd /home/scarter4work/projects/nukex4/build
cmake --build . --target NukeX -j$(nproc)
```

- [ ] **Step 3: Commit.**

```bash
git add src/module/NukeXModule.cpp
git commit -m "chore: bump version to 4.0.0.4"
```

---

### Task D2: CHANGELOG

**Files:**
- Create or modify: `CHANGELOG.md`

- [ ] **Step 1: Add v4.0.0.4 section at the top.**

```markdown
# NukeX v4 — Changelog

## v4.0.0.4 — <ship date>

### Added
- **Stretch pipeline wired to output.** New `NukeX_stretched` window opens alongside stacked + noise.
- **Metadata-driven Auto selection.** `primaryStretch=Auto` reads FILTER/BAYERPAT/NAXIS3 and picks the Phase-5 champion for the detected class. Classification logged to Console.
- **Finishing stretch slot.** `finishingStretch` parameter (only `None` enrolled this release).

### Changed
- **Schema break:** `stretchType` + `autoStretch` replaced by `primaryStretch` + `finishingStretch`. Saved projects referencing the old IDs revert to defaults.
- **Phase B perf:** <N>x speedup on the reference LRGB workload (<pre>ms → <post>ms). Details in docs/superpowers/plans/2026-04-19-phase7-perf-findings.md.

### Validation
- New `make e2e` target drives a PJSR harness through `PixInsight.sh --automation-mode`. Enforces bitwise regression (stacked + noise), histogram sanity, dropdown-drives-pipeline, and Phase B perf gate across 4 real-data stacks.
```

- [ ] **Step 2: Commit.**

```bash
git add CHANGELOG.md
git commit -m "docs: CHANGELOG for v4.0.0.4"
```

---

### Task D3: Final gate — clean build + ctest + e2e + manual cancel

**Files:** none modified.

- [ ] **Step 1: Clean build.**

```bash
cd /home/scarter4work/projects/nukex4
rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build . -j$(nproc)
```

- [ ] **Step 2: ctest.**

```bash
ctest --output-on-failure
```

Expected: all PASS.

- [ ] **Step 3: E2E.**

```bash
cmake --build . --target e2e 2>&1 | tee /tmp/e2e_final.log
grep -E "status" build/e2e_report.json
```

Expected: `"status": "PASS"`.

- [ ] **Step 4: Manual cancel test.**

Follow `tools/manual_cancel_test.md`. Record the result.

- [ ] **Step 5: STOP if any gate fails.** No signing until all four are green.

---

### Task D4: Sign + package

**Files:** produces `repository/<date>-linux-x64-NukeX.tar.gz`, updated `repository/updates.xri`.

- [ ] **Step 1: Sign module.**

```bash
cd /home/scarter4work/projects/nukex4
cmake --build build --target sign
ls -la build/src/module/NukeX-pxm.so build/src/module/NukeX-pxm.xsgn
```

- [ ] **Step 2: Package.**

```bash
cmake --build build --target package
ls repository/
sha1sum repository/*.tar.gz
grep sha1 repository/updates.xri
```

Expected: new tarball, updates.xri updated + signed, SHA1 in manifest matches tarball.

---

### Task D5: Commit, tag, push

- [ ] **Step 1: Combined release commit.**

```bash
cd /home/scarter4work/projects/nukex4
git add repository/
git commit -m "release: v4.0.0.4 — stretch wiring + Phase B perf + E2E harness

Ships Phase 7 close-out per docs/superpowers/plans/2026-04-19-phase7-closeout-plan.md:
- Stretch pipeline wired into ExecuteGlobal with NukeX_stretched window
- Metadata-driven Auto curve selection (Phase-5 champion lookup)
- Schema break: primaryStretch + finishingStretch replace stretchType + autoStretch
- Phase B perf: >= 1.5x speedup on LRGB reference workload
- PJSR E2E harness with bitwise regression across 4-stack corpus

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 2: Tag.**

```bash
git tag v4.0.0.4 -m "v4.0.0.4 release"
```

- [ ] **Step 3: Push.**

```bash
git push origin main --tags
```

- [ ] **Step 4: Update memory.**

Edit `~/.claude/projects/-home-scarter4work-projects-nukex4/memory/project_phase7_closeout.md` to reflect the actual ship date and status `Phase 7 shipped as v4.0.0.4 on <date>`.

---

## Self-review

**Spec coverage (walking §1-§4 of the spec):**

- §1 Stretch wiring + Auto (C.1): A1-A9 cover FITSMetadata, FilterClassifier, StretchAutoSelector, StretchFactory, schema change, Instance storage, Interface UI, ExecuteGlobal wiring.
- §2 Perf profiling + 1.5x ship bar: B1-B6 cover instrumentation, profiling build, baseline capture, perf/flamegraph, optimization, verification.
- §3 E2E automation harness + 18 checks + bitwise + 4-stack corpus: C1 (manifest), C2 (scaffolding), C3 (checks 1-3), C4 (4-7), C5 (8-15), C6 (16-18), C7 (goldens + CMake target).
- §4 Release workflow: D1 (version), D2 (changelog), D3 (gate), D4 (sign+package), D5 (commit+tag+push).

Non-goals (SAS/OTS/Photometric impl, stats-tuning, adaptive DB, macOS/Windows): no tasks — respected.

Cross-machine GPU bitwise risk: documented in spec as a contingency; not addressed by a task (YAGNI until it actually fails).

**Placeholder scan:** no TBDs, no "add validation", no "similar to task N". Task B5's body depends on B4's findings — flagged explicitly ("this task's content is determined by the findings") rather than hand-waved. Every step shows code or exact commands.

**Type/name consistency:** `build_primary` / `build_finishing` / `select_auto` / `classify_filter`, `primaryStretch` / `finishingStretch`, `NukeX_stretched` — spelled identically across all tasks.
