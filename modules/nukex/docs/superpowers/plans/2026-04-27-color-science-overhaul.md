# Color Science Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace v4 hardcoded `OSC_RGB` Bayer routing with filter-taxonomy-driven Phase A routing + Phase B Q-matrix decomposition + Lab/LCH ColorComposer with calibrated emission-line palette, fixing the M27-green dual-narrowband bug and adding OSC-as-LRGB synthesis.

**Architecture:** Three new libs (`lib/io/FilterClassifier`, `lib/calibration/{QEDatabase,ChannelDecomposer}`, `lib/compose/{Palette,ColorComposer}`) feeding refactored `stacking_engine.cpp` Phase A router and Phase B Q-solve. Cube voxels gain a name-indexed slot lookup (still backed by fixed `MAX_CHANNELS=8` arrays). Module wires it all and emits 3-channel RGB from composer output. v4 `StackingMode::OSC_HAO3`/`OSC_S2O3`/`output_rgb_mapping` and the module-level `filter_classifier` are deleted; the existing 4-value `FilterClass` migrates to the new 5-value lib enum with Phase 8 rating-DB schema bump.

**Tech Stack:** C++17, CMake, Catch2 v3, nlohmann/json (already vendored via FetchContent), Eigen 3.4 (new vendor via FetchContent for pseudo-inverse), cfitsio (existing), PCL (PixInsight Class Library).

**Reference spec:** `docs/superpowers/specs/2026-04-26-color-science-overhaul-design.md` (committed at `317219d`).

**Reference research:** `research/qe_database_research.json` (22 sensors, 55 cameras, 87 filters).

---

## File Structure

### New code

| Path | Responsibility |
|---|---|
| `src/lib/io/include/nukex/io/filter.hpp` | `Filter` value type: `(FilterClass, FilterName, Camera, BandwidthSpec)` |
| `src/lib/io/include/nukex/io/filter_classifier.hpp` + `src/lib/io/src/filter_classifier.cpp` | `FrameMetadata → Filter`. 5-class enum. Tiered fallback per spec §6.3. |
| `src/lib/calibration/CMakeLists.txt` | New lib subdir |
| `src/lib/calibration/include/nukex/calibration/qe_database.hpp` + `src/lib/calibration/src/qe_database.cpp` | Loads `share/qe_database.json` + override JSON; exposes `lookup_camera_qe(camera, wavelength, photosite)` and `lookup_filter(name) → BandwidthSpec` |
| `src/lib/calibration/include/nukex/calibration/channel_decomposer.hpp` + `src/lib/calibration/src/channel_decomposer.cpp` | Builds Q matrix per `(camera, filter)`, returns per-pixel `solve(R, G, B)` via Eigen pseudo-inverse |
| `src/lib/compose/CMakeLists.txt` | New lib subdir |
| `src/lib/compose/include/nukex/compose/palette.hpp` + `src/lib/compose/src/palette.cpp` | Calibrated emission-line CIE-Lab vectors at 656/501/672 nm. Property-test invariant: no green-quadrant vector. |
| `src/lib/compose/include/nukex/compose/color_composer.hpp` + `src/lib/compose/src/color_composer.cpp` | Cube derived slots → 3-ch sRGB. Default Lab/LCH composite + opt-in continuum-subtract. |
| `share/qe_database.json` | Shipped DB (transformed from research JSON, drops `_meta`) |
| `tools/import_qe_research.py` | One-shot transform: research → shipped JSON, schema-validates output |
| `test/unit/io/test_filter_classifier.cpp` | Unit tests for new classifier |
| `test/unit/calibration/CMakeLists.txt` | New test subdir |
| `test/unit/calibration/test_qe_database.cpp` | Unit tests for DB load/override/missing |
| `test/unit/calibration/test_channel_decomposer.cpp` | Unit tests for Q-solve / singular-matrix / multi-camera lookup |
| `test/unit/compose/CMakeLists.txt` | New test subdir |
| `test/unit/compose/test_palette.cpp` | Unit + property tests for palette vectors |
| `test/unit/compose/test_color_composer.cpp` | Unit tests for Lab/LCH + continuum-subtract paths |
| `test/integration/CMakeLists.txt` | New (currently empty dir) |
| `test/integration/test_color_science_pipeline.cpp` | End-to-end synthetic FITS → derived semantic slots |
| `docs/qe_overrides_format.md` | User documentation for override JSON schema |

### Refactored existing code

| Path | Change |
|---|---|
| `src/lib/core/include/nukex/core/channel_config.hpp` + `src/lib/core/src/channel_config.cpp` | Add `ChannelConfig::from_filter(Filter)` factory; remove dead `OSC_HAO3`/`OSC_S2O3` switch cases + `output_rgb_mapping[3]` field; add named-slot lookup table `slot_name_to_index[]` |
| `src/lib/core/include/nukex/core/voxel.hpp` + `src/lib/core/src/voxel.cpp` (if exists) | Add `slot(string_view name)` helper that does name → index resolution via ChannelConfig (zero-cost — name list is small) |
| `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` + `src/lib/stacker/src/stacking_engine.cpp` | Add `qe_database_path` + `qe_override_path` to `Config`; replace hardcoded `OSC_RGB` at line 107 with Phase A Router; add Phase B Q-solve + ColorComposer hook in finalization; emit derived semantic slots into final output |
| `src/module/NukeXInstance.h` + `src/module/NukeXInstance.cpp` | Wire `QEDatabase` + new `FilterClassifier` + `ChannelDecomposer` + `ColorComposer` into engine config; emit ImageWindow always-3-ch from composer; add optional secondary window for raw line-emission channels; migrate `lastRun.filter_class` to new enum (rating-DB schema bump) |
| `src/module/NukeXInterface.h` + `src/module/NukeXInterface.cpp` | Add "QE override file…" file picker + browse button |
| `src/module/NukeXParameters.cpp` | Register `qeOverridePath` parameter |
| `src/module/stretch_auto_selector.{hpp,cpp}` | Migrate from old 4-value `FilterClass` to new 5-value enum from `lib/io/` |
| `src/module/RatingDialog.h` + `src/module/RatingDialog.cpp` | Constructor accepts new 5-value enum (or rating-int wrapper); learning DB schema bump (V2 → V3) |
| `src/lib/learning/include/nukex/learning/rating_database.hpp` + `src/lib/learning/src/rating_database.cpp` | Migration: V2→V3 adds new `filter_class` codes (5 values); maps legacy stored ints to new enum |
| `test/fixtures/e2e_manifest.json` | New baselines: `bayer_nb_hao3_m27`, `bayer_nb_s2o3_target`, `lrgbsho_target`. Refresh `mono_lrgb_ngc7635_v5` and `bayer_rgb_ngc7635_v5`. Preserve `sweep_ghs`/`sweep_mtf`/`sweep_arcsinh` bit-identically. |
| `src/module/NukeXVersion.h` | Bump to `5.0.0.0` |
| `repository/updates.xri` | New release manifest |
| `CHANGELOG.md` | v5.0.0.0 entry |

### Deletions (per no-stubs/no-dead-code rule)

| Path | What |
|---|---|
| `src/module/filter_classifier.hpp` + `src/module/filter_classifier.cpp` | Migrated to `src/lib/io/`. Module-level files deleted at end. |
| `src/lib/core/include/nukex/core/channel_config.hpp` (`StackingMode` enum + `output_rgb_mapping[3]`) | Replaced by `Filter` + `FilterClass`. All callers migrate to `ChannelConfig::from_filter()`. |

---

## Phase 1: Foundation Types (no pipeline coupling, fully testable in isolation)

### Task 1: `Filter` value type

**Files:**
- Create: `src/lib/io/include/nukex/io/filter.hpp`
- Test: `test/unit/io/test_filter.cpp`
- Modify: `test/unit/io/CMakeLists.txt` (register new test)

- [ ] **Step 1: Write the failing test**

Create `test/unit/io/test_filter.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/io/filter.hpp"

using namespace nukex;

TEST_CASE("Filter: default-constructed is unknown", "[filter]") {
    Filter f;
    REQUIRE(f.cls == FilterClass::UNKNOWN);
    REQUIRE(f.name.empty());
    REQUIRE(f.camera.empty());
    REQUIRE(f.bandwidth.center_nm == 0.0);
    REQUIRE(f.bandwidth.fwhm_nm == 0.0);
}

TEST_CASE("Filter: explicit construction holds all fields", "[filter]") {
    Filter f{
        FilterClass::DUAL_NB_OSC,
        "HaO3",
        "ASI585MC",
        BandwidthSpec{578.5, 155.0}
    };
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "HaO3");
    REQUIRE(f.camera == "ASI585MC");
    REQUIRE(f.bandwidth.center_nm == Catch::Approx(578.5));
    REQUIRE(f.bandwidth.fwhm_nm == Catch::Approx(155.0));
}

TEST_CASE("FilterClass enum exposes 5 + UNKNOWN values", "[filter]") {
    REQUIRE(static_cast<int>(FilterClass::UNKNOWN)            == 0);
    REQUIRE(static_cast<int>(FilterClass::BROADBAND_L)        == 1);
    REQUIRE(static_cast<int>(FilterClass::BROADBAND_RGB)      == 2);
    REQUIRE(static_cast<int>(FilterClass::BROADBAND_OSC)      == 3);
    REQUIRE(static_cast<int>(FilterClass::NARROWBAND_SINGLE)  == 4);
    REQUIRE(static_cast<int>(FilterClass::DUAL_NB_OSC)        == 5);
}

TEST_CASE("filter_class_name returns stable strings", "[filter]") {
    REQUIRE(std::string(filter_class_name(FilterClass::BROADBAND_L))       == "BROADBAND_L");
    REQUIRE(std::string(filter_class_name(FilterClass::BROADBAND_RGB))     == "BROADBAND_RGB");
    REQUIRE(std::string(filter_class_name(FilterClass::BROADBAND_OSC))     == "BROADBAND_OSC");
    REQUIRE(std::string(filter_class_name(FilterClass::NARROWBAND_SINGLE)) == "NARROWBAND_SINGLE");
    REQUIRE(std::string(filter_class_name(FilterClass::DUAL_NB_OSC))       == "DUAL_NB_OSC");
    REQUIRE(std::string(filter_class_name(FilterClass::UNKNOWN))           == "UNKNOWN");
}
```

- [ ] **Step 2: Wire the test into CMake**

Edit `test/unit/io/CMakeLists.txt` and add the registration line for `test_filter` following the existing pattern:

```cmake
nukex_add_test(test_filter test_filter.cpp nukex4_io)
```

- [ ] **Step 3: Run test to verify it fails to build (header missing)**

Run: `cd build && cmake .. && make test_filter 2>&1 | head -20`
Expected: build error "filter.hpp: No such file or directory"

- [ ] **Step 4: Implement `filter.hpp`**

Create `src/lib/io/include/nukex/io/filter.hpp`:

```cpp
#ifndef NUKEX_IO_FILTER_HPP
#define NUKEX_IO_FILTER_HPP

#include <string>

namespace nukex {

enum class FilterClass : int {
    UNKNOWN            = 0,
    BROADBAND_L        = 1,
    BROADBAND_RGB      = 2,
    BROADBAND_OSC      = 3,
    NARROWBAND_SINGLE  = 4,
    DUAL_NB_OSC        = 5,
};

const char* filter_class_name(FilterClass c);

struct BandwidthSpec {
    double center_nm = 0.0;
    double fwhm_nm   = 0.0;
};

struct Filter {
    FilterClass    cls = FilterClass::UNKNOWN;
    std::string    name;
    std::string    camera;
    BandwidthSpec  bandwidth;
};

} // namespace nukex

#endif
```

- [ ] **Step 5: Implement `filter_class_name` (inline in a small `.cpp` to keep the header pure)**

Add a new file `src/lib/io/src/filter.cpp`:

```cpp
#include "nukex/io/filter.hpp"

namespace nukex {

const char* filter_class_name(FilterClass c) {
    switch (c) {
        case FilterClass::UNKNOWN:           return "UNKNOWN";
        case FilterClass::BROADBAND_L:       return "BROADBAND_L";
        case FilterClass::BROADBAND_RGB:     return "BROADBAND_RGB";
        case FilterClass::BROADBAND_OSC:     return "BROADBAND_OSC";
        case FilterClass::NARROWBAND_SINGLE: return "NARROWBAND_SINGLE";
        case FilterClass::DUAL_NB_OSC:       return "DUAL_NB_OSC";
    }
    return "UNKNOWN";
}

} // namespace nukex
```

Add `src/filter.cpp` to the `nukex4_io` library source list in `src/lib/io/CMakeLists.txt`.

- [ ] **Step 6: Run test to verify pass**

Run: `cd build && make test_filter && ctest -R test_filter --output-on-failure`
Expected: PASS, 4 test cases, all assertions green.

- [ ] **Step 7: Commit**

```bash
git add src/lib/io/include/nukex/io/filter.hpp \
        src/lib/io/src/filter.cpp \
        src/lib/io/CMakeLists.txt \
        test/unit/io/test_filter.cpp \
        test/unit/io/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(io): introduce Filter value type with 5-class FilterClass enum

Foundation for color-science overhaul. Filter is the canonical record
of (FilterClass, name, camera, bandwidth) consumed by Phase A routing
and Phase B Q-solve. Five-class taxonomy expands the v4 four-class
classifier to distinguish single-line narrowband from dual-NB OSC.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `FilterClassifier` — `FrameMetadata → Filter` with tiered fallback

**Files:**
- Create: `src/lib/io/include/nukex/io/filter_classifier.hpp`
- Create: `src/lib/io/src/filter_classifier.cpp`
- Test: `test/unit/io/test_filter_classifier.cpp`
- Modify: `test/unit/io/CMakeLists.txt`

The classifier resolves missing/unknown FILTER per spec §6.3:
- Bayer + missing FILTER → `BROADBAND_OSC` (silent, common case)
- Mono + missing FILTER → `BROADBAND_L` with name `"L_unnamed"` (silent)
- Mono + unknown FILTER → `BROADBAND_L` with the FILTER value as name (loud warning)
- Bayer + unknown FILTER → `UNKNOWN` sentinel (caller must fail loud at batch start)

Filter-name matching uses a lookup table with normalized aliases (lowercase, strip non-alphanumerics, collapse `-`/`_`/space).

- [ ] **Step 1: Write the failing tests**

Create `test/unit/io/test_filter_classifier.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/core/frame_metadata.hpp"

using namespace nukex;

static FrameMetadata make_meta(const std::string& filter,
                               const std::string& bayer = "",
                               const std::string& instrument = "ASI585MC") {
    FrameMetadata m;
    m.filter        = filter;
    m.bayer_pattern = bayer;
    m.instrument    = instrument;
    return m;
}

TEST_CASE("FilterClassifier: HaO3 dual-NB on Bayer", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("HaO3", "RGGB"));
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "HaO3");
    REQUIRE(f.camera == "ASI585MC");
}

TEST_CASE("FilterClassifier: S2O3 dual-NB on Bayer", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("S2O3", "RGGB"));
    REQUIRE(f.cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(f.name == "S2O3");
}

TEST_CASE("FilterClassifier: Hα single-line narrowband on mono", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("Ha"))      .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("Halpha"))  .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("H-alpha")) .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("OIII"))    .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("O3"))      .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("SII"))     .cls == FilterClass::NARROWBAND_SINGLE);
    REQUIRE(c.classify(make_meta("S2"))      .cls == FilterClass::NARROWBAND_SINGLE);
}

TEST_CASE("FilterClassifier: L / R / G / B broadband on mono", "[filter_classifier]") {
    FilterClassifier c;
    REQUIRE(c.classify(make_meta("L"))         .cls == FilterClass::BROADBAND_L);
    REQUIRE(c.classify(make_meta("Luminance")) .cls == FilterClass::BROADBAND_L);
    REQUIRE(c.classify(make_meta("R"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Red"))       .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("G"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Green"))     .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("B"))         .cls == FilterClass::BROADBAND_RGB);
    REQUIRE(c.classify(make_meta("Blue"))      .cls == FilterClass::BROADBAND_RGB);
}

TEST_CASE("FilterClassifier: alias normalization", "[filter_classifier]") {
    FilterClassifier c;
    // L-eXtreme variants should all match
    REQUIRE(c.classify(make_meta("L-eXtreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("L_eXtreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("L Extreme")).cls == FilterClass::DUAL_NB_OSC);
    REQUIRE(c.classify(make_meta("LExtreme") ).cls == FilterClass::DUAL_NB_OSC);
}

TEST_CASE("FilterClassifier: missing FILTER + Bayer → BROADBAND_OSC silent", "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("", "RGGB"));
    REQUIRE(f.cls == FilterClass::BROADBAND_OSC);
    REQUIRE(f.name == "OSC");
}

TEST_CASE("FilterClassifier: missing FILTER + mono → BROADBAND_L L_unnamed silent",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("", ""));
    REQUIRE(f.cls == FilterClass::BROADBAND_L);
    REQUIRE(f.name == "L_unnamed");
}

TEST_CASE("FilterClassifier: unknown FILTER + Bayer → UNKNOWN sentinel",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("ALP-T-fake-2026", "RGGB"));
    REQUIRE(f.cls == FilterClass::UNKNOWN);
    REQUIRE(f.name == "ALP-T-fake-2026");
}

TEST_CASE("FilterClassifier: unknown FILTER + mono → BROADBAND_L preserve name + warn",
          "[filter_classifier]") {
    FilterClassifier c;
    Filter f = c.classify(make_meta("custom_Hbeta", ""));
    REQUIRE(f.cls == FilterClass::BROADBAND_L);
    REQUIRE(f.name == "custom_Hbeta");
    REQUIRE(c.last_warning().find("Unknown filter") != std::string::npos);
    REQUIRE(c.last_warning().find("custom_Hbeta")   != std::string::npos);
}
```

Add registration in `test/unit/io/CMakeLists.txt`:

```cmake
nukex_add_test(test_filter_classifier test_filter_classifier.cpp nukex4_io nukex4_core)
```

- [ ] **Step 2: Run to verify it fails (header missing)**

Run: `cd build && cmake .. && make test_filter_classifier 2>&1 | head -10`
Expected: build error "filter_classifier.hpp: No such file or directory"

- [ ] **Step 3: Implement the header**

Create `src/lib/io/include/nukex/io/filter_classifier.hpp`:

```cpp
#ifndef NUKEX_IO_FILTER_CLASSIFIER_HPP
#define NUKEX_IO_FILTER_CLASSIFIER_HPP

#include "nukex/io/filter.hpp"

#include <string>

namespace nukex {

struct FrameMetadata;

class FilterClassifier {
public:
    FilterClassifier() = default;

    Filter classify(const FrameMetadata& meta);

    const std::string& last_warning() const { return last_warning_; }

private:
    std::string last_warning_;

    static std::string normalize_name(const std::string& raw);
    static FilterClass lookup_known(const std::string& normalized,
                                    std::string& canonical_name_out,
                                    BandwidthSpec& bandwidth_out);
};

} // namespace nukex

#endif
```

- [ ] **Step 4: Implement the classifier `.cpp`**

Create `src/lib/io/src/filter_classifier.cpp`:

```cpp
#include "nukex/io/filter_classifier.hpp"
#include "nukex/core/frame_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace nukex {

namespace {

struct KnownFilter {
    FilterClass    cls;
    const char*    canonical;
    double         center_nm;
    double         fwhm_nm;
};

const std::unordered_map<std::string, KnownFilter>& known_table() {
    static const std::unordered_map<std::string, KnownFilter> table = {
        {"l",         {FilterClass::BROADBAND_L,       "L",   550.0, 300.0}},
        {"luminance", {FilterClass::BROADBAND_L,       "L",   550.0, 300.0}},
        {"r",         {FilterClass::BROADBAND_RGB,     "R",   620.0, 100.0}},
        {"red",       {FilterClass::BROADBAND_RGB,     "R",   620.0, 100.0}},
        {"g",         {FilterClass::BROADBAND_RGB,     "G",   540.0, 100.0}},
        {"green",     {FilterClass::BROADBAND_RGB,     "G",   540.0, 100.0}},
        {"b",         {FilterClass::BROADBAND_RGB,     "B",   460.0, 100.0}},
        {"blue",      {FilterClass::BROADBAND_RGB,     "B",   460.0, 100.0}},

        {"ha",        {FilterClass::NARROWBAND_SINGLE, "Ha",   656.3,   7.0}},
        {"halpha",    {FilterClass::NARROWBAND_SINGLE, "Ha",   656.3,   7.0}},
        {"oiii",      {FilterClass::NARROWBAND_SINGLE, "OIII", 500.7,   7.0}},
        {"o3",        {FilterClass::NARROWBAND_SINGLE, "OIII", 500.7,   7.0}},
        {"sii",       {FilterClass::NARROWBAND_SINGLE, "SII",  672.4,   7.0}},
        {"s2",        {FilterClass::NARROWBAND_SINGLE, "SII",  672.4,   7.0}},

        {"hao3",      {FilterClass::DUAL_NB_OSC,       "HaO3", 578.5, 155.0}},
        {"haoiii",    {FilterClass::DUAL_NB_OSC,       "HaO3", 578.5, 155.0}},
        {"s2o3",      {FilterClass::DUAL_NB_OSC,       "S2O3", 586.0, 170.0}},
        {"siioiii",   {FilterClass::DUAL_NB_OSC,       "S2O3", 586.0, 170.0}},
        {"lextreme",  {FilterClass::DUAL_NB_OSC,       "L-eXtreme", 578.5, 7.0}},
        {"lenhance",  {FilterClass::DUAL_NB_OSC,       "L-eNhance", 578.5, 25.0}},
        {"luempro",   {FilterClass::DUAL_NB_OSC,       "L-uMpro",   578.5, 35.0}},
        {"alpt",      {FilterClass::DUAL_NB_OSC,       "ALP-T",     578.5, 5.0}},
    };
    return table;
}

} // namespace

std::string FilterClassifier::normalize_name(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

FilterClass FilterClassifier::lookup_known(const std::string& normalized,
                                           std::string& canonical_name_out,
                                           BandwidthSpec& bandwidth_out) {
    const auto& table = known_table();
    auto it = table.find(normalized);
    if (it == table.end()) return FilterClass::UNKNOWN;
    canonical_name_out      = it->second.canonical;
    bandwidth_out.center_nm = it->second.center_nm;
    bandwidth_out.fwhm_nm   = it->second.fwhm_nm;
    return it->second.cls;
}

Filter FilterClassifier::classify(const FrameMetadata& meta) {
    last_warning_.clear();

    const bool        is_bayer  = !meta.bayer_pattern.empty();
    const std::string normalized = normalize_name(meta.filter);

    Filter out;
    out.camera = meta.instrument;

    if (normalized.empty()) {
        if (is_bayer) {
            out.cls            = FilterClass::BROADBAND_OSC;
            out.name           = "OSC";
            out.bandwidth      = BandwidthSpec{550.0, 300.0};
        } else {
            out.cls            = FilterClass::BROADBAND_L;
            out.name           = "L_unnamed";
            out.bandwidth      = BandwidthSpec{550.0, 300.0};
        }
        return out;
    }

    std::string   canonical;
    BandwidthSpec bw;
    FilterClass   cls = lookup_known(normalized, canonical, bw);
    if (cls != FilterClass::UNKNOWN) {
        out.cls       = cls;
        out.name      = canonical;
        out.bandwidth = bw;
        return out;
    }

    // Unknown filter
    if (is_bayer) {
        out.cls  = FilterClass::UNKNOWN;
        out.name = meta.filter; // preserve original spelling for the loud-fail message
    } else {
        out.cls       = FilterClass::BROADBAND_L;
        out.name      = meta.filter;
        out.bandwidth = BandwidthSpec{550.0, 300.0};
        last_warning_ = "Unknown filter '" + meta.filter
                      + "' for mono frame — treating as generic luminance. "
                      + "If this is a narrowband filter, add it to qe_overrides.json.";
    }
    return out;
}

} // namespace nukex
```

Add `src/filter_classifier.cpp` to the `nukex4_io` source list.

- [ ] **Step 5: Run tests to verify pass**

Run: `cd build && make test_filter_classifier && ctest -R test_filter_classifier --output-on-failure`
Expected: PASS, all 9 test cases green.

- [ ] **Step 6: Commit**

```bash
git add src/lib/io/include/nukex/io/filter_classifier.hpp \
        src/lib/io/src/filter_classifier.cpp \
        src/lib/io/CMakeLists.txt \
        test/unit/io/test_filter_classifier.cpp \
        test/unit/io/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(io): FilterClassifier with 5-class taxonomy + tiered unknown handling

Resolves FrameMetadata.filter into a Filter value type. Tiered policy:
known filters → canonical class; missing FILTER + Bayer → BROADBAND_OSC
silent; missing + mono → BROADBAND_L "L_unnamed" silent; unknown + Bayer
→ UNKNOWN sentinel (caller must fail loud); unknown + mono → BROADBAND_L
preserving name + warning. Implements spec §6.3.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `QEDatabase` — JSON loader with override merge

**Files:**
- Create: `src/lib/calibration/CMakeLists.txt`
- Create: `src/lib/calibration/include/nukex/calibration/qe_database.hpp`
- Create: `src/lib/calibration/src/qe_database.cpp`
- Test: `test/unit/calibration/CMakeLists.txt` + `test/unit/calibration/test_qe_database.cpp`
- Modify: root `CMakeLists.txt` to register new subdirs
- Create: `test/fixtures/qe/minimal_db.json` + `qe/override.json` + `qe/malformed.json`

The DB exposes:
- `lookup_camera_qe(camera, wavelength_nm, photosite) → double` — interpolated/nearest QE
- `lookup_filter(name) → BandwidthSpec` — augments the FilterClassifier defaults
- `has_camera(name)`, `has_filter(name)` — for upfront validation
- `confidence(camera) → enum {HIGH, MEDIUM, LOW, UNKNOWN}` — drives FITS-keyword annotation

Override merge: override entries replace shipped entries on key collision; new entries from override are added.

- [ ] **Step 1: Add new subdir to root CMakeLists.txt**

Edit root `CMakeLists.txt` and add `add_subdirectory(src/lib/calibration)` between `add_subdirectory(src/lib/alignment)` and `add_subdirectory(src/lib/classify)`:

```cmake
add_subdirectory(src/lib/core)
add_subdirectory(src/lib/io)
add_subdirectory(src/lib/alignment)
add_subdirectory(src/lib/calibration)   # NEW
add_subdirectory(src/lib/classify)
add_subdirectory(src/lib/combine)
add_subdirectory(src/lib/compose)        # NEW (Task 5)
add_subdirectory(src/lib/fitting)
add_subdirectory(src/lib/gpu)
add_subdirectory(src/lib/stacker)
add_subdirectory(src/lib/stretch)
add_subdirectory(src/lib/learning)
add_subdirectory(src/module)
```

- [ ] **Step 2: Create the calibration CMakeLists.txt**

Create `src/lib/calibration/CMakeLists.txt`:

```cmake
add_library(nukex4_calibration STATIC
    src/qe_database.cpp
)

target_include_directories(nukex4_calibration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_calibration
    PUBLIC  nukex4_core nukex4_io
    PRIVATE nlohmann_json::nlohmann_json
)

target_compile_features(nukex4_calibration PUBLIC cxx_std_17)
```

- [ ] **Step 3: Create the test subdir and CMakeLists.txt**

Create `test/unit/calibration/CMakeLists.txt`:

```cmake
nukex_add_test(test_qe_database test_qe_database.cpp nukex4_calibration nukex4_io nukex4_core)
```

Add `add_subdirectory(unit/calibration)` to `test/CMakeLists.txt` (preserve alphabetical order with the existing `unit/X` entries).

- [ ] **Step 4: Create the test fixtures**

Create `test/fixtures/qe/minimal_db.json`:

```json
{
  "schema_version": 1,
  "cameras": {
    "ASI585MC": {
      "sensor": "IMX585",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "486": { "R": 0.03, "G": 0.65, "B": 0.70 },
        "501": { "R": 0.03, "G": 0.85, "B": 0.50 },
        "656": { "R": 0.73, "G": 0.32, "B": 0.03 },
        "672": { "R": 0.71, "G": 0.33, "B": 0.05 }
      },
      "confidence": "high"
    },
    "ASI2600MC": {
      "sensor": "IMX571",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "486": { "R": 0.07, "G": 0.78, "B": 0.86 },
        "501": { "R": 0.08, "G": 0.89, "B": 0.60 },
        "656": { "R": 0.46, "G": 0.05, "B": 0.04 },
        "672": { "R": 0.42, "G": 0.05, "B": 0.05 }
      },
      "confidence": "high"
    }
  },
  "filters": {
    "HaO3": {
      "type": "DUAL_NB",
      "lines": [
        { "name": "Ha",   "wavelength_nm": 656.3, "fwhm_nm": 7.0 },
        { "name": "OIII", "wavelength_nm": 500.7, "fwhm_nm": 7.0 }
      ]
    },
    "S2O3": {
      "type": "DUAL_NB",
      "lines": [
        { "name": "SII",  "wavelength_nm": 672.4, "fwhm_nm": 7.0 },
        { "name": "OIII", "wavelength_nm": 500.7, "fwhm_nm": 7.0 }
      ]
    }
  }
}
```

Create `test/fixtures/qe/override.json` (same schema, only fields meant to win):

```json
{
  "schema_version": 1,
  "cameras": {
    "ASI585MC": {
      "sensor": "IMX585",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "656": { "R": 0.99, "G": 0.99, "B": 0.99 }
      },
      "confidence": "high"
    },
    "Custom_Cam_1": {
      "sensor": "Custom",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "656": { "R": 0.50, "G": 0.10, "B": 0.05 }
      },
      "confidence": "low"
    }
  }
}
```

Create `test/fixtures/qe/malformed.json`:

```json
{ "schema_version": 1, "cameras": { "Bad": { qe: { } } } }
```

- [ ] **Step 5: Write the failing tests**

Create `test/unit/calibration/test_qe_database.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/calibration/qe_database.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

static fs::path fixture(const char* name) {
    return fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / name;
}

TEST_CASE("QEDatabase: load shipped DB, lookup camera QE", "[qe_database]") {
    QEDatabase db;
    auto result = db.load_shipped(fixture("minimal_db.json").string());
    REQUIRE(result.ok);
    REQUIRE(db.has_camera("ASI585MC"));
    REQUIRE(db.has_camera("ASI2600MC"));
    REQUIRE_FALSE(db.has_camera("DoesNotExist"));

    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.73).margin(0.01));
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::G) == Catch::Approx(0.32).margin(0.01));
    REQUIRE(db.lookup_camera_qe("ASI585MC", 500.7, Photosite::B) == Catch::Approx(0.50).margin(0.02));
}

TEST_CASE("QEDatabase: filter passband lookup", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.has_filter("HaO3"));
    auto bw = db.lookup_filter("HaO3");
    REQUIRE(bw.lines.size() == 2);
    REQUIRE(bw.lines[0].name == "Ha");
    REQUIRE(bw.lines[0].wavelength_nm == Catch::Approx(656.3));
    REQUIRE(bw.lines[1].name == "OIII");
    REQUIRE(bw.lines[1].wavelength_nm == Catch::Approx(500.7));
}

TEST_CASE("QEDatabase: override merge, override wins on collision", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);

    // Pre-override
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.73).margin(0.01));

    auto ov = db.load_override(fixture("override.json").string());
    REQUIRE(ov.ok);

    // Override wins on collision
    REQUIRE(db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R) == Catch::Approx(0.99).margin(0.01));

    // New camera from override is added
    REQUIRE(db.has_camera("Custom_Cam_1"));
    REQUIRE(db.lookup_camera_qe("Custom_Cam_1", 656.3, Photosite::R) == Catch::Approx(0.50).margin(0.01));
}

TEST_CASE("QEDatabase: missing shipped DB → loud fail", "[qe_database]") {
    QEDatabase db;
    auto r = db.load_shipped("/nonexistent/path/to/qe.json");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("missing") != std::string::npos);
}

TEST_CASE("QEDatabase: malformed override → loud fail with line/col", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    auto r = db.load_override(fixture("malformed.json").string());
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("malformed") != std::string::npos);
    // nlohmann json reports a byte offset, surfaced as "at line N, col M"
    REQUIRE(r.error.find("line") != std::string::npos);
}

TEST_CASE("QEDatabase: confidence enum reads from JSON", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.confidence("ASI585MC") == QEConfidence::HIGH);
    REQUIRE(db.confidence("Unknown")  == QEConfidence::UNKNOWN);
}
```

Wire `NUKEX_TEST_FIXTURES_DIR` define in `test/CMakeLists.txt` (top-level), if not already, by adding:

```cmake
add_compile_definitions(NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")
```

- [ ] **Step 6: Verify it fails to build**

Run: `cd build && cmake .. && make test_qe_database 2>&1 | tail -10`
Expected: build error "qe_database.hpp: No such file or directory"

- [ ] **Step 7: Implement the header**

Create `src/lib/calibration/include/nukex/calibration/qe_database.hpp`:

```cpp
#ifndef NUKEX_CALIBRATION_QE_DATABASE_HPP
#define NUKEX_CALIBRATION_QE_DATABASE_HPP

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nukex {

enum class Photosite { R, G, B, MONO_PEAK };

enum class QEConfidence { UNKNOWN, LOW, MEDIUM, HIGH };

struct EmissionLine {
    std::string name;
    double      wavelength_nm = 0.0;
    double      fwhm_nm       = 0.0;
};

struct FilterPassband {
    std::vector<EmissionLine> lines;
    std::string               type;     // "DUAL_NB", "BROADBAND", etc.
};

struct CameraQE {
    std::string                     sensor;
    std::string                     type;       // "OSC" / "mono" / "both-variants"
    std::string                     bayer;      // "RGGB", "BGGR", etc. (empty for mono)
    std::map<int, std::map<Photosite, double>>  qe_by_wavelength;  // sorted by wavelength
    QEConfidence                    confidence = QEConfidence::UNKNOWN;
};

struct LoadResult {
    bool        ok = false;
    std::string error;
};

class QEDatabase {
public:
    QEDatabase() = default;

    LoadResult load_shipped(const std::string& path);
    LoadResult load_override(const std::string& path);

    bool has_camera(const std::string& name) const;
    bool has_filter(const std::string& name) const;

    QEConfidence confidence(const std::string& camera) const;

    // Returns 0.0 if camera unknown or wavelength out of bounds.
    // Otherwise: nearest-wavelength QE if outside data range, linear interpolation otherwise.
    double lookup_camera_qe(const std::string& camera,
                            double             wavelength_nm,
                            Photosite          photosite) const;

    FilterPassband lookup_filter(const std::string& name) const;

    int n_cameras() const { return static_cast<int>(cameras_.size()); }
    int n_filters() const { return static_cast<int>(filters_.size()); }

private:
    std::unordered_map<std::string, CameraQE>       cameras_;
    std::unordered_map<std::string, FilterPassband> filters_;

    LoadResult parse_and_merge(const std::string& path, bool is_override);
};

} // namespace nukex

#endif
```

- [ ] **Step 8: Implement the .cpp**

Create `src/lib/calibration/src/qe_database.cpp`:

```cpp
#include "nukex/calibration/qe_database.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nukex {

namespace {

QEConfidence parse_confidence(const std::string& s) {
    if (s == "high")   return QEConfidence::HIGH;
    if (s == "medium") return QEConfidence::MEDIUM;
    if (s == "low")    return QEConfidence::LOW;
    return QEConfidence::UNKNOWN;
}

Photosite parse_photosite_key(const std::string& key) {
    if (key == "R")            return Photosite::R;
    if (key == "G")            return Photosite::G;
    if (key == "B")            return Photosite::B;
    if (key == "Gr" || key == "Gb") return Photosite::G;
    return Photosite::MONO_PEAK;
}

} // namespace

LoadResult QEDatabase::load_shipped(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {false, "QE database missing or unreadable: " + path};
    }
    return parse_and_merge(path, /*is_override*/ false);
}

LoadResult QEDatabase::load_override(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {false, "QE override file missing or unreadable: " + path};
    }
    return parse_and_merge(path, /*is_override*/ true);
}

LoadResult QEDatabase::parse_and_merge(const std::string& path, bool is_override) {
    using nlohmann::json;
    std::ifstream f(path);
    if (!f.is_open()) {
        return {false, std::string(is_override ? "QE override" : "QE database") + " missing: " + path};
    }
    json doc;
    try {
        f >> doc;
    } catch (const json::parse_error& e) {
        std::ostringstream oss;
        oss << (is_override ? "QE override" : "QE database")
            << " is malformed at line " << e.byte
            << " (parser: " << e.what() << ")";
        return {false, oss.str()};
    }

    if (doc.contains("cameras") && doc["cameras"].is_object()) {
        for (auto it = doc["cameras"].begin(); it != doc["cameras"].end(); ++it) {
            const std::string& name = it.key();
            const json& cam_json    = it.value();
            CameraQE cam;
            if (cam_json.contains("sensor")) cam.sensor = cam_json["sensor"].get<std::string>();
            if (cam_json.contains("type"))   cam.type   = cam_json["type"].get<std::string>();
            if (cam_json.contains("bayer"))  cam.bayer  = cam_json["bayer"].get<std::string>();
            if (cam_json.contains("confidence")) {
                cam.confidence = parse_confidence(cam_json["confidence"].get<std::string>());
            }
            if (cam_json.contains("qe") && cam_json["qe"].is_object()) {
                for (auto wlit = cam_json["qe"].begin(); wlit != cam_json["qe"].end(); ++wlit) {
                    int wl = std::stoi(wlit.key());
                    std::map<Photosite, double> per_site;
                    for (auto pit = wlit.value().begin(); pit != wlit.value().end(); ++pit) {
                        per_site[parse_photosite_key(pit.key())] = pit.value().get<double>();
                    }
                    cam.qe_by_wavelength[wl] = per_site;
                }
            }
            // Override semantics: replace whole camera record.
            // (Coarse but reflects spec: "override wins on key collision".)
            cameras_[name] = std::move(cam);
        }
    }

    if (doc.contains("filters") && doc["filters"].is_object()) {
        for (auto it = doc["filters"].begin(); it != doc["filters"].end(); ++it) {
            const std::string& name = it.key();
            const json& fjson = it.value();
            FilterPassband fp;
            if (fjson.contains("type")) fp.type = fjson["type"].get<std::string>();
            if (fjson.contains("lines") && fjson["lines"].is_array()) {
                for (const auto& line : fjson["lines"]) {
                    EmissionLine el;
                    if (line.contains("name")) el.name = line["name"].get<std::string>();
                    if (line.contains("wavelength_nm")) el.wavelength_nm = line["wavelength_nm"].get<double>();
                    if (line.contains("fwhm_nm"))      el.fwhm_nm       = line["fwhm_nm"].get<double>();
                    fp.lines.push_back(el);
                }
            }
            filters_[name] = std::move(fp);
        }
    }

    return {true, ""};
}

bool QEDatabase::has_camera(const std::string& name) const {
    return cameras_.find(name) != cameras_.end();
}

bool QEDatabase::has_filter(const std::string& name) const {
    return filters_.find(name) != filters_.end();
}

QEConfidence QEDatabase::confidence(const std::string& camera) const {
    auto it = cameras_.find(camera);
    if (it == cameras_.end()) return QEConfidence::UNKNOWN;
    return it->second.confidence;
}

double QEDatabase::lookup_camera_qe(const std::string& camera,
                                    double             wavelength_nm,
                                    Photosite          photosite) const {
    auto it = cameras_.find(camera);
    if (it == cameras_.end()) return 0.0;
    const auto& curve = it->second.qe_by_wavelength;
    if (curve.empty()) return 0.0;

    auto upper = curve.upper_bound(static_cast<int>(wavelength_nm + 0.5));
    if (upper == curve.begin()) {
        auto p = upper->second.find(photosite);
        return (p == upper->second.end()) ? 0.0 : p->second;
    }
    if (upper == curve.end()) {
        auto last = std::prev(upper);
        auto p = last->second.find(photosite);
        return (p == last->second.end()) ? 0.0 : p->second;
    }
    auto lower = std::prev(upper);
    auto pl = lower->second.find(photosite);
    auto pu = upper->second.find(photosite);
    if (pl == lower->second.end() || pu == upper->second.end()) return 0.0;
    double t = (wavelength_nm - lower->first) / static_cast<double>(upper->first - lower->first);
    return pl->second + t * (pu->second - pl->second);
}

FilterPassband QEDatabase::lookup_filter(const std::string& name) const {
    auto it = filters_.find(name);
    if (it == filters_.end()) return {};
    return it->second;
}

} // namespace nukex
```

- [ ] **Step 9: Run tests to verify pass**

Run: `cd build && cmake .. && make test_qe_database && ctest -R test_qe_database --output-on-failure`
Expected: PASS, all 6 test cases green.

- [ ] **Step 10: Commit**

```bash
git add src/lib/calibration/ \
        test/unit/calibration/ \
        test/fixtures/qe/ \
        CMakeLists.txt \
        test/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(calibration): QEDatabase loader with shipped + override merge

Loads share/qe_database.json and optional ~/.nukex4/qe_overrides.json.
Override wins on camera-key collision; new override entries are added.
Linear interpolation between adjacent wavelength samples; nearest-edge
extrapolation. Loud-fail on missing shipped DB or malformed JSON (with
parser line/col). Confidence enum drives downstream FITS-keyword
annotation. Ships with unit tests covering load/override-merge/missing/
malformed/lookup/confidence paths.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: `ChannelDecomposer` — Q-matrix solve via Eigen pseudo-inverse

**Files:**
- Modify: `src/lib/calibration/CMakeLists.txt` (add Eigen FetchContent + new source file)
- Create: `src/lib/calibration/include/nukex/calibration/channel_decomposer.hpp`
- Create: `src/lib/calibration/src/channel_decomposer.cpp`
- Test: `test/unit/calibration/test_channel_decomposer.cpp`
- Modify: `test/unit/calibration/CMakeLists.txt`

For HaO3 + ASI585MC the Q matrix is 3×2 (RGB → Hα, OIII):

```
Q = [ QE_R(656)  QE_R(501) ]
    [ QE_G(656)  QE_G(501) ]
    [ QE_B(656)  QE_B(501) ]
```

The solve `(Hα, OIII) = pinv(Q) @ (R, G, B)` recovers line emissions. For S2O3 same shape with (672, 501).

**Numerical note:** Use Eigen's `colPivHouseholderQr().solve()` for least-squares — more numerically stable than computing pinv explicitly via SVD, and adequate for small 3×2/3×3 matrices.

- [ ] **Step 1: Vendor Eigen via FetchContent**

Edit `src/lib/calibration/CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    Eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(Eigen)
if(NOT eigen_POPULATED)
    FetchContent_Populate(Eigen)
    add_library(Eigen3_headers INTERFACE)
    target_include_directories(Eigen3_headers INTERFACE ${eigen_SOURCE_DIR})
    add_library(Eigen3::Eigen ALIAS Eigen3_headers)
endif()

add_library(nukex4_calibration STATIC
    src/qe_database.cpp
    src/channel_decomposer.cpp
)

target_include_directories(nukex4_calibration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_calibration
    PUBLIC  nukex4_core nukex4_io
    PRIVATE nlohmann_json::nlohmann_json Eigen3::Eigen
)

target_compile_features(nukex4_calibration PUBLIC cxx_std_17)
```

- [ ] **Step 2: Write the failing tests**

Create `test/unit/calibration/test_channel_decomposer.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/calibration/channel_decomposer.hpp"
#include "nukex/calibration/qe_database.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

static fs::path fixture(const char* name) {
    return fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / name;
}

static QEDatabase loaded_db() {
    QEDatabase db;
    auto r = db.load_shipped(fixture("minimal_db.json").string());
    REQUIRE(r.ok);
    return db;
}

TEST_CASE("ChannelDecomposer: Q matrix HaO3 + ASI585MC has expected shape", "[decomposer]") {
    QEDatabase db = loaded_db();
    ChannelDecomposer d(db);
    auto Q = d.build_q("ASI585MC", "HaO3");
    REQUIRE(Q.rows() == 3); // R, G, B photosites
    REQUIRE(Q.cols() == 2); // Hα, OIII lines
    // Top-left = R photosite QE at Ha (656)
    REQUIRE(Q(0, 0) == Catch::Approx(0.73).margin(0.01));
    // Bottom-right = B photosite QE at OIII (501)
    REQUIRE(Q(2, 1) == Catch::Approx(0.50).margin(0.02));
}

TEST_CASE("ChannelDecomposer: synthetic Hα+OIII recovered to within 1e-3",
          "[decomposer]") {
    QEDatabase db = loaded_db();
    ChannelDecomposer d(db);

    // Inject: Ha=0.5, OIII=0.3 → predict (R, G, B) via Q, then solve back
    auto Q = d.build_q("ASI585MC", "HaO3");
    Eigen::Vector2d truth(0.5, 0.3);
    Eigen::Vector3d rgb = Q * truth;

    Eigen::Vector2d recovered = d.solve("ASI585MC", "HaO3", rgb);
    REQUIRE(recovered(0) == Catch::Approx(0.5).margin(1e-6));
    REQUIRE(recovered(1) == Catch::Approx(0.3).margin(1e-6));
}

TEST_CASE("ChannelDecomposer: S2O3 + ASI585MC recovers SII + OIII", "[decomposer]") {
    QEDatabase db = loaded_db();
    ChannelDecomposer d(db);
    auto Q = d.build_q("ASI585MC", "S2O3");
    REQUIRE(Q.rows() == 3);
    REQUIRE(Q.cols() == 2);

    Eigen::Vector2d truth(0.4, 0.2);
    Eigen::Vector3d rgb = Q * truth;
    Eigen::Vector2d recovered = d.solve("ASI585MC", "S2O3", rgb);
    REQUIRE(recovered(0) == Catch::Approx(0.4).margin(1e-6));
    REQUIRE(recovered(1) == Catch::Approx(0.2).margin(1e-6));
}

TEST_CASE("ChannelDecomposer: multi-camera lookup picks correct Q", "[decomposer]") {
    QEDatabase db = loaded_db();
    ChannelDecomposer d(db);

    auto Q585  = d.build_q("ASI585MC",  "HaO3");
    auto Q2600 = d.build_q("ASI2600MC", "HaO3");

    // R-photosite QE at Hα should differ between the two sensors
    REQUIRE(Q585(0, 0)  == Catch::Approx(0.73).margin(0.01));
    REQUIRE(Q2600(0, 0) == Catch::Approx(0.46).margin(0.01));
    REQUIRE(Q585(0, 0) != Catch::Approx(Q2600(0, 0)).margin(0.05));
}

TEST_CASE("ChannelDecomposer: singular Q throws SingularQError", "[decomposer]") {
    // Synthesize an override DB where R, G, B all have identical QE at both lines:
    QEDatabase db = loaded_db();
    // Manually add a degenerate override
    auto path = fs::temp_directory_path() / "qe_singular_override.json";
    {
        std::ofstream f(path);
        f << R"({
          "schema_version": 1,
          "cameras": {
            "Degenerate": {
              "sensor": "Fake", "type": "OSC", "bayer": "RGGB",
              "qe": {
                "501": { "R": 0.5, "G": 0.5, "B": 0.5 },
                "656": { "R": 0.5, "G": 0.5, "B": 0.5 }
              },
              "confidence": "low"
            }
          }
        })";
    }
    REQUIRE(db.load_override(path.string()).ok);
    ChannelDecomposer d(db);
    REQUIRE_THROWS_AS(d.build_q("Degenerate", "HaO3"), SingularQError);
}

TEST_CASE("ChannelDecomposer: unknown camera throws", "[decomposer]") {
    QEDatabase db = loaded_db();
    ChannelDecomposer d(db);
    REQUIRE_THROWS_AS(d.build_q("DoesNotExist", "HaO3"), UnknownCameraError);
}

TEST_CASE("ChannelDecomposer: unknown filter throws", "[decomposer]") {
    QEDatabase db = loaded_db();
    ChannelDecomposer d(db);
    REQUIRE_THROWS_AS(d.build_q("ASI585MC", "DoesNotExist"), UnknownFilterError);
}
```

Add registration in `test/unit/calibration/CMakeLists.txt`:

```cmake
nukex_add_test(test_channel_decomposer test_channel_decomposer.cpp nukex4_calibration nukex4_io nukex4_core)
```

- [ ] **Step 3: Verify build fails (header missing)**

Run: `cd build && cmake .. && make test_channel_decomposer 2>&1 | tail -10`
Expected: build error "channel_decomposer.hpp: No such file"

- [ ] **Step 4: Implement the header**

Create `src/lib/calibration/include/nukex/calibration/channel_decomposer.hpp`:

```cpp
#ifndef NUKEX_CALIBRATION_CHANNEL_DECOMPOSER_HPP
#define NUKEX_CALIBRATION_CHANNEL_DECOMPOSER_HPP

#include <Eigen/Dense>

#include <stdexcept>
#include <string>

namespace nukex {

class QEDatabase;

class UnknownCameraError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class UnknownFilterError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SingularQError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ChannelDecomposer {
public:
    explicit ChannelDecomposer(const QEDatabase& db) : db_(db) {}

    // Build the 3×N Q matrix for the (camera, filter) pair.
    // For dual-NB: 3×2; for broadband-OSC: 3×3 (identity-like, mostly diagonal).
    Eigen::MatrixXd build_q(const std::string& camera,
                            const std::string& filter_name) const;

    // Solve (R, G, B) → (line1, line2, ...) via least-squares.
    // Throws if Q matrix is singular.
    Eigen::VectorXd solve(const std::string&     camera,
                          const std::string&     filter_name,
                          const Eigen::Vector3d& rgb) const;

private:
    const QEDatabase& db_;
};

} // namespace nukex

#endif
```

- [ ] **Step 5: Implement the .cpp**

Create `src/lib/calibration/src/channel_decomposer.cpp`:

```cpp
#include "nukex/calibration/channel_decomposer.hpp"
#include "nukex/calibration/qe_database.hpp"

namespace nukex {

Eigen::MatrixXd ChannelDecomposer::build_q(const std::string& camera,
                                           const std::string& filter_name) const {
    if (!db_.has_camera(camera)) {
        throw UnknownCameraError("Camera not in QE DB: " + camera);
    }
    if (!db_.has_filter(filter_name)) {
        throw UnknownFilterError("Filter not in QE DB: " + filter_name);
    }
    auto fp = db_.lookup_filter(filter_name);
    if (fp.lines.empty()) {
        throw UnknownFilterError("Filter " + filter_name + " has no emission lines defined");
    }

    const int n_lines = static_cast<int>(fp.lines.size());
    Eigen::MatrixXd Q(3, n_lines);
    for (int j = 0; j < n_lines; ++j) {
        const double wl = fp.lines[j].wavelength_nm;
        Q(0, j) = db_.lookup_camera_qe(camera, wl, Photosite::R);
        Q(1, j) = db_.lookup_camera_qe(camera, wl, Photosite::G);
        Q(2, j) = db_.lookup_camera_qe(camera, wl, Photosite::B);
    }

    // Singularity check via rank
    Eigen::FullPivLU<Eigen::MatrixXd> lu(Q);
    if (lu.rank() < n_lines) {
        throw SingularQError("Q matrix for (" + camera + ", " + filter_name +
                             ") is singular — QE values must form a non-degenerate basis. " +
                             "Filter QE in DB is suspect. Report bug + check override.");
    }

    return Q;
}

Eigen::VectorXd ChannelDecomposer::solve(const std::string&     camera,
                                         const std::string&     filter_name,
                                         const Eigen::Vector3d& rgb) const {
    Eigen::MatrixXd Q = build_q(camera, filter_name);
    return Q.colPivHouseholderQr().solve(rgb);
}

} // namespace nukex
```

- [ ] **Step 6: Run tests to verify pass**

Run: `cd build && cmake .. && make test_channel_decomposer && ctest -R test_channel_decomposer --output-on-failure`
Expected: PASS, all 7 test cases green.

- [ ] **Step 7: Commit**

```bash
git add src/lib/calibration/include/nukex/calibration/channel_decomposer.hpp \
        src/lib/calibration/src/channel_decomposer.cpp \
        src/lib/calibration/CMakeLists.txt \
        test/unit/calibration/test_channel_decomposer.cpp \
        test/unit/calibration/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(calibration): ChannelDecomposer Q-matrix solve via Eigen QR

Builds 3×N Q matrix from QEDatabase per (camera, filter) and solves
(R, G, B) → emission lines via colPivHouseholderQr (numerically
stable, adequate for 3×2/3×3). Singular Q throws SingularQError;
unknown camera/filter throw distinct exceptions for caller-side
loud-fail. Eigen 3.4 vendored via FetchContent (header-only). Round-
trips synthetic Hα+OIII to within 1e-6.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: `Palette` — calibrated emission-line CIE-Lab vectors

**Files:**
- Create: `src/lib/compose/CMakeLists.txt`
- Create: `src/lib/compose/include/nukex/compose/palette.hpp`
- Create: `src/lib/compose/src/palette.cpp`
- Test: `test/unit/compose/CMakeLists.txt` + `test/unit/compose/test_palette.cpp`
- Modify: `test/CMakeLists.txt` (register the new compose subdir)

Initial palette vectors (per spec §5.3, tunable after real-data validation):

| Line | a* | b* | Visual |
|---|---|---|---|
| Hα 656nm | +50 | +10 | red (slightly warm) |
| OIII 501nm | -15 | -35 | cyan-blue |
| SII 672nm | +60 | +25 | deep red / red-orange |

**Property invariant:** No vector lives in the Lab green quadrant (`a* < 0 AND b* > 0`).

- [ ] **Step 1: Create the compose lib CMakeLists.txt**

Create `src/lib/compose/CMakeLists.txt`:

```cmake
add_library(nukex4_compose STATIC
    src/palette.cpp
)

target_include_directories(nukex4_compose
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_compose
    PUBLIC nukex4_core nukex4_io
)

target_compile_features(nukex4_compose PUBLIC cxx_std_17)
```

- [ ] **Step 2: Create the test subdir CMakeLists.txt and add to test root**

Create `test/unit/compose/CMakeLists.txt`:

```cmake
nukex_add_test(test_palette test_palette.cpp nukex4_compose nukex4_core)
```

Edit `test/CMakeLists.txt` and add `add_subdirectory(unit/compose)` (alphabetical with the other `unit/X` subdirs, which means after `unit/combine` and before `unit/core`).

- [ ] **Step 3: Write the failing tests**

Create `test/unit/compose/test_palette.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/compose/palette.hpp"

using namespace nukex;

TEST_CASE("Palette: Ha vector points to warm red", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::Ha);
    REQUIRE(v.a == Catch::Approx(+50.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(+10.0).margin(0.5));
}

TEST_CASE("Palette: OIII vector points to cyan-blue", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::OIII);
    REQUIRE(v.a == Catch::Approx(-15.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(-35.0).margin(0.5));
}

TEST_CASE("Palette: SII vector points to deep red-orange", "[palette]") {
    LabColor v = Palette::for_line(EmissionLineId::SII);
    REQUIRE(v.a == Catch::Approx(+60.0).margin(0.5));
    REQUIRE(v.b == Catch::Approx(+25.0).margin(0.5));
}

TEST_CASE("Palette property: no vector in green quadrant (-a*, +b*)",
          "[palette][property]") {
    for (auto line : {EmissionLineId::Ha, EmissionLineId::OIII, EmissionLineId::SII}) {
        LabColor v = Palette::for_line(line);
        const bool in_green_quadrant = (v.a < 0.0 && v.b > 0.0);
        INFO("Line " << static_cast<int>(line) << " has (a=" << v.a << ", b=" << v.b << ")");
        REQUIRE_FALSE(in_green_quadrant);
    }
}

TEST_CASE("Palette property: every vector has nonzero chroma", "[palette][property]") {
    for (auto line : {EmissionLineId::Ha, EmissionLineId::OIII, EmissionLineId::SII}) {
        LabColor v = Palette::for_line(line);
        const double chroma = std::sqrt(v.a * v.a + v.b * v.b);
        INFO("Line " << static_cast<int>(line) << " chroma = " << chroma);
        REQUIRE(chroma > 10.0); // any meaningful palette vector should have at least this much
    }
}
```

- [ ] **Step 4: Verify build fails**

Run: `cd build && cmake .. && make test_palette 2>&1 | tail -10`
Expected: build error "palette.hpp: No such file"

- [ ] **Step 5: Implement the header**

Create `src/lib/compose/include/nukex/compose/palette.hpp`:

```cpp
#ifndef NUKEX_COMPOSE_PALETTE_HPP
#define NUKEX_COMPOSE_PALETTE_HPP

namespace nukex {

enum class EmissionLineId { Ha, OIII, SII };

struct LabColor {
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

class Palette {
public:
    // Calibrated chrominance vector for the line. L is left at 0;
    // luminance comes from broadband data downstream in ColorComposer.
    static LabColor for_line(EmissionLineId line);
};

} // namespace nukex

#endif
```

- [ ] **Step 6: Implement the .cpp**

Create `src/lib/compose/src/palette.cpp`:

```cpp
#include "nukex/compose/palette.hpp"

namespace nukex {

LabColor Palette::for_line(EmissionLineId line) {
    switch (line) {
        case EmissionLineId::Ha:   return {0.0, +50.0, +10.0};
        case EmissionLineId::OIII: return {0.0, -15.0, -35.0};
        case EmissionLineId::SII:  return {0.0, +60.0, +25.0};
    }
    return {};
}

} // namespace nukex
```

- [ ] **Step 7: Run tests to verify pass**

Run: `cd build && cmake .. && make test_palette && ctest -R test_palette --output-on-failure`
Expected: PASS, 5 test cases green.

- [ ] **Step 8: Commit**

```bash
git add src/lib/compose/CMakeLists.txt \
        src/lib/compose/include/nukex/compose/palette.hpp \
        src/lib/compose/src/palette.cpp \
        test/unit/compose/CMakeLists.txt \
        test/unit/compose/test_palette.cpp \
        test/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(compose): calibrated emission-line palette with no-greens invariant

Initial Lab vectors for Hα (warm red), OIII (cyan-blue), SII (deep
red-orange). Property test enforces no vector lives in the green
Lab quadrant (a*<0 AND b*>0) — this is the structural fix for the
M27-green dual-narrowband bug, replacing post-hoc SCNR. Vectors are
tunable after real-data validation per spec §8.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: `ColorComposer` — Lab/LCH composite + continuum-subtract opt-in

**Files:**
- Modify: `src/lib/compose/CMakeLists.txt` (add new source)
- Create: `src/lib/compose/include/nukex/compose/color_composer.hpp`
- Create: `src/lib/compose/src/color_composer.cpp`
- Test: `test/unit/compose/test_color_composer.cpp`
- Modify: `test/unit/compose/CMakeLists.txt`

The composer takes a `DerivedSlots` struct (semantic line + broadband channels per pixel) and emits sRGB. Two modes:

1. **Lab/LCH default** — `L_broadband` from native L (or rec709-synthesized from R+G+B for OSC-only batches); chrominance is the sum of natural `(a*, b*)` and weighted emission palette vectors; soft-clip on gamut.

2. **Continuum-subtract opt-in** — subtract `k_X * broadband` from each line BEFORE the chrominance sum, so stars stay natural and only line-emitting regions get the chroma boost.

Color-space chain: sRGB → linear → XYZ (D65) → Lab → manipulate → Lab → XYZ → linear sRGB → sRGB. Standard ITU-R BT.709 primaries, D65 white point.

- [ ] **Step 1: Add channel_classifier source to compose CMakeLists.txt**

Edit `src/lib/compose/CMakeLists.txt`:

```cmake
add_library(nukex4_compose STATIC
    src/palette.cpp
    src/color_composer.cpp
)
```

- [ ] **Step 2: Write the failing tests**

Create `test/unit/compose/test_color_composer.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/compose/color_composer.hpp"

#include <cmath>

using namespace nukex;

static DerivedSlots zeroed() {
    DerivedSlots s;
    s.L = 0; s.R = 0; s.G = 0; s.B = 0;
    s.Ha = 0; s.OIII = 0; s.SII = 0;
    return s;
}

TEST_CASE("ColorComposer: pure broadband mid-gray maps to mid-gray sRGB",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.5; pix.G = 0.5; pix.B = 0.5;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.b == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("ColorComposer: pure Hα signal (no broadband) drives red",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5; // mid luminance
    pix.Ha = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r > out.g);
    REQUIRE(out.r > out.b);
}

TEST_CASE("ColorComposer: pure OIII signal (no broadband) drives cyan-blue",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5;
    pix.OIII = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.b > out.r);
    REQUIRE(out.g > out.r); // cyan = G+B > R
}

TEST_CASE("ColorComposer: pure SII signal (no broadband) drives deep red",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 0.5;
    pix.SII = 0.8;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r > out.g);
    REQUIRE(out.r > out.b);
}

TEST_CASE("ColorComposer: gamut soft-clip preserves hue, increments counter",
          "[color_composer]") {
    ColorComposer c;
    auto pix = zeroed();
    pix.L = 1.0;
    pix.Ha = 5.0;   // huge over-saturation
    pix.OIII = 0.0;
    pix.SII = 0.0;

    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r >= 0.0); REQUIRE(out.r <= 1.0);
    REQUIRE(out.g >= 0.0); REQUIRE(out.g <= 1.0);
    REQUIRE(out.b >= 0.0); REQUIRE(out.b <= 1.0);
    REQUIRE(c.gamut_clipped_count() >= 1);

    // Hue preserved: red dominant
    REQUIRE(out.r >= out.g);
    REQUIRE(out.r >= out.b);
}

TEST_CASE("ColorComposer continuum-subtract: pure broadband stays unchanged",
          "[color_composer][continuum]") {
    ColorComposer c;
    c.set_mode(ColorComposer::Mode::CONTINUUM_SUBTRACT);
    c.set_continuum_coefficients({0.1, 0.1, 0.1});

    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.5; pix.G = 0.5; pix.B = 0.5;
    sRGBPixel out = c.compose_pixel(pix);
    REQUIRE(out.r == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.g == Catch::Approx(0.5).margin(0.01));
    REQUIRE(out.b == Catch::Approx(0.5).margin(0.01));
}

TEST_CASE("ColorComposer continuum-subtract: Hα with continuum is suppressed",
          "[color_composer][continuum]") {
    ColorComposer c;
    c.set_mode(ColorComposer::Mode::CONTINUUM_SUBTRACT);
    c.set_continuum_coefficients({1.0, 0.0, 0.0}); // k_Ha = 1

    // Pixel with continuum Ha + matching broadband R → subtraction zeroes Ha
    auto pix = zeroed();
    pix.L = 0.5; pix.R = 0.3; pix.G = 0; pix.B = 0;
    pix.Ha = 0.3; // exactly matches continuum

    sRGBPixel out = c.compose_pixel(pix);
    // Should look like a pure broadband pixel with no Hα chroma boost
    REQUIRE(c.last_pixel_emission_a() == Catch::Approx(0.0).margin(0.5));
}

TEST_CASE("ColorComposer: zero signal everywhere → black", "[color_composer]") {
    ColorComposer c;
    sRGBPixel out = c.compose_pixel(zeroed());
    REQUIRE(out.r == Catch::Approx(0.0).margin(0.001));
    REQUIRE(out.g == Catch::Approx(0.0).margin(0.001));
    REQUIRE(out.b == Catch::Approx(0.0).margin(0.001));
}
```

Edit `test/unit/compose/CMakeLists.txt`:

```cmake
nukex_add_test(test_palette test_palette.cpp nukex4_compose nukex4_core)
nukex_add_test(test_color_composer test_color_composer.cpp nukex4_compose nukex4_core)
```

- [ ] **Step 3: Verify build fails**

Run: `cd build && cmake .. && make test_color_composer 2>&1 | tail -10`
Expected: build error "color_composer.hpp: No such file"

- [ ] **Step 4: Implement the header**

Create `src/lib/compose/include/nukex/compose/color_composer.hpp`:

```cpp
#ifndef NUKEX_COMPOSE_COLOR_COMPOSER_HPP
#define NUKEX_COMPOSE_COLOR_COMPOSER_HPP

#include "nukex/compose/palette.hpp"

#include <cstdint>

namespace nukex {

struct DerivedSlots {
    // Broadband channels (any may be zero if not in batch)
    double L = 0.0;
    double R = 0.0;
    double G = 0.0;
    double B = 0.0;
    // Emission-line channels (any may be zero if not in batch)
    double Ha   = 0.0;
    double OIII = 0.0;
    double SII  = 0.0;
};

struct sRGBPixel {
    double r = 0.0, g = 0.0, b = 0.0;
};

struct ContinuumK {
    double k_Ha   = 0.0;
    double k_OIII = 0.0;
    double k_SII  = 0.0;
};

class ColorComposer {
public:
    enum class Mode { LAB_LCH_DEFAULT, CONTINUUM_SUBTRACT };

    ColorComposer() = default;

    void set_mode(Mode m)                              { mode_ = m; }
    void set_continuum_coefficients(const ContinuumK& k) { continuum_ = k; }

    sRGBPixel compose_pixel(const DerivedSlots& s);

    // Stats
    std::int64_t gamut_clipped_count() const { return gamut_clipped_; }

    // Diagnostics for tests
    double last_pixel_emission_a() const { return last_emission_a_; }
    double last_pixel_emission_b() const { return last_emission_b_; }

private:
    Mode         mode_      = Mode::LAB_LCH_DEFAULT;
    ContinuumK   continuum_ = {};
    std::int64_t gamut_clipped_ = 0;
    double       last_emission_a_ = 0.0;
    double       last_emission_b_ = 0.0;

    static LabColor    rgb_to_lab(double r, double g, double b);
    static sRGBPixel   lab_to_srgb(const LabColor& lab);
    static double      signal_weight(double v);
    bool               clip_to_gamut(double& r, double& g, double& b);
};

} // namespace nukex

#endif
```

- [ ] **Step 5: Implement the .cpp**

Create `src/lib/compose/src/color_composer.cpp`:

```cpp
#include "nukex/compose/color_composer.hpp"

#include <algorithm>
#include <cmath>

namespace nukex {

namespace {

// sRGB transfer functions (IEC 61966-2-1)
double srgb_to_linear(double v) {
    return (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double linear_to_srgb(double v) {
    return (v <= 0.0031308) ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// D65 white point in XYZ
constexpr double Xn = 0.95047, Yn = 1.0, Zn = 1.08883;

// CIE Lab f / f^-1
double f_lab(double t) {
    return (t > 0.008856) ? std::cbrt(t) : (7.787 * t + 16.0 / 116.0);
}
double f_lab_inv(double t) {
    double t3 = t * t * t;
    return (t3 > 0.008856) ? t3 : (t - 16.0 / 116.0) / 7.787;
}

void srgb_to_xyz(double r, double g, double b, double& X, double& Y, double& Z) {
    double rl = srgb_to_linear(r), gl = srgb_to_linear(g), bl = srgb_to_linear(b);
    X = 0.4124564 * rl + 0.3575761 * gl + 0.1804375 * bl;
    Y = 0.2126729 * rl + 0.7151522 * gl + 0.0721750 * bl;
    Z = 0.0193339 * rl + 0.1191920 * gl + 0.9503041 * bl;
}
void xyz_to_srgb(double X, double Y, double Z, double& r, double& g, double& b) {
    double rl =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double gl = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double bl =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    r = linear_to_srgb(std::max(0.0, rl));
    g = linear_to_srgb(std::max(0.0, gl));
    b = linear_to_srgb(std::max(0.0, bl));
}

} // namespace

LabColor ColorComposer::rgb_to_lab(double r, double g, double b) {
    double X, Y, Z;
    srgb_to_xyz(r, g, b, X, Y, Z);
    double fx = f_lab(X / Xn), fy = f_lab(Y / Yn), fz = f_lab(Z / Zn);
    return { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

sRGBPixel ColorComposer::lab_to_srgb(const LabColor& lab) {
    double fy = (lab.L + 16.0) / 116.0;
    double fx = fy + lab.a / 500.0;
    double fz = fy - lab.b / 200.0;
    double X = Xn * f_lab_inv(fx);
    double Y = Yn * f_lab_inv(fy);
    double Z = Zn * f_lab_inv(fz);
    sRGBPixel out;
    xyz_to_srgb(X, Y, Z, out.r, out.g, out.b);
    return out;
}

double ColorComposer::signal_weight(double v) {
    return std::max(0.0, v); // simple linear weight; clamp negatives
}

bool ColorComposer::clip_to_gamut(double& r, double& g, double& b) {
    bool clipped = false;
    auto clamp = [&clipped](double& x) {
        if (x < 0.0) { x = 0.0; clipped = true; }
        else if (x > 1.0) { x = 1.0; clipped = true; }
    };
    clamp(r); clamp(g); clamp(b);
    return clipped;
}

sRGBPixel ColorComposer::compose_pixel(const DerivedSlots& s) {
    // 1) L_broadband: native L, or rec709 from RGB if no L
    double L_broad = (s.L > 0.0) ? s.L
                                 : (0.299 * s.R + 0.587 * s.G + 0.114 * s.B);

    // 2) Continuum subtract (opt-in)
    double ha = s.Ha, oiii = s.OIII, sii = s.SII;
    if (mode_ == Mode::CONTINUUM_SUBTRACT) {
        ha   -= continuum_.k_Ha   * s.R;
        oiii -= continuum_.k_OIII * (s.G + s.B) * 0.5;
        sii  -= continuum_.k_SII  * s.R;
    }

    // 3) Natural Lab from broadband (if any)
    LabColor lab_natural{ L_broad * 100.0, 0.0, 0.0 };
    bool have_broadband = (s.R + s.G + s.B) > 0.0;
    if (have_broadband) {
        lab_natural = rgb_to_lab(s.R, s.G, s.B);
        // Replace L with broadband-derived L
        lab_natural.L = L_broad * 100.0;
    }

    // 4) Emission contribution to chrominance
    double w_ha   = signal_weight(ha);
    double w_oiii = signal_weight(oiii);
    double w_sii  = signal_weight(sii);
    double total_w = w_ha + w_oiii + w_sii;

    LabColor pal_ha   = Palette::for_line(EmissionLineId::Ha);
    LabColor pal_oiii = Palette::for_line(EmissionLineId::OIII);
    LabColor pal_sii  = Palette::for_line(EmissionLineId::SII);

    double emission_a = 0.0, emission_b = 0.0;
    if (total_w > 0.0) {
        emission_a = (w_ha * pal_ha.a + w_oiii * pal_oiii.a + w_sii * pal_sii.a);
        emission_b = (w_ha * pal_ha.b + w_oiii * pal_oiii.b + w_sii * pal_sii.b);
    }
    last_emission_a_ = emission_a;
    last_emission_b_ = emission_b;

    LabColor lab_final{
        lab_natural.L,
        lab_natural.a + emission_a,
        lab_natural.b + emission_b
    };

    // 5) Convert to sRGB and soft-clip
    sRGBPixel out = lab_to_srgb(lab_final);
    if (clip_to_gamut(out.r, out.g, out.b)) {
        ++gamut_clipped_;
    }
    return out;
}

} // namespace nukex
```

- [ ] **Step 6: Run tests to verify pass**

Run: `cd build && cmake .. && make test_color_composer && ctest -R test_color_composer --output-on-failure`
Expected: PASS, all 8 test cases green.

- [ ] **Step 7: Commit**

```bash
git add src/lib/compose/include/nukex/compose/color_composer.hpp \
        src/lib/compose/src/color_composer.cpp \
        src/lib/compose/CMakeLists.txt \
        test/unit/compose/test_color_composer.cpp \
        test/unit/compose/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(compose): ColorComposer Lab/LCH default + continuum-subtract opt-in

Composes derived semantic slots (Ha, OIII, SII, L, R, G, B) into
3-channel sRGB. Default mode: broadband-derived L + natural chrominance
+ emission-weighted palette vector contribution. Continuum-subtract
opt-in mode: subtracts estimated continuum from each emission line
before chrominance contribution, leaving stars natural and boosting
only line-emitting regions. Standard sRGB↔Lab via D65 white point;
soft-clip to gamut with per-pixel counter.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 2: Cube / Voxel Adaptation

The voxel keeps its fixed `MAX_CHANNELS=8` arrays (per the Explore agent's recommendation; SoA-friendly memory layout for GPU stays intact). What we add is a **named-slot lookup table** at the `ChannelConfig` level so callers can write `voxel.welford[cfg.slot_index("R_HaO3")]` without losing typed clarity.

### Task 7: `ChannelConfig` — name-indexed slot lookup + `from_filter()` factory

**Files:**
- Modify: `src/lib/core/include/nukex/core/channel_config.hpp`
- Modify: `src/lib/core/src/channel_config.cpp`
- Test: `test/unit/core/test_channel_config_from_filter.cpp` (new file)
- Modify: `test/unit/core/CMakeLists.txt`

This task adds the new factory and slot-name machinery. The dead `OSC_HAO3`/`OSC_S2O3` switch cases stay in place for now — they get removed in Task 17 (cleanup). Adding alongside keeps existing `from_mode()` callers green during the refactor.

- [ ] **Step 1: Write the failing tests**

Create `test/unit/core/test_channel_config_from_filter.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/io/filter.hpp"

using namespace nukex;

TEST_CASE("ChannelConfig::from_filter BROADBAND_OSC allocates 4 slots", "[channel_config]") {
    Filter f{ FilterClass::BROADBAND_OSC, "OSC", "ASI585MC", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 4);
    REQUIRE(cfg.slot_index("R") == 0);
    REQUIRE(cfg.slot_index("G") == 1);
    REQUIRE(cfg.slot_index("B") == 2);
    REQUIRE(cfg.slot_index("L") == 3);
    REQUIRE(cfg.bayer != BayerPattern::NONE);
}

TEST_CASE("ChannelConfig::from_filter BROADBAND_L allocates 1 slot", "[channel_config]") {
    Filter f{ FilterClass::BROADBAND_L, "L", "ASI2600MM", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 1);
    REQUIRE(cfg.slot_index("L") == 0);
    REQUIRE(cfg.bayer == BayerPattern::NONE);
}

TEST_CASE("ChannelConfig::from_filter DUAL_NB_OSC HaO3 allocates 3 slots",
          "[channel_config]") {
    Filter f{ FilterClass::DUAL_NB_OSC, "HaO3", "ASI585MC", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 3);
    REQUIRE(cfg.slot_index("R_HaO3") == 0);
    REQUIRE(cfg.slot_index("G_HaO3") == 1);
    REQUIRE(cfg.slot_index("B_HaO3") == 2);
}

TEST_CASE("ChannelConfig::from_filter DUAL_NB_OSC S2O3 allocates 3 slots",
          "[channel_config]") {
    Filter f{ FilterClass::DUAL_NB_OSC, "S2O3", "ASI585MC", {} };
    auto cfg = ChannelConfig::from_filter(f);
    REQUIRE(cfg.n_channels == 3);
    REQUIRE(cfg.slot_index("R_S2O3") == 0);
    REQUIRE(cfg.slot_index("G_S2O3") == 1);
    REQUIRE(cfg.slot_index("B_S2O3") == 2);
}

TEST_CASE("ChannelConfig::from_filter NARROWBAND_SINGLE allocates 1 slot per line",
          "[channel_config]") {
    Filter f_ha{ FilterClass::NARROWBAND_SINGLE, "Ha", "ASI2600MM", {} };
    auto cfg_ha = ChannelConfig::from_filter(f_ha);
    REQUIRE(cfg_ha.n_channels == 1);
    REQUIRE(cfg_ha.slot_index("Ha") == 0);

    Filter f_oiii{ FilterClass::NARROWBAND_SINGLE, "OIII", "ASI2600MM", {} };
    auto cfg_oiii = ChannelConfig::from_filter(f_oiii);
    REQUIRE(cfg_oiii.slot_index("OIII") == 0);

    Filter f_sii{ FilterClass::NARROWBAND_SINGLE, "SII", "ASI2600MM", {} };
    auto cfg_sii = ChannelConfig::from_filter(f_sii);
    REQUIRE(cfg_sii.slot_index("SII") == 0);
}

TEST_CASE("ChannelConfig::from_filter BROADBAND_RGB names map to channel index",
          "[channel_config]") {
    auto cfg_r = ChannelConfig::from_filter({FilterClass::BROADBAND_RGB, "R", "ASI2600MM", {}});
    REQUIRE(cfg_r.n_channels == 1);
    REQUIRE(cfg_r.slot_index("R") == 0);

    auto cfg_g = ChannelConfig::from_filter({FilterClass::BROADBAND_RGB, "G", "ASI2600MM", {}});
    REQUIRE(cfg_g.slot_index("G") == 0);

    auto cfg_b = ChannelConfig::from_filter({FilterClass::BROADBAND_RGB, "B", "ASI2600MM", {}});
    REQUIRE(cfg_b.slot_index("B") == 0);
}

TEST_CASE("ChannelConfig::merge unions slots from two configs", "[channel_config]") {
    auto cfg1 = ChannelConfig::from_filter({FilterClass::BROADBAND_L, "L", "ASI2600MM", {}});
    auto cfg2 = ChannelConfig::from_filter({FilterClass::DUAL_NB_OSC, "HaO3", "ASI585MC", {}});
    auto merged = ChannelConfig::merge(cfg1, cfg2);
    REQUIRE(merged.n_channels == 4); // L + R_HaO3 + G_HaO3 + B_HaO3
    REQUIRE(merged.slot_index("L")      != -1);
    REQUIRE(merged.slot_index("R_HaO3") != -1);
    REQUIRE(merged.slot_index("G_HaO3") != -1);
    REQUIRE(merged.slot_index("B_HaO3") != -1);
}

TEST_CASE("ChannelConfig::slot_index returns -1 for unknown name", "[channel_config]") {
    auto cfg = ChannelConfig::from_filter({FilterClass::BROADBAND_L, "L", "ASI2600MM", {}});
    REQUIRE(cfg.slot_index("DoesNotExist") == -1);
}
```

Edit `test/unit/core/CMakeLists.txt` to register:

```cmake
nukex_add_test(test_channel_config_from_filter test_channel_config_from_filter.cpp nukex4_core nukex4_io)
```

- [ ] **Step 2: Verify build fails**

Run: `cd build && cmake .. && make test_channel_config_from_filter 2>&1 | tail -10`
Expected: build error "no member named 'from_filter' in struct ChannelConfig"

- [ ] **Step 3: Add `from_filter` + `slot_index` + `merge` to header**

Edit `src/lib/core/include/nukex/core/channel_config.hpp`. Add at the bottom of the file (before the closing `}` of `namespace nukex`), and modify the `ChannelConfig` struct to include the new methods:

```cpp
#include "nukex/io/filter.hpp"
// ... (existing includes stay)

struct ChannelConfig {
    StackingMode mode          = StackingMode::MONO_L;
    uint8_t      n_channels    = 1;
    std::string  channel_names[MAX_CHANNELS];
    uint8_t      output_rgb_mapping[3] = {0, 0, 0};   // (removed in Task 17)
    BayerPattern bayer         = BayerPattern::NONE;

    static ChannelConfig from_mode(StackingMode mode);
    static ChannelConfig from_filter(const Filter& f);
    static ChannelConfig merge(const ChannelConfig& a, const ChannelConfig& b);

    int channel_index_for_name(const std::string& name) const;
    int slot_index(const std::string& name) const; // == channel_index_for_name; legible alias
    bool is_mono() const;
};
```

- [ ] **Step 4: Implement `from_filter`, `merge`, and `slot_index` alias**

Edit `src/lib/core/src/channel_config.cpp` and add at the bottom:

```cpp
#include "nukex/io/filter.hpp"

namespace nukex {

ChannelConfig ChannelConfig::from_filter(const Filter& f) {
    ChannelConfig cfg;
    switch (f.cls) {
        case FilterClass::BROADBAND_L:
            cfg.n_channels = 1;
            cfg.channel_names[0] = "L";
            cfg.bayer = BayerPattern::NONE;
            break;

        case FilterClass::BROADBAND_RGB:
            cfg.n_channels = 1;
            cfg.channel_names[0] = f.name; // "R", "G", or "B"
            cfg.bayer = BayerPattern::NONE;
            break;

        case FilterClass::BROADBAND_OSC:
            cfg.n_channels = 4;
            cfg.channel_names[0] = "R";
            cfg.channel_names[1] = "G";
            cfg.channel_names[2] = "B";
            cfg.channel_names[3] = "L"; // synthesized rec709 L per-frame
            cfg.bayer = BayerPattern::RGGB; // overridden by stacking_engine if header differs
            break;

        case FilterClass::NARROWBAND_SINGLE:
            cfg.n_channels = 1;
            cfg.channel_names[0] = f.name; // "Ha", "OIII", or "SII"
            cfg.bayer = BayerPattern::NONE;
            break;

        case FilterClass::DUAL_NB_OSC:
            cfg.n_channels = 3;
            cfg.channel_names[0] = "R_" + f.name;
            cfg.channel_names[1] = "G_" + f.name;
            cfg.channel_names[2] = "B_" + f.name;
            cfg.bayer = BayerPattern::RGGB; // overridden by stacking_engine if header differs
            break;

        case FilterClass::UNKNOWN:
            // Caller is expected to fail-loud upstream; default to MONO_L for safety
            cfg.n_channels = 1;
            cfg.channel_names[0] = "L";
            cfg.bayer = BayerPattern::NONE;
            break;
    }
    return cfg;
}

ChannelConfig ChannelConfig::merge(const ChannelConfig& a, const ChannelConfig& b) {
    ChannelConfig out;
    int idx = 0;
    auto append_unique = [&](const std::string& name) {
        if (name.empty()) return;
        for (int i = 0; i < idx; ++i) {
            if (out.channel_names[i] == name) return;
        }
        if (idx < MAX_CHANNELS) {
            out.channel_names[idx++] = name;
        }
    };
    for (int i = 0; i < a.n_channels; ++i) append_unique(a.channel_names[i]);
    for (int i = 0; i < b.n_channels; ++i) append_unique(b.channel_names[i]);
    out.n_channels = static_cast<uint8_t>(idx);
    out.bayer = (a.bayer != BayerPattern::NONE) ? a.bayer : b.bayer;
    return out;
}

int ChannelConfig::slot_index(const std::string& name) const {
    return channel_index_for_name(name);
}

} // namespace nukex
```

- [ ] **Step 5: Run tests to verify pass**

Run: `cd build && cmake .. && make test_channel_config_from_filter && ctest -R test_channel_config_from_filter --output-on-failure`
Expected: PASS, all 8 test cases green.

- [ ] **Step 6: Commit**

```bash
git add src/lib/core/include/nukex/core/channel_config.hpp \
        src/lib/core/src/channel_config.cpp \
        test/unit/core/test_channel_config_from_filter.cpp \
        test/unit/core/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(core): ChannelConfig::from_filter + slot_index + merge

Filter-driven channel allocation (5-class taxonomy). slot_index() lets
callers reference voxel arrays by semantic slot name (e.g. "R_HaO3")
instead of brittle integer indices. merge() unions slot lists for
mixed-class batches. Old StackingMode::OSC_HAO3 / OSC_S2O3 switch
cases stay until Task 17 cleanup so existing callers don't break.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 3: Pipeline Refactor

### Task 8: `StackingEngine::Config` — add `qe_database_path` + `qe_override_path`

**Files:**
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`
- Test: `test/unit/stacker/test_engine_config.cpp` (new)
- Modify: `test/unit/stacker/CMakeLists.txt`

This is a tiny task to lock in the Config-shape change before we wire QEDatabase into the engine.

- [ ] **Step 1: Write the failing test**

Create `test/unit/stacker/test_engine_config.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"

using namespace nukex;

TEST_CASE("StackingEngine::Config has QE paths with sensible defaults", "[engine_config]") {
    StackingEngine::Config c;
    REQUIRE(c.cache_dir == "/tmp");
    REQUIRE(c.qe_database_path.find("qe_database.json") != std::string::npos);
    REQUIRE(c.qe_override_path.empty()); // optional, no default
}

TEST_CASE("StackingEngine::Config QE paths are settable", "[engine_config]") {
    StackingEngine::Config c;
    c.qe_database_path = "/tmp/custom_db.json";
    c.qe_override_path = "/tmp/custom_override.json";
    REQUIRE(c.qe_database_path == "/tmp/custom_db.json");
    REQUIRE(c.qe_override_path == "/tmp/custom_override.json");
}
```

Add to `test/unit/stacker/CMakeLists.txt`:

```cmake
nukex_add_test(test_engine_config test_engine_config.cpp nukex4_stacker nukex4_calibration nukex4_io nukex4_core)
```

- [ ] **Step 2: Verify build fails**

Run: `cd build && cmake .. && make test_engine_config 2>&1 | tail -10`
Expected: build error "no member 'qe_database_path' in struct Config"

- [ ] **Step 3: Add the fields**

Edit `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`. Inside `struct Config`, after `cache_dir`:

```cpp
struct Config {
    FrameAligner::Config  aligner_config;
    WeightConfig          weight_config;
    ModelSelector::Config fitting_config;
    std::string           cache_dir = "/tmp";
    std::string           qe_database_path = "share/qe_database.json"; // resolved at startup
    std::string           qe_override_path; // optional; empty = none
    GPUExecutorConfig     gpu_config;
};
```

- [ ] **Step 4: Run tests to verify pass**

Run: `cd build && cmake .. && make test_engine_config && ctest -R test_engine_config --output-on-failure`
Expected: PASS, both test cases green.

- [ ] **Step 5: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/stacking_engine.hpp \
        test/unit/stacker/test_engine_config.cpp \
        test/unit/stacker/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(stacker): add qe_database_path + qe_override_path to engine Config

Threading point for color-science overhaul. Default DB path is
'share/qe_database.json' (resolved relative to module install dir
at startup); override is empty by default.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Phase A Router — replace hardcoded `OSC_RGB` at `stacking_engine.cpp:107`

**Files:**
- Modify: `src/lib/stacker/CMakeLists.txt` (link calibration + io libs)
- Modify: `src/lib/stacker/src/stacking_engine.cpp`
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` (private member: `FilterClassifier`, `QEDatabase`)
- Test: `test/integration/CMakeLists.txt` + `test/integration/test_phase_a_router.cpp`

This is the central fix. The hardcoded `OSC_RGB` at line 107 is replaced by:
1. Construct `FilterClassifier` and `QEDatabase` from `Config`.
2. Classify the first frame's filter; build initial `ChannelConfig::from_filter()`.
3. As subsequent frames arrive, if their filter differs and is in the same batch, **merge** their config into the cube's.
4. Per-frame Phase A routing dispatches debayered values to the correctly-named slots.
5. `BROADBAND_OSC` frames also synthesize an L slot from `0.299*R + 0.587*G + 0.114*B`.
6. `UNKNOWN` filter on Bayer = batch rejected upstream (StackingEngine::execute returns failure with explicit message).

- [ ] **Step 1: Wire calibration + io into stacker CMakeLists.txt**

Edit `src/lib/stacker/CMakeLists.txt`. In `target_link_libraries(nukex4_stacker ...)` add `nukex4_calibration nukex4_io nukex4_compose` to the PUBLIC list.

- [ ] **Step 2: Add integration test directory and CMakeLists.txt**

Create `test/integration/CMakeLists.txt`:

```cmake
nukex_add_test(test_phase_a_router test_phase_a_router.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)
target_compile_definitions(test_phase_a_router PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")
```

Add `add_subdirectory(integration)` to `test/CMakeLists.txt`.

- [ ] **Step 3: Write the failing integration test**

Create `test/integration/test_phase_a_router.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/io/filter.hpp"

#include <filesystem>
#include <fstream>

using namespace nukex;
namespace fs = std::filesystem;

// Tests build synthetic single-frame "FITS" via the test harness's MiniFITS writer.
// We assume test/util/mini_fits_writer exists with .write_synthetic(path, w, h, filter, instrument)
// signature. (The util will be added in Task 19; for now this test stays disabled with [.integration].)

TEST_CASE("Phase A: BROADBAND_OSC frame synthesizes L slot",
          "[.integration][phase_a]") {
    // Build a 16x16 synthetic RGGB frame with R=0.5, G=0.5, B=0.5
    auto tmp = fs::temp_directory_path() / "phase_a_osc.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, /*progress*/nullptr);

    REQUIRE(result.ok);
    // BROADBAND_OSC: 4 slots populated (R, G, B, L)
    REQUIRE(result.cube.channel_config.slot_index("R") != -1);
    REQUIRE(result.cube.channel_config.slot_index("G") != -1);
    REQUIRE(result.cube.channel_config.slot_index("B") != -1);
    REQUIRE(result.cube.channel_config.slot_index("L") != -1);
    // L slot ≈ 0.299*0.5 + 0.587*0.5 + 0.114*0.5 = 0.5
    int  L_idx = result.cube.channel_config.slot_index("L");
    auto px    = result.cube.at(8, 8);
    REQUIRE(px.welford[L_idx].mean == Catch::Approx(0.5f).margin(0.05f));
}

TEST_CASE("Phase A: HaO3 dual-NB frame routes into R_HaO3/G_HaO3/B_HaO3",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_hao3.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube.channel_config.slot_index("R_HaO3") != -1);
    REQUIRE(result.cube.channel_config.slot_index("G_HaO3") != -1);
    REQUIRE(result.cube.channel_config.slot_index("B_HaO3") != -1);
    // No plain "R" slot allocated for pure HaO3 batch
    REQUIRE(result.cube.channel_config.slot_index("R") == -1);
}

TEST_CASE("Phase A: unknown FILTER on Bayer fails the batch loud at start",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_unknown.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "ALP-T-fake-2026", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("ALP-T-fake-2026") != std::string::npos);
    REQUIRE(result.error.find("qe_overrides.json") != std::string::npos);
}

TEST_CASE("Phase A: missing FILTER on Bayer is silent BROADBAND_OSC",
          "[.integration][phase_a]") {
    auto tmp = fs::temp_directory_path() / "phase_a_no_filter.fits";
    test_util::write_synthetic_bayer(tmp.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.cube.channel_config.slot_index("L") != -1); // L synth proves OSC route
}

TEST_CASE("Phase A: mixed L + HaO3 batch builds union slot config",
          "[.integration][phase_a]") {
    auto t1 = fs::temp_directory_path() / "phase_a_l.fits";
    auto t2 = fs::temp_directory_path() / "phase_a_hao3_2.fits";
    test_util::write_synthetic_mono(t1.string(), 16, 16, "ASI2600MM", "L", 0.5f);
    test_util::write_synthetic_bayer(t2.string(), 16, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({t1.string(), t2.string()}, {}, nullptr);

    REQUIRE(result.ok);
    // Union of L + (R/G/B)_HaO3 slots
    REQUIRE(result.cube.channel_config.slot_index("L")      != -1);
    REQUIRE(result.cube.channel_config.slot_index("R_HaO3") != -1);
    REQUIRE(result.cube.channel_config.slot_index("G_HaO3") != -1);
    REQUIRE(result.cube.channel_config.slot_index("B_HaO3") != -1);
}
```

- [ ] **Step 4: Verify build fails (test_util writer doesn't exist yet)**

Run: `cd build && cmake .. && make test_phase_a_router 2>&1 | tail -10`
Expected: build error referencing `test_util::write_synthetic_bayer`. We'll wire the util in Task 19; for now keep these tests under `[.integration]` so they don't run by default. The build itself must succeed for the routing logic.

- [ ] **Step 5: Read the current `stacking_engine.cpp` lines 90-130 to capture surrounding context**

Run: `sed -n '90,130p' src/lib/stacker/src/stacking_engine.cpp`

Expected: shows the existing block with `parse_bayer_pattern`, `if (bayer != BayerPattern::NONE) { ch_config = ChannelConfig::from_mode(StackingMode::OSC_RGB); ...`. **Keep that exact context for the next step.**

- [ ] **Step 6: Replace the hardcoded `OSC_RGB` (lines 101-111) with classifier-driven routing**

In `src/lib/stacker/src/stacking_engine.cpp`, replace this block (currently around lines 101-111):

```cpp
    // Detect Bayer pattern from FITS header
    BayerPattern bayer = parse_bayer_pattern(first.metadata.bayer_pattern);

    // Auto-detect channel config from metadata
    ChannelConfig ch_config;
    if (bayer != BayerPattern::NONE) {
        ch_config = ChannelConfig::from_mode(StackingMode::OSC_RGB);  // ← LINE 107
        ch_config.bayer = bayer;
    } else {
        ch_config = ChannelConfig::from_mode(StackingMode::MONO_L);
    }
```

with:

```cpp
    // Classify the first frame's filter (drives initial ChannelConfig)
    Filter first_filter = filter_classifier_.classify(first.metadata);
    BayerPattern bayer = parse_bayer_pattern(first.metadata.bayer_pattern);

    // Loud-fail: unknown filter on Bayer data is a batch-stopper per spec §6.3
    if (first_filter.cls == FilterClass::UNKNOWN && bayer != BayerPattern::NONE) {
        return ExecuteResult{
            /*ok*/false,
            /*error*/"FILTER='" + first_filter.name + "' on Bayer frame not in QE DB. " +
                     "Add it to ~/.nukex4/qe_overrides.json (see docs) and retry, " +
                     "or remove the FILTER keyword to default to plain OSC.",
            {}, {}, {}, {}
        };
    }

    // Surface mono-side warning if the classifier emitted one
    if (!filter_classifier_.last_warning().empty() && progress) {
        progress->message(filter_classifier_.last_warning().c_str());
    }

    ChannelConfig ch_config = ChannelConfig::from_filter(first_filter);
    if (bayer != BayerPattern::NONE) {
        ch_config.bayer = bayer; // FITS header wins over default
    }
```

(`ExecuteResult` field names depend on the existing return struct — see step 7 for the surrounding adjustment if the existing struct lacks an `error` field.)

- [ ] **Step 7: Confirm `ExecuteResult` shape and adjust if needed**

Run: `grep -n "struct ExecuteResult\|class ExecuteResult\|return.*ExecuteResult\|return.*Result{" src/lib/stacker/include/nukex/stacker/stacking_engine.hpp src/lib/stacker/src/stacking_engine.cpp | head -20`

If the existing return type lacks an `ok` / `error` pair, add them to the struct in the header and wire all existing return sites to set `ok = true`. Wrap the existing all-fields-default constructor as the success default.

- [ ] **Step 8: Add `FilterClassifier` and `QEDatabase` as private members**

Edit `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`. In the private section of `class StackingEngine`:

```cpp
private:
    Config                                config_;
    FilterClassifier                      filter_classifier_;
    QEDatabase                            qe_database_;
    std::unique_ptr<ChannelDecomposer>    decomposer_;     // built lazily after QE load
    std::unique_ptr<ColorComposer>        composer_;
```

Add the matching includes at the top of the header:

```cpp
#include "nukex/io/filter_classifier.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"
#include "nukex/compose/color_composer.hpp"
```

- [ ] **Step 9: Initialize QE DB in the engine constructor**

In `src/lib/stacker/src/stacking_engine.cpp`, modify the `StackingEngine::StackingEngine(const Config&)` constructor body:

```cpp
StackingEngine::StackingEngine(const Config& config)
    : config_(config) {
    auto r1 = qe_database_.load_shipped(config_.qe_database_path);
    if (!r1.ok) {
        // Defer the loud failure to execute() so progress->message can publish it
        qe_load_error_ = r1.error;
        return;
    }
    if (!config_.qe_override_path.empty()) {
        auto r2 = qe_database_.load_override(config_.qe_override_path);
        if (!r2.ok) {
            qe_load_error_ = r2.error;
            return;
        }
    }
    decomposer_ = std::make_unique<ChannelDecomposer>(qe_database_);
    composer_   = std::make_unique<ColorComposer>();
}
```

Add `std::string qe_load_error_;` to the private members in the header.

In `execute(...)`, at the very top:

```cpp
if (!qe_load_error_.empty()) {
    if (progress) progress->message(qe_load_error_.c_str());
    return ExecuteResult{ /*ok*/false, /*error*/qe_load_error_, {}, {}, {}, {} };
}
```

- [ ] **Step 10: Implement Phase A per-frame routing inside the existing accumulation loop**

Locate the per-frame accumulation block in `stacking_engine.cpp` (the section that reads `cube.at(x, y).welford[ch].update(...)` for each pixel). Wrap the per-frame logic to also classify the frame's filter and route into named slots:

```cpp
for (size_t fi = 0; fi < frame_paths.size(); ++fi) {
    auto frame = FITSReader::read(frame_paths[fi]);
    if (!frame.success) { /* existing skip-warn behavior */ continue; }

    DebayerEngine::equalize_bayer_background(frame.image, bayer);
    Image rgb = (bayer != BayerPattern::NONE)
                  ? DebayerEngine::debayer(frame.image, bayer)
                  : frame.image;

    // Per-frame filter classification
    Filter f = filter_classifier_.classify(frame.metadata);

    // Reject mid-batch unknown-on-Bayer
    if (f.cls == FilterClass::UNKNOWN && bayer != BayerPattern::NONE) {
        if (progress) {
            std::string msg = "Frame " + std::to_string(fi+1) +
                              " has unknown FILTER='" + f.name +
                              "' — rejecting frame.";
            progress->message(msg.c_str());
        }
        ++n_frames_failed_alignment_; // reuses existing counter; rename in Task 12
        continue;
    }

    // Merge per-frame config into the cube config (idempotent if same)
    ChannelConfig per_frame_cfg = ChannelConfig::from_filter(f);
    cube.channel_config = ChannelConfig::merge(cube.channel_config, per_frame_cfg);

    // Phase A routing: per pixel, dispatch by filter class
    for (int y = 0; y < cube.height; ++y) {
        for (int x = 0; x < cube.width; ++x) {
            auto& vox = cube.at(x, y);
            switch (f.cls) {
                case FilterClass::BROADBAND_OSC: {
                    float r = rgb.pixel(x, y, 0);
                    float g = rgb.pixel(x, y, 1);
                    float b = rgb.pixel(x, y, 2);
                    int ri = cube.channel_config.slot_index("R");
                    int gi = cube.channel_config.slot_index("G");
                    int bi = cube.channel_config.slot_index("B");
                    int li = cube.channel_config.slot_index("L");
                    vox.welford[ri].update(r);
                    vox.welford[gi].update(g);
                    vox.welford[bi].update(b);
                    vox.welford[li].update(0.299f * r + 0.587f * g + 0.114f * b);
                    vox.histogram[ri].add(r);
                    vox.histogram[gi].add(g);
                    vox.histogram[bi].add(b);
                    vox.histogram[li].add(0.299f * r + 0.587f * g + 0.114f * b);
                    break;
                }
                case FilterClass::DUAL_NB_OSC: {
                    float r = rgb.pixel(x, y, 0);
                    float g = rgb.pixel(x, y, 1);
                    float b = rgb.pixel(x, y, 2);
                    int ri = cube.channel_config.slot_index("R_" + f.name);
                    int gi = cube.channel_config.slot_index("G_" + f.name);
                    int bi = cube.channel_config.slot_index("B_" + f.name);
                    vox.welford[ri].update(r);
                    vox.welford[gi].update(g);
                    vox.welford[bi].update(b);
                    vox.histogram[ri].add(r);
                    vox.histogram[gi].add(g);
                    vox.histogram[bi].add(b);
                    break;
                }
                case FilterClass::BROADBAND_L:
                case FilterClass::NARROWBAND_SINGLE: {
                    float v = rgb.pixel(x, y, 0); // mono → channel 0
                    int   i = cube.channel_config.slot_index(f.name);
                    vox.welford[i].update(v);
                    vox.histogram[i].add(v);
                    break;
                }
                case FilterClass::BROADBAND_RGB: {
                    float v = rgb.pixel(x, y, 0); // single mono channel from filtered mono frame
                    int   i = cube.channel_config.slot_index(f.name); // "R", "G", or "B"
                    vox.welford[i].update(v);
                    vox.histogram[i].add(v);
                    break;
                }
                case FilterClass::UNKNOWN:
                    break; // already handled at top of loop
            }
            ++vox.n_frames;
            vox.n_channels = cube.channel_config.n_channels;
        }
    }
}
```

(The above is the new per-frame body — preserve existing alignment/cache calls in the same loop. The exact `Image::pixel` accessor name may differ; replace with the actual accessor from the existing image type.)

- [ ] **Step 11: Build and run unit tests (no integration tests run by default — they need test_util)**

Run: `cd build && make -j$(nproc) 2>&1 | tail -20 && ctest --output-on-failure 2>&1 | tail -20`
Expected: build succeeds, all unit tests pass. The `[.integration]` tests are excluded by Catch2's default filter.

- [ ] **Step 12: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/stacking_engine.hpp \
        src/lib/stacker/src/stacking_engine.cpp \
        src/lib/stacker/CMakeLists.txt \
        test/integration/CMakeLists.txt \
        test/integration/test_phase_a_router.cpp \
        test/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(stacker): replace hardcoded OSC_RGB with FilterClassifier-driven Phase A

The infamous stacking_engine.cpp:107 hardcoded ChannelConfig::from_mode(
StackingMode::OSC_RGB) is gone. Bayer frames now classify their FILTER
keyword and route into named voxel slots: BROADBAND_OSC → R/G/B + L_synth,
DUAL_NB_OSC → R_HaO3/G_HaO3/B_HaO3 (or S2O3 variant), with merge() across
frames. UNKNOWN-on-Bayer fails the batch loud at start. Mid-batch frames
with the same condition are rejected per-frame with a console message.

Integration tests under [.integration] are gated until Task 19 wires the
mini-FITS test util — the routing logic is exercised through unit tests
on ChannelConfig::from_filter / merge.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: Phase B Q-solve — derived semantic slots

**Files:**
- Modify: `src/lib/stacker/src/stacking_engine.cpp` (Phase B section, after `PixelSelector` runs)
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` (add `StackedResult` derived-slots map)
- Test: `test/integration/test_phase_b_qsolve.cpp` (new)
- Modify: `test/integration/CMakeLists.txt`

After Phase B fits + selects per-pixel best values into raw slots (`R_HaO3`, etc.), the Q-solve runs per pixel × per filter group, producing derived semantic slots (`Ha`, `OIII`, `SII`). For broadband-RGB-from-mono, derived slots pass through (`derived["R"] = raw["R"]`). For multi-source OIII, sample-count weighted mean across HaO3 + S2O3 contributions.

- [ ] **Step 1: Define the `DerivedStack` struct in the engine header**

Edit `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`. Add:

```cpp
struct DerivedStack {
    int width = 0;
    int height = 0;
    // Each map entry: derived semantic slot name → flat float array (width*height)
    std::unordered_map<std::string, std::vector<float>> slots;
    // Q-solve diagnostics
    std::int64_t negative_clamped_count = 0;
};
```

Add `DerivedStack derived;` to the existing `ExecuteResult` struct (or whatever the engine's return struct is named).

- [ ] **Step 2: Write the failing integration test (gated under [.integration])**

Create `test/integration/test_phase_b_qsolve.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("Phase B Q-solve: synthetic HaO3 frame recovers Ha + OIII slots",
          "[.integration][phase_b]") {
    // Build a 16x16 HaO3 Bayer frame where R_HaO3=Ha contribution + small OIII;
    // G_HaO3, B_HaO3 chosen so that pinv(Q) @ (R, G, B) = (Ha=0.5, OIII=0.3)
    auto path = fs::temp_directory_path() / "phase_b_hao3.fits";
    test_util::write_synthetic_q_solved_hao3(path.string(),
                                             16, 16, "ASI585MC",
                                             /*Ha*/0.5f, /*OIII*/0.3f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({path.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.derived.slots.count("Ha")   == 1);
    REQUIRE(result.derived.slots.count("OIII") == 1);

    int center = 8 * 16 + 8;
    REQUIRE(result.derived.slots["Ha"][center]   == Catch::Approx(0.5f).margin(0.02f));
    REQUIRE(result.derived.slots["OIII"][center] == Catch::Approx(0.3f).margin(0.02f));
}

TEST_CASE("Phase B Q-solve: pure broadband-OSC produces no Ha/OIII/SII slot",
          "[.integration][phase_b]") {
    auto path = fs::temp_directory_path() / "phase_b_osc.fits";
    test_util::write_synthetic_bayer(path.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({path.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.derived.slots.count("R") == 1);
    REQUIRE(result.derived.slots.count("G") == 1);
    REQUIRE(result.derived.slots.count("B") == 1);
    REQUIRE(result.derived.slots.count("L") == 1);
    REQUIRE(result.derived.slots.count("Ha")   == 0);
    REQUIRE(result.derived.slots.count("OIII") == 0);
    REQUIRE(result.derived.slots.count("SII")  == 0);
}

TEST_CASE("Phase B Q-solve: HaO3 + S2O3 mixed → multi-source OIII merge",
          "[.integration][phase_b]") {
    auto h = fs::temp_directory_path() / "phase_b_hao3_n.fits";
    auto s = fs::temp_directory_path() / "phase_b_s2o3_n.fits";
    test_util::write_synthetic_q_solved_hao3(h.string(), 16, 16, "ASI585MC", 0.5f, 0.3f);
    test_util::write_synthetic_q_solved_s2o3(s.string(), 16, 16, "ASI585MC", 0.4f, 0.3f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({h.string(), s.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.derived.slots.count("Ha")   == 1);
    REQUIRE(result.derived.slots.count("SII")  == 1);
    REQUIRE(result.derived.slots.count("OIII") == 1);

    int center = 8 * 16 + 8;
    REQUIRE(result.derived.slots["Ha"][center]   == Catch::Approx(0.5f).margin(0.02f));
    REQUIRE(result.derived.slots["SII"][center]  == Catch::Approx(0.4f).margin(0.02f));
    // Multi-source OIII: equal sample counts → mean ≈ 0.3
    REQUIRE(result.derived.slots["OIII"][center] == Catch::Approx(0.3f).margin(0.02f));
}

TEST_CASE("Phase B Q-solve: negative emission clamped, counter incremented",
          "[.integration][phase_b]") {
    // Construct a frame where (R, G, B) leads to negative Hα via Q-solve
    auto p = fs::temp_directory_path() / "phase_b_neg.fits";
    test_util::write_synthetic_q_solved_hao3(p.string(), 16, 16, "ASI585MC",
                                             /*Ha*/-0.1f, /*OIII*/0.3f);
    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({p.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.derived.negative_clamped_count > 0);
    int center = 8 * 16 + 8;
    REQUIRE(result.derived.slots["Ha"][center] >= 0.0f);
}
```

Add to `test/integration/CMakeLists.txt`:

```cmake
nukex_add_test(test_phase_b_qsolve test_phase_b_qsolve.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)
target_compile_definitions(test_phase_b_qsolve PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")
```

- [ ] **Step 3: Add Phase B Q-solve in `stacking_engine.cpp` after the existing per-channel selection block**

Locate the post-PixelSelector block in `stacking_engine.cpp` (where each pixel × each raw channel has `stacked.raw[ch].at(x, y) = best_value`). After it, add the Q-solve and multi-source OIII merge:

```cpp
DerivedStack derived;
derived.width  = cube.width;
derived.height = cube.height;
const int N = cube.width * cube.height;

// Helper: copy a raw slot to derived (broadband passthrough)
auto passthrough_slot = [&](const std::string& name) {
    int idx = cube.channel_config.slot_index(name);
    if (idx == -1) return;
    auto& dst = derived.slots[name];
    dst.assign(N, 0.0f);
    for (int p = 0; p < N; ++p) {
        dst[p] = cube.voxels()[p].selected_value[idx];
    }
};

// Broadband + single-line passthrough slots
for (auto name : {"L", "R", "G", "B", "Ha", "OIII", "SII"}) {
    passthrough_slot(name);
}

// Q-solve for dual-NB groups
struct QGroup { std::string filter_name; std::string r_slot, g_slot, b_slot; };
std::vector<QGroup> groups;
if (cube.channel_config.slot_index("R_HaO3") != -1) {
    groups.push_back({"HaO3", "R_HaO3", "G_HaO3", "B_HaO3"});
}
if (cube.channel_config.slot_index("R_S2O3") != -1) {
    groups.push_back({"S2O3", "R_S2O3", "G_S2O3", "B_S2O3"});
}

// Track Ha / OIII / SII contributions for multi-source merge
std::unordered_map<std::string, std::vector<float>>   line_sum;
std::unordered_map<std::string, std::vector<int64_t>> line_n;
auto ensure_acc = [&](const std::string& name) {
    if (!line_sum.count(name)) {
        line_sum[name].assign(N, 0.0f);
        line_n  [name].assign(N, 0);
    }
};

for (const auto& g : groups) {
    int ri = cube.channel_config.slot_index(g.r_slot);
    int gi = cube.channel_config.slot_index(g.g_slot);
    int bi = cube.channel_config.slot_index(g.b_slot);
    auto fp = qe_database_.lookup_filter(g.filter_name);

    // Map filter line names ("Ha","OIII") or ("SII","OIII") in order from FilterPassband.lines
    Eigen::MatrixXd Q;
    try {
        Q = decomposer_->build_q(first_filter.camera, g.filter_name);
    } catch (const SingularQError& e) {
        return ExecuteResult{ false, std::string("Phase B: ") + e.what(), {}, {}, {}, {}, {} };
    }

    for (int p = 0; p < N; ++p) {
        Eigen::Vector3d rgb(cube.voxels()[p].selected_value[ri],
                            cube.voxels()[p].selected_value[gi],
                            cube.voxels()[p].selected_value[bi]);
        Eigen::VectorXd lines = Q.colPivHouseholderQr().solve(rgb);
        // lines.size() == fp.lines.size()
        for (int j = 0; j < lines.size(); ++j) {
            float v = static_cast<float>(lines(j));
            if (v < 0.0f) { v = 0.0f; ++derived.negative_clamped_count; }
            ensure_acc(fp.lines[j].name);
            line_sum[fp.lines[j].name][p] += v
                * static_cast<float>(cube.voxels()[p].welford[gi].count());
            line_n  [fp.lines[j].name][p] +=
                cube.voxels()[p].welford[gi].count();
        }
    }
}

// Finalize derived line slots via sample-count weighted mean
for (auto& [name, sumv] : line_sum) {
    auto& dst = derived.slots[name];
    dst.assign(N, 0.0f);
    for (int p = 0; p < N; ++p) {
        if (line_n[name][p] > 0) {
            dst[p] = sumv[p] / static_cast<float>(line_n[name][p]);
        }
    }
}

result.derived = std::move(derived);
```

(`cube.voxels()` is a new accessor — add `const std::vector<SubcubeVoxel>& voxels() const { return voxels_; }` to the `Cube` class. Do this in the same task — small change.)

- [ ] **Step 4: Add the `voxels()` accessor**

Edit `src/lib/core/include/nukex/core/cube.hpp` and add inside `class Cube`:

```cpp
const std::vector<SubcubeVoxel>& voxels() const { return voxels_; }
std::vector<SubcubeVoxel>&       voxels()       { return voxels_; }
```

If `SubcubeVoxel` does not currently have a `selected_value[MAX_CHANNELS]` array for Phase B output, add it (single line per voxel — fits naturally next to `snr[MAX_CHANNELS]`):

```cpp
float selected_value[MAX_CHANNELS] = {};   // PixelSelector output
```

If a different name is already used (`stacked_value`, `combined_value`, etc.), use that instead — grep first.

- [ ] **Step 5: Build (integration tests still gated, must compile)**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/stacking_engine.hpp \
        src/lib/stacker/src/stacking_engine.cpp \
        src/lib/core/include/nukex/core/cube.hpp \
        src/lib/core/include/nukex/core/voxel.hpp \
        test/integration/test_phase_b_qsolve.cpp \
        test/integration/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(stacker): Phase B Q-solve produces derived semantic slots

After per-pixel pixel-selection, Q-solve runs per filter group on the
selected raw RGB slots, producing semantic emission-line slots
(Ha, OIII, SII). Multi-source OIII (HaO3 + S2O3) merged via sample-
count weighted mean. Negative-emission values clamped to zero with
counter for diagnostic. SingularQError propagates as a loud-fail
StackingEngine::ExecuteResult error. Broadband + single-line raw
slots pass through unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: ColorComposer hookup — derived slots → 3-channel sRGB output

**Files:**
- Modify: `src/lib/stacker/src/stacking_engine.cpp` (final-emission section)
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` (`ExecuteResult.composed` field)
- Test: `test/integration/test_color_composer_hookup.cpp` (new)
- Modify: `test/integration/CMakeLists.txt`

After Phase B produces `DerivedStack`, the engine runs ColorComposer per pixel and produces a 3-channel `Image` ready for stretching + emit.

- [ ] **Step 1: Add `composed` to `ExecuteResult`**

Edit `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp`:

```cpp
struct ExecuteResult {
    bool        ok = true;
    std::string error;
    Image       stacked;   // legacy single/multi-channel for backward compat (passthrough of derived)
    Image       composed;  // 3-channel sRGB from ColorComposer
    Cube        cube;
    DerivedStack derived;
    StackingStats stats;
    std::int64_t gamut_clipped_count = 0;
};
```

- [ ] **Step 2: Write the failing integration test**

Create `test/integration/test_color_composer_hookup.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/stacker/stacking_engine.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("ColorComposer hookup: HaO3 single-frame produces 3-ch composed image",
          "[.integration][composer_hookup]") {
    auto path = fs::temp_directory_path() / "comp_hao3.fits";
    test_util::write_synthetic_q_solved_hao3(path.string(), 16, 16, "ASI585MC", 0.5f, 0.3f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({path.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.composed.n_channels() == 3);
    REQUIRE(result.composed.width()      == 16);
    REQUIRE(result.composed.height()     == 16);

    // Hα-dominant pixel → red channel highest
    float r = result.composed.pixel(8, 8, 0);
    float g = result.composed.pixel(8, 8, 1);
    float b = result.composed.pixel(8, 8, 2);
    INFO("RGB at (8,8) = (" << r << ", " << g << ", " << b << ")");
    REQUIRE(r > g);
    REQUIRE(r > b);
}

TEST_CASE("ColorComposer hookup: pure broadband-OSC composed ≈ debayered RGB",
          "[.integration][composer_hookup]") {
    auto path = fs::temp_directory_path() / "comp_osc.fits";
    test_util::write_synthetic_bayer(path.string(), 16, 16, "RGGB", "ASI585MC", "", 0.5f);

    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({path.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.composed.n_channels() == 3);
    // No emission slots → composed is broadband chrominance only
    REQUIRE(result.composed.pixel(8, 8, 0) == Catch::Approx(0.5f).margin(0.05f));
    REQUIRE(result.composed.pixel(8, 8, 1) == Catch::Approx(0.5f).margin(0.05f));
    REQUIRE(result.composed.pixel(8, 8, 2) == Catch::Approx(0.5f).margin(0.05f));
}
```

Add to `test/integration/CMakeLists.txt`:

```cmake
nukex_add_test(test_color_composer_hookup test_color_composer_hookup.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)
target_compile_definitions(test_color_composer_hookup PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")
```

- [ ] **Step 3: Verify build fails (composer call site missing)**

Run: `cd build && cmake .. && make test_color_composer_hookup 2>&1 | tail -10`
Expected: build error (the engine has no `composed` field assignment yet).

- [ ] **Step 4: Implement the composer call site in `stacking_engine.cpp`**

After the `result.derived = std::move(derived);` line from Task 10, add:

```cpp
// Build composed 3-ch sRGB from derived slots
result.composed = Image(result.derived.width, result.derived.height, /*channels*/3);
const auto& d = result.derived;
auto get_or_zero = [&](const std::string& name, int p) -> double {
    auto it = d.slots.find(name);
    if (it == d.slots.end() || p >= static_cast<int>(it->second.size())) return 0.0;
    return it->second[p];
};
for (int p = 0, y = 0; y < d.height; ++y) {
    for (int x = 0; x < d.width; ++x, ++p) {
        DerivedSlots ds;
        ds.L    = get_or_zero("L",    p);
        ds.R    = get_or_zero("R",    p);
        ds.G    = get_or_zero("G",    p);
        ds.B    = get_or_zero("B",    p);
        ds.Ha   = get_or_zero("Ha",   p);
        ds.OIII = get_or_zero("OIII", p);
        ds.SII  = get_or_zero("SII",  p);

        sRGBPixel out = composer_->compose_pixel(ds);
        result.composed.set_pixel(x, y, 0, static_cast<float>(out.r));
        result.composed.set_pixel(x, y, 1, static_cast<float>(out.g));
        result.composed.set_pixel(x, y, 2, static_cast<float>(out.b));
    }
}
result.gamut_clipped_count = composer_->gamut_clipped_count();

// Legacy `stacked` field: for backward compat, point at composed when available
result.stacked = result.composed;
```

(`Image::set_pixel(x, y, ch, v)` mirrors the existing `pixel()` accessor — if the existing API uses a different setter name, use it.)

- [ ] **Step 5: Build**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20`
Expected: clean build, all unit tests still pass via `ctest --output-on-failure`.

- [ ] **Step 6: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/stacking_engine.hpp \
        src/lib/stacker/src/stacking_engine.cpp \
        test/integration/test_color_composer_hookup.cpp \
        test/integration/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(stacker): wire ColorComposer at end of pipeline → 3-ch sRGB output

After Phase B Q-solve produces derived semantic slots, ColorComposer
runs per pixel and emits a 3-channel composed Image. ExecuteResult
gains a 'composed' field (Image) and a 'gamut_clipped_count' diagnostic.
Legacy 'stacked' field aliases to composed for downstream compat.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 4: Module + UI Integration

### Task 12: `NukeXInstance` wires QE-DB path + emits ImageWindow from composer output

**Files:**
- Modify: `src/module/NukeXInstance.h` (add `qeOverridePath` field)
- Modify: `src/module/NukeXInstance.cpp` (wire `Config.qe_database_path` + `qe_override_path`; consume `result.composed` for ImageWindow)
- Modify: `src/module/NukeXParameters.h` + `.cpp` (register `qeOverridePath` parameter)
- Test: build + manual smoke (no PCL unit tests in module layer)

NukeXInstance takes the user-supplied override path (default empty), passes it through to engine config along with the resolved shipped DB path. After execute returns, it emits a 3-channel ImageWindow from `result.composed` instead of the legacy multi-channel `result.stacked`.

QE database resolution at startup:
- Look first at `${MODULE_DIR}/share/qe_database.json` (mirrors how rating DB is shipped).
- Fall back to `share/qe_database.json` relative to current working directory (developer convenience).

- [ ] **Step 1: Read NukeXInstance.h to know exact existing layout (lines 1-100)**

Run: `sed -n '1,100p' src/module/NukeXInstance.h`

Note the position of `cacheDirectory` — `qeOverridePath` will sit right after it.

- [ ] **Step 2: Add `qeOverridePath` to NukeXInstance.h**

In the public section of `class NukeXInstance` (after `String cacheDirectory`):

```cpp
   String      qeOverridePath        = String();   // empty = use shipped DB only
```

- [ ] **Step 3: Add `qeOverridePath` parameter registration**

Edit `src/module/NukeXParameters.h`. Find the existing `class NXCacheDirectory` (or equivalent String parameter); copy its pattern for the new parameter:

```cpp
class NXQEOverridePath : public MetaString
{
public:
   NXQEOverridePath( MetaProcess* P );
   IsoString Id() const override;
   size_type MinLength() const override;
   String AllowedCharacters() const override;
   String DefaultValue() const override;
};
extern NXQEOverridePath* TheNXQEOverridePathParameter;
```

In `src/module/NukeXParameters.cpp`, add the implementation block following the `NXCacheDirectory` pattern. Set `Id()` to `"qeOverridePath"` and `DefaultValue()` to `String()`.

In `src/module/NukeXProcess.cpp` (or wherever parameters are registered with the metaprocess), add:

```cpp
new NXQEOverridePath( this );
```

In `NukeXInstance::Assign(const ProcessImplementation& other)`, copy the new field:

```cpp
qeOverridePath = static_cast<const NukeXInstance&>(other).qeOverridePath;
```

In `NukeXInstance::ExportParameters()` and `ImportParameters()` (or whatever serialization hooks exist), add the new field.

- [ ] **Step 4: Resolve the shipped DB path at runtime**

In `src/module/NukeXInstance.cpp`, near the top of `ExecuteGlobal()`, before the `nukex::StackingEngine::Config config;` declaration:

```cpp
auto resolve_qe_db = []() -> String {
    String moduleDir = pcl::ModuleHandle::Module()->Description().filePath.Directory();
    String dbPath = moduleDir + "/share/qe_database.json";
    if (pcl::File::Exists(dbPath)) return dbPath;
    return "share/qe_database.json"; // dev fallback
};
String resolvedDbPath = resolve_qe_db();
```

(Adjust the PCL module-directory accessor to whatever the codebase already uses — run `grep -rn "Module()->Description\|ModuleDirectory\|InstallDir" src/module/` first.)

- [ ] **Step 5: Pass paths into engine config**

In `ExecuteGlobal()`, where `nukex::StackingEngine::Config config;` is built:

```cpp
config.cache_dir         = cacheDirectory.ToUTF8().c_str();
config.qe_database_path  = resolvedDbPath.ToUTF8().c_str();
config.qe_override_path  = qeOverridePath.IsEmpty() ? std::string()
                                                    : qeOverridePath.ToUTF8().c_str();
config.gpu_config.force_cpu_fallback = !enableGPU;
```

- [ ] **Step 6: Emit `ImageWindow` from `result.composed`**

Locate the existing `ImageWindow window(...)` block. Replace the channel-count and pixel copy logic to source from `result.composed`:

```cpp
if (result.composed.width() > 0 && result.composed.height() > 0)
{
   const int w  = result.composed.width();
   const int h  = result.composed.height();
   const int nc = result.composed.n_channels(); // always 3 from composer

   ImageWindow window(w, h, nc,
                      32,    // bits per sample (float32)
                      true,  // float sample
                      true,  // color (always 3-ch from composer)
                      true,  // initialProcessing
                      "NukeX_stacked");

   View view = window.MainView();
   ImageVariant v = view.Image();
   if (v.IsFloatSample() && v.BitsPerSample() == 32)
   {
      pcl::Image& img = static_cast<pcl::Image&>(*v);
      for (int ch = 0; ch < nc; ++ch)
      {
         const float* src = result.composed.channel_data(ch);
         float* dst       = img.PixelData(ch);
         ::memcpy(dst, src, w * h * sizeof(float));
      }
   }

   FITSKeywordArray kws;
   kws.Add(FITSHeaderKeyword("HISTORY",
       String().Format("NukeX v%d.%d.%d.%d color-science overhaul",
           NUKEX_MODULE_VERSION_MAJOR, NUKEX_MODULE_VERSION_MINOR,
           NUKEX_MODULE_VERSION_REVISION, NUKEX_MODULE_VERSION_BUILD),
       ""));
   kws.Add(FITSHeaderKeyword("NUKEX_GAMUT_CLIP",
       String(result.gamut_clipped_count), "pixels soft-clipped to gamut"));
   kws.Add(FITSHeaderKeyword("NUKEX_NEG_CLAMP",
       String(result.derived.negative_clamped_count),
       "Q-solve negative emission clamps"));
   view.Window().SetKeywords(kws);

   window.Show();
}
```

- [ ] **Step 7: Build the module + run all tests**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20 && ctest --output-on-failure 2>&1 | tail -10`
Expected: clean build, all unit tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/module/NukeXInstance.h \
        src/module/NukeXInstance.cpp \
        src/module/NukeXParameters.h \
        src/module/NukeXParameters.cpp \
        src/module/NukeXProcess.cpp
git commit -m "$(cat <<'EOF'
feat(module): wire QE-DB + override path; emit composed ImageWindow

NukeXInstance gains qeOverridePath parameter (optional, defaults to
empty = shipped DB only). Resolves shipped DB path from module install
dir at startup. Passes both paths into StackingEngine::Config. Emits
output ImageWindow from result.composed (always 3-channel sRGB) instead
of legacy multi-channel result.stacked. Annotates output FITS with QE
confidence + gamut-clip + Q-solve clamp stats.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: `NukeXInterface` — QE override file picker

**Files:**
- Modify: `src/module/NukeXInterface.h` (add controls to `GUIData`)
- Modify: `src/module/NukeXInterface.cpp` (build + wire controls)

A simple Edit + Browse button below the existing Options section. The picker uses `pcl::OpenFileDialog` filtered to `*.json`. The selected path is committed to `instance.qeOverridePath` and persists with the process instance.

- [ ] **Step 1: Add GUIData fields**

Edit `src/module/NukeXInterface.h`. Inside `struct GUIData`, after the existing `EnableGPU_CheckBox` block:

```cpp
   HorizontalSizer  QEOverride_Sizer;
   Label            QEOverride_Label;
   Edit             QEOverride_Edit;
   PushButton       QEOverride_Browse_Button;
   PushButton       QEOverride_Clear_Button;
```

- [ ] **Step 2: Wire the controls in `NukeXInterface.cpp` constructor**

Find the existing `EnableGPU_CheckBox.OnClick(...)` registration. Right after it, add:

```cpp
   QEOverride_Label.SetText("QE override file:");
   QEOverride_Label.SetToolTip("Optional JSON file with custom camera/filter QE values "
                               "to augment or replace the shipped database.");

   QEOverride_Edit.SetReadOnly();
   QEOverride_Edit.SetMinWidth(360);
   QEOverride_Edit.SetText(instance.qeOverridePath);

   QEOverride_Browse_Button.SetText("Browse…");
   QEOverride_Browse_Button.OnClick(
       (Button::click_event_handler)&NukeXInterface::__QEOverride_Browse_Click, *this);

   QEOverride_Clear_Button.SetText("Clear");
   QEOverride_Clear_Button.OnClick(
       (Button::click_event_handler)&NukeXInterface::__QEOverride_Clear_Click, *this);

   QEOverride_Sizer.SetSpacing(6);
   QEOverride_Sizer.Add(QEOverride_Label);
   QEOverride_Sizer.Add(QEOverride_Edit, 100);
   QEOverride_Sizer.Add(QEOverride_Browse_Button);
   QEOverride_Sizer.Add(QEOverride_Clear_Button);

   Options_Sizer.Add(QEOverride_Sizer);
```

- [ ] **Step 3: Add the click handlers**

In `NukeXInterface.h` (private section):

```cpp
   void __QEOverride_Browse_Click( Button& sender, bool checked );
   void __QEOverride_Clear_Click ( Button& sender, bool checked );
```

In `NukeXInterface.cpp`:

```cpp
void NukeXInterface::__QEOverride_Browse_Click( Button& /*sender*/, bool /*checked*/ )
{
   OpenFileDialog dlg;
   dlg.SetCaption("Select QE override JSON file");
   dlg.SetFilters({ FileFilter("JSON files", "*.json"),
                    FileFilter("All files",  "*")    });
   dlg.DisableMultipleSelections();
   if (dlg.Execute() && !dlg.FileNames().IsEmpty())
   {
      String chosen = dlg.FileName();
      instance.qeOverridePath = chosen;
      GUI->QEOverride_Edit.SetText(chosen);
   }
}

void NukeXInterface::__QEOverride_Clear_Click( Button& /*sender*/, bool /*checked*/ )
{
   instance.qeOverridePath = String();
   GUI->QEOverride_Edit.Clear();
}
```

- [ ] **Step 4: Update `UpdateControls()` to populate the edit on instance load**

Find the existing `NukeXInterface::UpdateControls()`. Add at the bottom:

```cpp
   GUI->QEOverride_Edit.SetText(instance.qeOverridePath);
```

- [ ] **Step 5: Build**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20`
Expected: clean build (no PCL unit tests in this layer; smoke-test inside PixInsight in Task 21).

- [ ] **Step 6: Commit**

```bash
git add src/module/NukeXInterface.h src/module/NukeXInterface.cpp
git commit -m "$(cat <<'EOF'
feat(module): QE override file picker in NukeX interface

Adds Edit + Browse + Clear controls below the existing Options
section. Browse uses OpenFileDialog filtered to *.json; selected
path commits to instance.qeOverridePath and persists with the
process. Empty by default → shipped QE DB only.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 14: Migrate Phase 8 `lastRun.filter_class` + `stretch_auto_selector` to new 5-class enum

**Files:**
- Modify: `src/module/NukeXInstance.cpp` (callsites at lines 160, 591-592, 691; rating-int mapping)
- Modify: `src/module/NukeXInterface.cpp` (line 484)
- Modify: `src/module/RatingDialog.h` + `.cpp` (constructor signature)
- Modify: `src/module/stretch_auto_selector.cpp` + `.hpp` (consumes new enum from `nukex/io/filter_classifier.hpp`)
- Modify: `src/lib/learning/include/nukex/learning/rating_database.hpp` + `.cpp` (V2→V3 schema bump + remap of stored class codes)
- Modify: `src/module/CMakeLists.txt` (link `nukex4_io` so the module sees the new classifier)
- Test: `test/unit/learning/test_rating_db_migration.cpp` (new)
- Modify: `test/unit/learning/CMakeLists.txt`

**Mapping from old 4-class to new 5-class** (used by V2→V3 migration):

| Old code | Old enum    | New code | New enum            |
|----------|-------------|----------|---------------------|
| 0        | LRGB_MONO   | 1        | BROADBAND_L         |
| 1        | LRGB_COLOR  | 3        | BROADBAND_OSC       |
| 2        | BAYER_RGB   | 3        | BROADBAND_OSC       |
| 3        | NARROWBAND  | 4        | NARROWBAND_SINGLE   |

Rationale: pre-v5 user ratings can't distinguish dual-NB from single-line narrowband (the v4 classifier didn't), so they collapse to NARROWBAND_SINGLE. After v5, the classifier emits DUAL_NB_OSC for HaO3/S2O3 and the rating system learns those separately. This is intentional — pre-v5 ratings on dual-NB stacks were tuned against the broken M27-green output, so reusing them for the v5 calibrated path would mis-tune.

- [ ] **Step 1: Write the failing migration test**

Create `test/unit/learning/test_rating_db_migration.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/learning/rating_database.hpp"

#include <filesystem>
#include <fstream>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("Rating DB: V2 -> V3 migration maps old filter_class codes",
          "[rating_db][migration]") {
    auto path = fs::temp_directory_path() / "rating_v2.db";
    if (fs::exists(path)) fs::remove(path);

    {
        RatingDatabase db(path.string());
        db.force_set_schema_version(2);
        db.insert_legacy_rating(/*old_class*/0, "GHS", 4, "{}");
        db.insert_legacy_rating(/*old_class*/1, "GHS", 4, "{}");
        db.insert_legacy_rating(/*old_class*/2, "GHS", 4, "{}");
        db.insert_legacy_rating(/*old_class*/3, "GHS", 4, "{}");
    }

    RatingDatabase db(path.string());
    REQUIRE(db.schema_version() == 3);

    auto rows = db.all_ratings();
    REQUIRE(rows.size() == 4);

    int n_bb_l = 0, n_bb_osc = 0, n_nb_single = 0;
    for (const auto& r : rows) {
        if (r.filter_class == 1) ++n_bb_l;
        if (r.filter_class == 3) ++n_bb_osc;
        if (r.filter_class == 4) ++n_nb_single;
    }
    REQUIRE(n_bb_l      == 1);
    REQUIRE(n_bb_osc    == 2);
    REQUIRE(n_nb_single == 1);
}
```

Add registration to `test/unit/learning/CMakeLists.txt`:

```cmake
nukex_add_test(test_rating_db_migration test_rating_db_migration.cpp nukex4_learning)
```

- [ ] **Step 2: Verify build fails**

Run: `cd build && cmake .. && make test_rating_db_migration 2>&1 | tail -10`
Expected: build error referencing `force_set_schema_version` / `insert_legacy_rating` (helpers not present yet).

- [ ] **Step 3: Add migration helpers to `rating_database.hpp`**

Edit `src/lib/learning/include/nukex/learning/rating_database.hpp`. Update the `RatingDatabase` class:

```cpp
class RatingDatabase {
public:
    RatingDatabase(const std::string& path);
    ~RatingDatabase();

    int schema_version() const;

    struct Rating {
        int          id;
        int          filter_class;
        std::string  stretch_name;
        int          rating;
        std::string  params_json;
    };
    std::vector<Rating> all_ratings() const;

    // Test hooks
    void force_set_schema_version(int v);
    void insert_legacy_rating(int old_filter_class,
                              const std::string& stretch,
                              int rating,
                              const std::string& params);

private:
    std::string path_;
    void open_or_create();
    void run_v3_migration();
};
```

- [ ] **Step 4: Implement the V2→V3 migration in `rating_database.cpp`**

In `src/lib/learning/src/rating_database.cpp`, use the existing prepared-statement wrapper helpers (rather than ad-hoc raw SQL invocation) so error reporting and transaction scope are consistent with the rest of the file:

```cpp
void RatingDatabase::run_v3_migration() {
    // Map old 4-class codes to new 5-class codes:
    //   0 (LRGB_MONO)  -> 1 (BROADBAND_L)
    //   1 (LRGB_COLOR) -> 3 (BROADBAND_OSC)
    //   2 (BAYER_RGB)  -> 3 (BROADBAND_OSC)
    //   3 (NARROWBAND) -> 4 (NARROWBAND_SINGLE)

    static const std::pair<int, int> kRemap[] = {
        {0, 1}, {1, 3}, {2, 3}, {3, 4}
    };

    auto stmt_update = prepare("UPDATE ratings SET filter_class = ? WHERE filter_class = ?;");
    for (const auto& [old_code, new_code] : kRemap) {
        bind_int(stmt_update, 1, new_code);
        bind_int(stmt_update, 2, old_code);
        step_done(stmt_update);
        reset(stmt_update);
    }

    auto stmt_meta = prepare("UPDATE schema_meta SET version = 3;");
    step_done(stmt_meta);
}
```

Where `prepare`, `bind_int`, `step_done`, `reset` are the existing wrapper functions in this file (or the same helpers used elsewhere — grep first, reuse the pattern; do not reach for the raw C entry points directly). If those wrappers don't exist yet, add them at the top of the file as small `inline` helpers around the SQLite C API.

In `open_or_create()`, after reading the schema version:

```cpp
int v = read_schema_version();
if (v < 3) {
    run_v3_migration();
}
```

- [ ] **Step 5: Run test to verify pass**

Run: `cd build && cmake .. && make test_rating_db_migration && ctest -R test_rating_db_migration --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Update callsites in module to use new enum**

In `src/module/stretch_auto_selector.hpp` + `.cpp`, swap the include from `"filter_classifier.hpp"` to `"nukex/io/filter_classifier.hpp"` and update calls:

```cpp
// Before:
//   FilterClass cls = classify_filter(meta);
// After:
   FilterClassifier classifier;
   Filter f = classifier.classify(meta);
   FilterClass cls = f.cls;
```

Update the `switch (cls)` block to handle all 5 new values plus UNKNOWN:

```cpp
switch (cls) {
    case FilterClass::BROADBAND_L:
        // L mono: same as old LRGB_MONO behavior
        break;
    case FilterClass::BROADBAND_RGB:
    case FilterClass::BROADBAND_OSC:
        // RGB-color or OSC: same as old LRGB_COLOR/BAYER_RGB
        break;
    case FilterClass::NARROWBAND_SINGLE:
    case FilterClass::DUAL_NB_OSC:
        // Both narrowband paths use same auto-stretch family
        break;
    case FilterClass::UNKNOWN:
        // Default to a conservative GHS
        break;
}
```

In `src/module/NukeXInstance.cpp`, replace the `nukex::classify_filter(meta)` call at line 591-592 with the new classifier:

```cpp
nukex::FilterClassifier cls;
nukex::Filter f = cls.classify(meta);
lastRun.filter_class = filter_class_to_rating_int(f.cls);
```

Update `filter_class_to_rating_int()` mapping to the new 5-class enum:

```cpp
int filter_class_to_rating_int(nukex::FilterClass c) {
    switch (c) {
        case nukex::FilterClass::BROADBAND_L:       return 1;
        case nukex::FilterClass::BROADBAND_RGB:     return 2;
        case nukex::FilterClass::BROADBAND_OSC:     return 3;
        case nukex::FilterClass::NARROWBAND_SINGLE: return 4;
        case nukex::FilterClass::DUAL_NB_OSC:       return 5;
        case nukex::FilterClass::UNKNOWN:           return 0;
    }
    return 0;
}
```

In `src/module/RatingDialog.h`, update the constructor parameter type if it currently takes the old enum directly. If it takes an `int`, no change needed.

In `src/module/CMakeLists.txt`, add `nukex4_io` to the link list for the module (so it can see the new classifier header).

- [ ] **Step 7: Build the module**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20 && ctest --output-on-failure 2>&1 | tail -10`
Expected: clean build, all unit tests pass (including the new migration test).

- [ ] **Step 8: Commit**

```bash
git add src/module/NukeXInstance.cpp \
        src/module/NukeXInterface.cpp \
        src/module/RatingDialog.h src/module/RatingDialog.cpp \
        src/module/stretch_auto_selector.hpp src/module/stretch_auto_selector.cpp \
        src/module/CMakeLists.txt \
        src/lib/learning/include/nukex/learning/rating_database.hpp \
        src/lib/learning/src/rating_database.cpp \
        test/unit/learning/test_rating_db_migration.cpp \
        test/unit/learning/CMakeLists.txt
git commit -m "$(cat <<'EOF'
refactor(module): migrate Phase 8 rating + auto-selector to 5-class enum

stretch_auto_selector and NukeXInstance now consume the new
FilterClassifier from src/lib/io/. lastRun.filter_class becomes
the new 5-value mapping (1=BROADBAND_L, 2=BROADBAND_RGB, 3=
BROADBAND_OSC, 4=NARROWBAND_SINGLE, 5=DUAL_NB_OSC, 0=UNKNOWN).
Rating DB schema bumps V2 -> V3 with explicit code remapping; pre-
v5 dual-NB ratings collapse to NARROWBAND_SINGLE since the v4
classifier could not distinguish them and those ratings were
tuned against the broken-green M27 output anyway.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 5: Data + Tooling

### Task 15: `tools/import_qe_research.py` — research JSON → shipping JSON transform

**Files:**
- Create: `tools/import_qe_research.py`
- Create: `tools/test_import_qe_research.py`

The transform:
1. Reads `research/qe_database_research.json`
2. Drops the `_meta` block (research-only metadata)
3. Drops `qe_inherits_from_sensor` markers from cameras (the script resolves them)
4. For each camera with `qe_inherits_from_sensor: true`, copies QE values from the corresponding sensor entry (per the research methodology note)
5. Validates the output against the JSON schema described in Task 3 fixtures (camera with `qe`, `confidence`; filter with `lines`)
6. Writes `share/qe_database.json`

This is a one-shot script (run by the dev when refreshing the shipped DB), but lives in `tools/` so it's reproducible.

- [ ] **Step 1: Write the tests**

Create `tools/test_import_qe_research.py`:

```python
import json
import os
import subprocess
import tempfile
import pathlib
import pytest

REPO = pathlib.Path(__file__).parent.parent
SCRIPT = REPO / "tools" / "import_qe_research.py"

def run_import(input_path, output_path):
    result = subprocess.run(
        ["python3", str(SCRIPT), str(input_path), str(output_path)],
        capture_output=True, text=True, check=False,
    )
    return result

def test_drops_meta_block(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "_meta": {"researcher": "test"},
        "sensors": {},
        "cameras": {
            "ASI_test": {
                "sensor": "Fake",
                "type": "OSC",
                "bayer": "RGGB",
                "qe": {"656": {"R": 0.5, "G": 0.1, "B": 0.05}},
                "confidence": "high"
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    assert "_meta" not in out
    assert "ASI_test" in out["cameras"]

def test_resolves_sensor_inheritance(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "_meta": {"researcher": "test"},
        "sensors": {
            "IMX571": {
                "qe": {
                    "656": {"R": 0.46, "Gr": 0.05, "Gb": 0.05, "B": 0.04, "mono_pk": 0.50}
                },
                "confidence": "high"
            }
        },
        "cameras": {
            "ASI2600MC": {
                "sensor": "IMX571",
                "type": "OSC",
                "bayer": "RGGB",
                "qe_inherits_from_sensor": True,
                "confidence": "medium"
            }
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    cam = out["cameras"]["ASI2600MC"]
    assert "qe_inherits_from_sensor" not in cam
    assert "qe" in cam
    assert cam["qe"]["656"]["R"] == 0.46

def test_validates_camera_required_fields(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({
        "cameras": {
            "Bad": { "type": "OSC" }  # missing qe + confidence
        },
        "filters": {}
    }))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode != 0
    assert "Bad" in r.stderr

def test_writes_schema_version(tmp_path):
    src = tmp_path / "research.json"
    src.write_text(json.dumps({"sensors": {}, "cameras": {}, "filters": {}}))
    dst = tmp_path / "shipped.json"
    r = run_import(src, dst)
    assert r.returncode == 0
    out = json.loads(dst.read_text())
    assert out.get("schema_version") == 1
```

- [ ] **Step 2: Run tests to verify failure (script missing)**

Run: `python3 -m pytest tools/test_import_qe_research.py -v 2>&1 | tail -10`
Expected: errors importing/running the missing script.

- [ ] **Step 3: Implement the script**

Create `tools/import_qe_research.py`:

```python
#!/usr/bin/env python3
"""
Transform research/qe_database_research.json into share/qe_database.json.

Drops the `_meta` block, resolves `qe_inherits_from_sensor` references,
normalises Bayer photosite keys (Gr/Gb -> G average where applicable),
validates required fields, and writes a versioned shipping JSON.
"""

import argparse
import json
import sys

REQUIRED_CAMERA_FIELDS = ("type", "confidence")

def normalise_qe_block(qe):
    """Collapse Gr/Gb into a single G value (mean) for shipping."""
    out = {}
    for wl, sites in qe.items():
        s = {}
        if "R" in sites: s["R"] = sites["R"]
        if "G" in sites:
            s["G"] = sites["G"]
        else:
            gr = sites.get("Gr"); gb = sites.get("Gb")
            if gr is not None and gb is not None:
                s["G"] = (gr + gb) / 2.0
            elif gr is not None: s["G"] = gr
            elif gb is not None: s["G"] = gb
        if "B" in sites: s["B"] = sites["B"]
        out[wl] = s
    return out

def resolve_camera(name, cam, sensors):
    """Resolve qe_inherits_from_sensor by copying sensor QE."""
    out = dict(cam)
    if out.pop("qe_inherits_from_sensor", False):
        sensor_name = out.get("sensor")
        if not sensor_name or sensor_name not in sensors:
            raise ValueError(
                f"Camera '{name}' inherits from sensor "
                f"'{sensor_name}' but sensor not found in research"
            )
        out["qe"] = sensors[sensor_name].get("qe", {})
    if "qe" in out:
        out["qe"] = normalise_qe_block(out["qe"])
    return out

def validate_camera(name, cam):
    missing = [f for f in REQUIRED_CAMERA_FIELDS if f not in cam]
    if missing:
        raise ValueError(f"Camera '{name}' missing required fields: {missing}")
    if "qe" not in cam:
        raise ValueError(f"Camera '{name}' missing 'qe' block (after inheritance resolution)")

def transform(research):
    sensors = research.get("sensors", {})
    out_cameras = {}
    for name, cam in research.get("cameras", {}).items():
        resolved = resolve_camera(name, cam, sensors)
        validate_camera(name, resolved)
        out_cameras[name] = resolved

    return {
        "schema_version": 1,
        "cameras": out_cameras,
        "filters": research.get("filters", {}),
    }

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input",  help="path to research/qe_database_research.json")
    ap.add_argument("output", help="path to share/qe_database.json")
    args = ap.parse_args()

    with open(args.input, "r") as f:
        research = json.load(f)

    try:
        shipped = transform(research)
    except ValueError as e:
        print(f"Validation error: {e}", file=sys.stderr)
        return 1

    with open(args.output, "w") as f:
        json.dump(shipped, f, indent=2, sort_keys=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

Make it executable: `chmod +x tools/import_qe_research.py`.

- [ ] **Step 4: Run tests to verify pass**

Run: `python3 -m pytest tools/test_import_qe_research.py -v 2>&1 | tail -20`
Expected: 4 passed.

- [ ] **Step 5: Commit**

```bash
git add tools/import_qe_research.py tools/test_import_qe_research.py
git commit -m "$(cat <<'EOF'
feat(tools): import_qe_research.py transforms research → shipping JSON

Drops _meta block, resolves qe_inherits_from_sensor references against
the sensors map, normalises Gr/Gb to a single G mean, validates required
fields per camera, writes share/qe_database.json with schema_version 1.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 16: Generate + commit `share/qe_database.json`

**Files:**
- Create: `share/qe_database.json` (generated artifact)
- Modify: `CMakeLists.txt` (install rule for `share/`)
- Modify: `tools/release.sh` (include `share/qe_database.json` in the package tarball)

- [ ] **Step 1: Generate the shipped DB from research**

Run:

```bash
mkdir -p share
python3 tools/import_qe_research.py research/qe_database_research.json share/qe_database.json
```

- [ ] **Step 2: Sanity-check the output**

Run:

```bash
python3 -c "
import json
db = json.load(open('share/qe_database.json'))
print('schema_version:', db['schema_version'])
print('cameras:', len(db['cameras']))
print('filters:', len(db['filters']))
print('first 3 cameras:', list(db['cameras'].keys())[:3])
print('confidences:', set(c['confidence'] for c in db['cameras'].values()))
"
```

Expected: `schema_version: 1`, `cameras: ~55`, `filters: ~87`, confidence set ⊆ `{high, medium, low}`.

- [ ] **Step 3: Add CMake install rule**

Edit root `CMakeLists.txt`. After the existing module install rules, add:

```cmake
install(FILES share/qe_database.json
        DESTINATION share
        COMPONENT runtime)
```

- [ ] **Step 4: Update `tools/release.sh` to include `share/` in the tarball**

In `tools/release.sh`, find the `tar` invocation that builds the release package. Add `share/qe_database.json` to the file list. Concretely:

```bash
tar czf "${PACKAGE}.tar.gz" \
    -C "${BUILD_DIR}/src/module" "NukeX-pxm.so" "NukeX-pxm.xsgn" \
    -C "${REPO_ROOT}" "share/qe_database.json"
```

(Adjust path arguments to match the existing release-script structure — read the script first.)

- [ ] **Step 5: Verify build still works (no test added — this is a data file)**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10 && ls -la ../share/qe_database.json`
Expected: clean build; the JSON file is present and >50KB.

- [ ] **Step 6: Commit**

```bash
git add share/qe_database.json CMakeLists.txt tools/release.sh
git commit -m "$(cat <<'EOF'
feat(share): ship qe_database.json (55 cameras, 87 filters)

Generated from research/qe_database_research.json via
tools/import_qe_research.py. Sensor-inheritance resolved; Gr/Gb
collapsed to G mean; validated against schema. CMake install rule
deposits it into the module's share/ directory; release script
bundles it into the tarball alongside the .so + .xsgn.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 17: Document `qe_overrides.json` format

**Files:**
- Create: `docs/qe_overrides_format.md`

User-facing reference for the override file at `~/.nukex4/qe_overrides.json`.

- [ ] **Step 1: Write the doc**

Create `docs/qe_overrides_format.md`:

```markdown
# QE Override File Format

NukeX v5 ships a quantum-efficiency database covering 55 cameras and 87 filters at `share/qe_database.json`. To add custom cameras, filters, or override shipped values, create `~/.nukex4/qe_overrides.json` (or any other path you select via the QE override file picker in the NukeX interface).

## Schema

```json
{
  "schema_version": 1,
  "cameras": {
    "<camera-instrume-name>": {
      "sensor": "<sensor-model>",
      "type":   "OSC | mono | both-variants",
      "bayer":  "RGGB | BGGR | GRBG | GBRG",
      "qe": {
        "<wavelength_nm>": { "R": 0..1, "G": 0..1, "B": 0..1 }
      },
      "confidence": "high | medium | low"
    }
  },
  "filters": {
    "<filter-name>": {
      "type": "DUAL_NB | NARROWBAND | BROADBAND",
      "lines": [
        { "name": "Ha",   "wavelength_nm": 656.3, "fwhm_nm": 7.0 }
      ]
    }
  }
}
```

## Override semantics

- Top-level keys (`cameras`, `filters`) merge with the shipped DB.
- A camera or filter entry in the override **replaces** the entire shipped entry on key collision (camera name or filter name).
- New entries (cameras/filters not in the shipped DB) are added.
- An override file with `cameras: {}` and `filters: {}` is a valid no-op.

## When you need an override

| Situation | Add to override |
|---|---|
| Your camera's `INSTRUME` is missing from the shipped DB | a camera entry with measured QE values |
| You measured your own filter passband | a filter entry with the correct line wavelengths |
| Shipped QE is "low" or "medium" confidence and you want better numbers | a replacement camera entry with `confidence: "high"` |

## Validation

NukeX loads the override at batch start. Malformed JSON fails loud with a parser line/col message. Missing required fields fail loud with the camera/filter name.
```

- [ ] **Step 2: Commit**

```bash
git add docs/qe_overrides_format.md
git commit -m "$(cat <<'EOF'
docs: QE override file format reference

User-facing schema for ~/.nukex4/qe_overrides.json. Covers required
fields, override semantics (entry-level replacement on collision),
and the situations that warrant an override.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 6: Cleanup

### Task 18: Delete `StackingMode` enum + `output_rgb_mapping` field

**Files:**
- Modify: `src/lib/core/include/nukex/core/channel_config.hpp`
- Modify: `src/lib/core/src/channel_config.cpp`
- Modify: any remaining callers of `ChannelConfig::from_mode` (should be zero after Tasks 9-12)
- Test: `test/unit/core/test_channel_config.cpp` (existing — may need updates)

`from_filter()` (Task 7) and the new Phase A Router (Task 9) made `from_mode()` and the `OSC_HAO3`/`OSC_S2O3` switch cases unused. Remove them now per the no-dead-code rule.

- [ ] **Step 1: Verify zero remaining callers of `from_mode` and `output_rgb_mapping`**

Run:

```bash
grep -rn "from_mode\|output_rgb_mapping\|StackingMode" src/ test/ 2>&1 | grep -v "^Binary file"
```

Expected: empty output, OR only matches inside `channel_config.hpp/.cpp` (the symbols being deleted) and tests that should be removed/updated.

If any production-code callers remain (other than the target file), STOP and fix them before deleting. They likely indicate a missed refactor in Tasks 9-14.

- [ ] **Step 2: Delete `StackingMode` enum + `from_mode` + `output_rgb_mapping`**

Edit `src/lib/core/include/nukex/core/channel_config.hpp`. Remove:
- The `enum class StackingMode` declaration (lines 9-16 in current file)
- The `StackingMode mode` field in `ChannelConfig`
- The `output_rgb_mapping[3]` field
- The `static ChannelConfig from_mode(StackingMode mode)` declaration

Edit `src/lib/core/src/channel_config.cpp`. Remove the entire `ChannelConfig::from_mode` function definition (with all its switch cases).

- [ ] **Step 3: Update existing tests that referenced the removed symbols**

Run: `grep -rn "from_mode\|StackingMode\|output_rgb_mapping" test/ 2>&1 | grep -v "^Binary"`

For each match, decide:
- If the test was specifically testing the dead path → delete it
- If the test was testing something else and just used `from_mode()` as setup → migrate to `from_filter()`

- [ ] **Step 4: Run all tests + module build**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20 && ctest --output-on-failure 2>&1 | tail -10`
Expected: clean build, all unit tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/lib/core/include/nukex/core/channel_config.hpp \
        src/lib/core/src/channel_config.cpp \
        test/unit/core/
git commit -m "$(cat <<'EOF'
chore(core): remove dead StackingMode enum + output_rgb_mapping

After Tasks 9-14 migrated all callers to ChannelConfig::from_filter,
these symbols had zero references. Spec §4.3 dead-code list closeout.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 19: Delete `src/module/filter_classifier.{hpp,cpp}` (migrated to lib/io/)

**Files:**
- Delete: `src/module/filter_classifier.hpp`
- Delete: `src/module/filter_classifier.cpp`
- Modify: `src/module/CMakeLists.txt` (remove from source list)
- Modify: any remaining `#include "filter_classifier.hpp"` (replace with `nukex/io/filter_classifier.hpp`)

- [ ] **Step 1: Verify zero remaining `#include "filter_classifier.hpp"` (the local module-relative one)**

Run:

```bash
grep -rn '#include "filter_classifier.hpp"' src/ test/
```

Expected: empty (Task 14 should have moved them all to `#include "nukex/io/filter_classifier.hpp"`). If any remain, fix them before deleting.

- [ ] **Step 2: Delete the files**

Run:

```bash
git rm src/module/filter_classifier.hpp src/module/filter_classifier.cpp
```

- [ ] **Step 3: Remove from `src/module/CMakeLists.txt`**

Open `src/module/CMakeLists.txt` and remove the `filter_classifier.cpp` line from the module's source list.

- [ ] **Step 4: Build + test**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | tail -20 && ctest --output-on-failure 2>&1 | tail -10`
Expected: clean build, all unit tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/module/CMakeLists.txt
git commit -m "$(cat <<'EOF'
chore(module): delete filter_classifier; migrated to src/lib/io/

Lives now in src/lib/io/filter_classifier with 5-class taxonomy.
Spec §4.3 dead-code list closeout.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 7: Tests + Validation

### Task 20: Mini-FITS test util — synthetic-frame writer for integration tests

**Files:**
- Create: `test/util/mini_fits_writer.hpp`
- Create: `test/util/mini_fits_writer.cpp`
- Modify: `test/util/CMakeLists.txt` (add new sources to `test_util` library)

The `[.integration]`-tagged tests in Tasks 9-11 reference `test_util::write_synthetic_bayer`, `write_synthetic_mono`, `write_synthetic_q_solved_hao3`, and `write_synthetic_q_solved_s2o3`. This task implements them.

The writer creates a single FITS frame with:
- Configurable W × H
- Configurable BAYERPAT, INSTRUME, FILTER keywords
- Either uniform pixel value (broadband test cases) or Q-matrix-back-derived RGB pixel values (Q-solve round-trip test cases)

- [ ] **Step 1: Write the failing tests for the util itself**

Append to `test/unit/io/test_filter.cpp` (or create `test/util/test_mini_fits_writer.cpp`):

```cpp
#include "catch_amalgamated.hpp"
#include "test_util/mini_fits_writer.hpp"
#include "nukex/io/fits_reader.hpp"

#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("MiniFITSWriter: bayer frame round-trips through FITSReader",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_bayer.fits";
    test_util::write_synthetic_bayer(p.string(), 32, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    auto r = nukex::FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.image.width()  == 32);
    REQUIRE(r.image.height() == 16);
    REQUIRE(r.metadata.bayer_pattern == "RGGB");
    REQUIRE(r.metadata.instrument    == "ASI585MC");
    REQUIRE(r.metadata.filter        == "HaO3");
}

TEST_CASE("MiniFITSWriter: q-solved HaO3 frame produces R/G/B that recover Ha+OIII",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_qsolve.fits";
    test_util::write_synthetic_q_solved_hao3(p.string(), 16, 16, "ASI585MC", 0.5f, 0.3f);

    auto r = nukex::FITSReader::read(p.string());
    REQUIRE(r.success);
    // The writer should have computed (R, G, B) such that the Q-solve recovers
    // Ha=0.5, OIII=0.3 — the integration tests in Task 10 verify the round-trip.
}
```

Add the test target to whatever `test/unit/io/` or `test/util/` CMakeLists.txt is appropriate — link against `test_util` and `nukex4_io`.

- [ ] **Step 2: Implement the writer header**

Create `test/util/mini_fits_writer.hpp`:

```cpp
#ifndef NUKEX_TEST_UTIL_MINI_FITS_WRITER_HPP
#define NUKEX_TEST_UTIL_MINI_FITS_WRITER_HPP

#include <string>

namespace test_util {

void write_synthetic_bayer(const std::string& path,
                           int w, int h,
                           const std::string& bayer,
                           const std::string& instrument,
                           const std::string& filter,
                           float uniform_value);

void write_synthetic_mono(const std::string& path,
                          int w, int h,
                          const std::string& instrument,
                          const std::string& filter,
                          float uniform_value);

// Builds R/G/B Bayer values such that Q-solve with the given camera's
// HaO3 Q matrix recovers (ha, oiii) at every pixel.
void write_synthetic_q_solved_hao3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float ha, float oiii);

void write_synthetic_q_solved_s2o3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float sii, float oiii);

} // namespace test_util

#endif
```

- [ ] **Step 3: Implement the writer**

Create `test/util/mini_fits_writer.cpp`:

```cpp
#include "test_util/mini_fits_writer.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"

#include <fitsio.h>
#include <Eigen/Dense>

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace test_util {

namespace {

void write_fits_with_metadata(const std::string& path,
                              int w, int h,
                              const std::vector<float>& pixels,
                              const std::string& bayer,
                              const std::string& instrument,
                              const std::string& filter) {
    fitsfile* fp = nullptr;
    int status = 0;
    std::remove(path.c_str()); // overwrite if present
    fits_create_file(&fp, path.c_str(), &status);
    long naxes[2] = {w, h};
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, w * h,
                   const_cast<float*>(pixels.data()), &status);
    if (!bayer.empty())
        fits_update_key_str(fp, "BAYERPAT", bayer.c_str(), nullptr, &status);
    if (!instrument.empty())
        fits_update_key_str(fp, "INSTRUME", instrument.c_str(), nullptr, &status);
    if (!filter.empty())
        fits_update_key_str(fp, "FILTER", filter.c_str(), nullptr, &status);
    fits_close_file(fp, &status);
    if (status != 0) {
        char msg[FLEN_ERRMSG];
        fits_get_errstatus(status, msg);
        throw std::runtime_error(std::string("MiniFITSWriter: ") + msg);
    }
}

// Lay out (R, G, B) into a single-channel Bayer pattern for the given pattern.
std::vector<float> bayerize(int w, int h, const std::string& pattern,
                            float r, float g, float b) {
    std::vector<float> out(w * h, 0.0f);
    auto put = [&](int x, int y, float v) {
        if (x >= 0 && x < w && y >= 0 && y < h) out[y * w + x] = v;
    };
    auto site = [&](char c) {
        switch (c) {
            case 'R': return r;
            case 'G': return g;
            case 'B': return b;
        }
        return 0.0f;
    };
    if (pattern.size() != 4) return out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int phase_x = x % 2, phase_y = y % 2;
            char c = pattern[phase_y * 2 + phase_x]; // RGGB top-left, etc.
            put(x, y, site(c));
        }
    }
    return out;
}

nukex::QEDatabase load_test_db() {
    nukex::QEDatabase db;
    auto r = db.load_shipped(std::string(NUKEX_TEST_FIXTURES_DIR) + "/qe/minimal_db.json");
    if (!r.ok) throw std::runtime_error(std::string("MiniFITSWriter: ") + r.error);
    return db;
}

} // namespace

void write_synthetic_bayer(const std::string& path,
                           int w, int h,
                           const std::string& bayer,
                           const std::string& instrument,
                           const std::string& filter,
                           float uniform_value) {
    auto pixels = bayerize(w, h, bayer, uniform_value, uniform_value, uniform_value);
    write_fits_with_metadata(path, w, h, pixels, bayer, instrument, filter);
}

void write_synthetic_mono(const std::string& path,
                          int w, int h,
                          const std::string& instrument,
                          const std::string& filter,
                          float uniform_value) {
    std::vector<float> pixels(w * h, uniform_value);
    write_fits_with_metadata(path, w, h, pixels, /*bayer*/"", instrument, filter);
}

static void write_qsolved(const std::string& path,
                          int w, int h,
                          const std::string& camera,
                          const std::string& filter,
                          double line1, double line2) {
    auto db = load_test_db();
    nukex::ChannelDecomposer dec(db);
    auto Q = dec.build_q(camera, filter); // 3×2

    Eigen::Vector2d truth(line1, line2);
    Eigen::Vector3d rgb = Q * truth;

    auto pixels = bayerize(w, h, "RGGB",
                           static_cast<float>(rgb(0)),
                           static_cast<float>(rgb(1)),
                           static_cast<float>(rgb(2)));
    write_fits_with_metadata(path, w, h, pixels, "RGGB", camera, filter);
}

void write_synthetic_q_solved_hao3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float ha, float oiii) {
    write_qsolved(path, w, h, camera, "HaO3", ha, oiii);
}

void write_synthetic_q_solved_s2o3(const std::string& path,
                                   int w, int h,
                                   const std::string& camera,
                                   float sii, float oiii) {
    write_qsolved(path, w, h, camera, "S2O3", sii, oiii);
}

} // namespace test_util
```

- [ ] **Step 4: Wire into `test/util/CMakeLists.txt`**

Add `mini_fits_writer.cpp` to the existing `test_util` library source list. Link `test_util` against `nukex4_calibration` and the cfitsio target:

```cmake
target_link_libraries(test_util
    PUBLIC nukex4_calibration nukex4_io nukex4_core
    PRIVATE cfitsio
)
target_compile_definitions(test_util PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")
```

- [ ] **Step 5: Build + run all tests including the previously-gated [.integration] ones**

Run:

```bash
cd build && cmake .. && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure 2>&1 | tail -20
ctest --output-on-failure -L integration 2>&1 | tail -20
# OR explicitly run the integration tests via Catch's [.integration] tag:
./test/integration/test_phase_a_router        "[.integration]"
./test/integration/test_phase_b_qsolve        "[.integration]"
./test/integration/test_color_composer_hookup "[.integration]"
```

Expected: all tests pass, including the 8 integration tests that were previously gated.

- [ ] **Step 6: Commit**

```bash
git add test/util/mini_fits_writer.hpp test/util/mini_fits_writer.cpp test/util/CMakeLists.txt
git commit -m "$(cat <<'EOF'
test(util): mini-FITS writer for color-science integration tests

Synthesises FITS frames with arbitrary BAYERPAT/INSTRUME/FILTER
metadata. Two flavours of synthesised pixels: uniform value (for
broadband tests) and Q-matrix-back-derived RGB (for Q-solve round-
trip tests). Unblocks the [.integration]-tagged tests added in
Tasks 9-11 — Phase A routing, Phase B Q-solve, ColorComposer
hookup all now run end-to-end.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 21: E2E manifest update — new baselines + bit-identical preservation

**Files:**
- Modify: `test/fixtures/e2e_manifest.json`
- Modify: `tools/validate_e2e.js` (if needed for new case schema)
- Run: `tools/run_e2e.sh regen` to refresh baselines

Per spec §7.3:

| Case | Status |
|---|---|
| `sweep_ghs`, `sweep_mtf`, `sweep_arcsinh` | **Bit-identical preserve** (stretch math is unchanged) |
| `mono_lrgb_ngc7635_v5` | New baseline (LRGB-mono with composer; replaces v4.0.1.0 hash) |
| `bayer_rgb_ngc7635_v5` | New baseline (plain OSC w/ L synth) |
| `bayer_nb_hao3_m27` | **New** — the M27 motivating case |
| `bayer_nb_s2o3_target` | **New** (S2O3 dual-NB) |
| `lrgbsho_target` | **New** (mixed L+R+G+B+HaO3) |

- [ ] **Step 1: Read the existing manifest**

Run: `cat test/fixtures/e2e_manifest.json`

- [ ] **Step 2: Update the manifest**

Edit `test/fixtures/e2e_manifest.json`. Change the existing cases and add the new ones:

```json
{
  "schema_version": 2,
  "description": "v5 color-science overhaul + preserved stretch sweeps",
  "cases": [
    {
      "name": "sweep_ghs",
      "type": "stretch_sweep",
      "stretch": "GHS",
      "preserve_bit_identical": true
    },
    {
      "name": "sweep_mtf",
      "type": "stretch_sweep",
      "stretch": "MTF",
      "preserve_bit_identical": true
    },
    {
      "name": "sweep_arcsinh",
      "type": "stretch_sweep",
      "stretch": "ArcSinh",
      "preserve_bit_identical": true
    },
    {
      "name": "mono_lrgb_ngc7635_v5",
      "filter_class_expected": "BROADBAND_L",
      "light_dir": "/mnt/qnap/astro_data/NGC7635/L/Lights",
      "flat_dir":  "",
      "primary_stretch": 0,
      "min_frames_ok_alignment": 65,
      "rebaseline_v5": true
    },
    {
      "name": "bayer_rgb_ngc7635_v5",
      "filter_class_expected": "BROADBAND_OSC",
      "light_dir": "/mnt/qnap/astro_data/NGC7635/OSC/Lights",
      "flat_dir":  "",
      "primary_stretch": 0,
      "min_frames_ok_alignment": 30,
      "rebaseline_v5": true
    },
    {
      "name": "bayer_nb_hao3_m27",
      "filter_class_expected": "DUAL_NB_OSC",
      "light_dir": "/mnt/qnap/astro_data/M27/HaO3/Lights",
      "flat_dir":  "",
      "primary_stretch": 0,
      "min_frames_ok_alignment": 20,
      "rebaseline_v5": true,
      "visual_bar": "no green cast; calibrated red+cyan distribution"
    },
    {
      "name": "bayer_nb_s2o3_target",
      "filter_class_expected": "DUAL_NB_OSC",
      "light_dir": "/mnt/qnap/astro_data/SH2-101/S2O3/Lights",
      "flat_dir":  "",
      "primary_stretch": 0,
      "min_frames_ok_alignment": 20,
      "rebaseline_v5": true,
      "skip": true,
      "skip_reason": "no S2O3 corpus configured yet"
    },
    {
      "name": "lrgbsho_target",
      "filter_class_expected": "MIXED",
      "light_dir": "/mnt/qnap/astro_data/M27/LRGBSHO/Lights",
      "flat_dir":  "",
      "primary_stretch": 0,
      "min_frames_ok_alignment": 50,
      "rebaseline_v5": true,
      "skip": true,
      "skip_reason": "no LRGBSHO corpus configured yet"
    }
  ],
  "phaseB_baseline_file": "phaseB_baseline_ms.txt",
  "phaseB_speedup_min": 1.5,
  "output_root": "/tmp/nukex_e2e",
  "golden_dir": "test/fixtures/golden"
}
```

(Adjust `light_dir` paths to whatever your corpus actually contains. Use `skip: true` for cases with no corpus.)

- [ ] **Step 3: Adjust `tools/validate_e2e.js` to honour `rebaseline_v5` and `preserve_bit_identical`**

Read `tools/validate_e2e.js` and find where the per-case loop reads each manifest entry. Add a branch:

- `preserve_bit_identical: true` → existing behaviour (compare hash to golden, fail loud on mismatch).
- `rebaseline_v5: true` AND no golden present → write the new golden.
- `rebaseline_v5: true` AND golden present → compare; mismatch is a regression.

Do NOT change the comparison code path for `preserve_bit_identical` cases — that's the regression floor.

- [ ] **Step 4: Run `make e2e` to verify the bit-identical sweeps still pass**

Run: `cd build && make e2e 2>&1 | tail -30`
Expected: `sweep_ghs`, `sweep_mtf`, `sweep_arcsinh` all pass with bit-identical hash match. The new cases either skip (no corpus) or write fresh goldens.

- [ ] **Step 5: Commit**

```bash
git add test/fixtures/e2e_manifest.json tools/validate_e2e.js
git commit -m "$(cat <<'EOF'
test(e2e): v5 manifest with preserved sweeps + new color-science baselines

sweep_ghs/mtf/arcsinh stay bit-identical (stretch math unchanged).
New baselines: mono_lrgb_ngc7635_v5, bayer_rgb_ngc7635_v5,
bayer_nb_hao3_m27. S2O3 + LRGBSHO scaffolds added but skip-flagged
until corpus is captured.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 22: Real-data validation runs (M27 HaO3 + NGC7635 LRGB-mono)

**Files:**
- Create: `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/M27_HaO3_v4.0.1.0_baseline.png`
- Create: `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/M27_HaO3_v5.0.0.0.png`
- Create: `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/NGC7635_LRGB_v5.0.0.0.png`
- Update: memory `project_color_science_brainstorm.md` with results

This is a manual validation step — runs in PixInsight against real data.

- [ ] **Step 1: Build + sign + install dev module locally**

Per the user's CLAUDE.md release workflow (steps 1-7), build a dev module:

```bash
cd build && make -j$(nproc)
tools/release.sh sign
# DO NOT run `make install` — install via PI's Resource Manager from the local build for testing
```

Then, in PixInsight: PI → Process → Module → Install Module… → point at `build/src/module/NukeX-pxm.so`.

- [ ] **Step 2: Run the M27 HaO3 stack**

In PixInsight, open the NukeX process. Add the M27 HaO3 light frames from `/mnt/qnap/astro_data/M27/HaO3/Lights/`. Stretch: Auto (let auto-selector choose). Run.

- [ ] **Step 3: Visual review — confirm no green cast**

Compare the v5 output against the v4.0.1.0 result captured in memory `project_m27_first_stretch_feedback`.

Visual bar (per spec §7.4):
- No green cast in line-emitting nebula regions
- Hα regions render red, OIII regions render cyan-blue
- Stars stay close-to-natural color (because the broadband chrominance dominates where line signal is low)

Capture before/after PNG screenshots into:
`docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/`

- [ ] **Step 4: Run the NGC7635 LRGB-mono stack**

Same workflow with the LRGB-mono corpus from `/mnt/qnap/astro_data/NGC7635/L/Lights/`. Visual bar: detail preserved compared to v4.0.1.0; color natural.

- [ ] **Step 5: Update the memory**

Edit `~/.claude/projects/-home-scarter4work-projects-nukex4/memory/project_color_science_brainstorm.md` to add a "Real-data validation" section noting:
- v5 M27 result (no green cast / still slightly green / wrong-color)
- v5 NGC7635 result (detail OK / detail loss / color natural)
- Any palette tuning needed (a*/b* values to revisit before release)

If the visual bar is NOT met, branch back to ColorComposer (Task 6) and tune the palette vectors before proceeding.

- [ ] **Step 6: Commit the visual evidence**

```bash
git add docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/
git commit -m "$(cat <<'EOF'
docs(visual-evidence): M27 HaO3 + NGC7635 LRGB v5 vs v4 comparisons

Capture validating the calibrated palette has no green cast in
dual-NB output and broadband detail/color is preserved.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 8: Release

### Task 23: Version bump + CHANGELOG + release packaging

**Files:**
- Modify: `src/module/NukeXVersion.h` (4.0.1.0 → 5.0.0.0)
- Modify: `CHANGELOG.md`
- Run: `tools/release.sh package`
- Modify: `repository/updates.xri`
- Commit: version bump + package

- [ ] **Step 1: Bump the version**

Edit `src/module/NukeXVersion.h`:

```cpp
#define NUKEX_MODULE_VERSION_MAJOR     5
#define NUKEX_MODULE_VERSION_MINOR     0
#define NUKEX_MODULE_VERSION_REVISION  0
#define NUKEX_MODULE_VERSION_BUILD     0

#define NUKEX_MODULE_RELEASE_YEAR      2026
#define NUKEX_MODULE_RELEASE_MONTH     <month-of-release>
#define NUKEX_MODULE_RELEASE_DAY       <day-of-release>
```

Replace `<month>`/`<day>` with the actual release date.

- [ ] **Step 2: Update CHANGELOG.md**

Add at the top of `CHANGELOG.md`:

```markdown
## v5.0.0.0 — Color Science Overhaul (2026-MM-DD)

### Added
- Filter taxonomy (5 classes: BROADBAND_L, BROADBAND_RGB, BROADBAND_OSC, NARROWBAND_SINGLE, DUAL_NB_OSC)
- QE database covering 55 cameras and 87 filters (`share/qe_database.json`)
- ChannelDecomposer with per-(camera, filter) Q-matrix solve via Eigen (vendored)
- ColorComposer with calibrated emission-line palette (Lab/LCH default; opt-in continuum-subtract)
- "QE override file…" picker in NukeX interface (loads `~/.nukex4/qe_overrides.json` or any user-selected JSON)
- OSC-as-LRGB synthesis (rec709 luminance synthesised per OSC frame)

### Fixed
- M27 dual-narrowband green-cast bug (root cause: hardcoded `OSC_RGB` routing for all Bayer data; spec §1)

### Changed
- Output ImageWindow is always 3-channel sRGB (from ColorComposer) instead of variable-channel raw
- Phase 8 rating DB schema bumped V2 → V3; pre-v5 dual-NB ratings collapse to NARROWBAND_SINGLE on migration
- E2E baselines refreshed for multi-channel cases; single-channel stretch sweeps preserved bit-identical

### Removed
- `StackingMode` enum (replaced by `Filter` + `FilterClass`)
- `output_rgb_mapping` field on `ChannelConfig` (dead code)
- `src/module/filter_classifier.{hpp,cpp}` (migrated to `src/lib/io/`)
```

- [ ] **Step 3: Build + run full test suite + e2e**

Run:

```bash
cd build && cmake .. && make clean && make -j$(nproc) 2>&1 | tail -10
ctest --output-on-failure 2>&1 | tail -20
make e2e 2>&1 | tail -30
```

Expected: clean build, all tests pass (unit + integration + bit-identical sweeps).

- [ ] **Step 4: Sign + package**

Run:

```bash
tools/release.sh package
```

Expected: produces a signed `.so` + `.xsgn` + `share/qe_database.json` tarball under `repository/`. The `updates.xri` is regenerated and signed.

- [ ] **Step 5: Verify the package contents**

Run:

```bash
ls -la repository/
tar -tzf repository/$(date +%Y%m%d)-*-NukeX.tar.gz | head -20
```

Expected: tarball contains `bin/NukeX-pxm.so`, `bin/NukeX-pxm.xsgn`, and `share/qe_database.json`.

- [ ] **Step 6: Commit version bump + package**

```bash
git add src/module/NukeXVersion.h CHANGELOG.md repository/
git commit -m "$(cat <<'EOF'
release: v5.0.0.0 — color science overhaul

Filter taxonomy + QE-driven Q-solve + Lab/LCH ColorComposer with
calibrated emission-line palette. Fixes M27 dual-narrowband green
cast (root cause: stacking_engine.cpp:107 hardcoded OSC_RGB for
all Bayer). Adds OSC-as-LRGB synthesis. 55 cameras / 87 filters
shipped in share/qe_database.json. Rating DB schema V2 -> V3.

See CHANGELOG.md for the full diff.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push
```

---

### Task 24: Memory updates + brainstorm checklist closure

**Files:**
- Modify: `~/.claude/projects/-home-scarter4work-projects-nukex4/memory/MEMORY.md`
- Create: `~/.claude/projects/-home-scarter4work-projects-nukex4/memory/project_v5000_closeout.md`
- Modify: `~/.claude/projects/-home-scarter4work-projects-nukex4/memory/project_color_science_brainstorm.md` (mark checklist complete)

- [ ] **Step 1: Write the closeout memory**

Create `memory/project_v5000_closeout.md`:

```markdown
---
name: v5.0.0.0 Color Science Overhaul Closeout
description: 2026-MM-DD shipped. Filter taxonomy + QE Q-solve + Lab/LCH ColorComposer with calibrated emission-line palette. Fixed M27 green cast. 55-camera/87-filter QE DB shipped.
type: project
---

**Status 2026-MM-DD evening:** v5.0.0.0 SHIPPED.

## What changed

- Replaced `stacking_engine.cpp:107` hardcoded OSC_RGB with FilterClassifier-driven Phase A router
- Added Phase B Q-matrix solve via Eigen (3.4 vendored via FetchContent)
- Added ColorComposer with Lab/LCH composite + opt-in continuum-subtract; calibrated palette has no greens by construction
- Shipped `share/qe_database.json` with 55 cameras, 87 filters; user override at `~/.nukex4/qe_overrides.json`
- Removed `StackingMode` enum, `output_rgb_mapping` field, `src/module/filter_classifier.*`
- Rating DB V2 → V3 with explicit code remap

## Real-data validation

- M27 HaO3: <fill in result>
- NGC7635 LRGB-mono: <fill in result>
- LRGBSHO: <fill in if corpus available>

## Memories closed

- `project_color_science_brainstorm.md` — brainstorming + spec checklist complete
```

- [ ] **Step 2: Update `MEMORY.md` index**

Add a line to `~/.claude/projects/-home-scarter4work-projects-nukex4/memory/MEMORY.md`:

```markdown
- [project_v5000_closeout.md](project_v5000_closeout.md) — v5.0.0.0 SHIPPED 2026-MM-DD. Color science overhaul: filter taxonomy + QE Q-solve + Lab/LCH composer. M27 green-cast fixed.
```

- [ ] **Step 3: Mark the brainstorm checklist complete**

Edit `~/.claude/projects/-home-scarter4work-projects-nukex4/memory/project_color_science_brainstorm.md` and check the remaining boxes:

```markdown
- [x] 8. User reviews written spec
- [x] 9. Transition to implementation (invoked superpowers:writing-plans)
- [x] 10. Plan executed → v5.0.0.0 shipped
```

- [ ] **Step 4: No commit needed (memory files are local-only)**

---

## Self-Review

Spec coverage check:

| Spec section | Implementation task |
|---|---|
| §3.1 four pillars: Filter type | Task 1 |
| §3.1 four pillars: DebayerEngine naïve | unchanged (verified in Task 9) |
| §3.1 four pillars: voxel raw slots | Task 7 (slot_index + merge) |
| §3.1 four pillars: ColorComposer is sole palette consumer | Task 6 |
| §4.1 new code: Filter, FilterClassifier | Tasks 1, 2 |
| §4.1 new code: QEDatabase | Task 3 |
| §4.1 new code: ChannelDecomposer | Task 4 |
| §4.1 new code: Palette | Task 5 |
| §4.1 new code: ColorComposer | Task 6 |
| §4.1 new code: share/qe_database.json | Task 16 |
| §4.1 new code: tools/import_qe_research.py | Task 15 |
| §4.2 refactored: channel_config | Tasks 7, 18 |
| §4.2 refactored: cube/SubcubeVoxel | Task 7 + Task 10 (selected_value field) |
| §4.2 refactored: stacking_engine.cpp | Tasks 8, 9, 10, 11 |
| §4.2 refactored: NukeXInstance | Task 12 |
| §4.2 refactored: NukeXInterface (QE override picker) | Task 13 |
| §4.3 deletions: output_rgb_mapping, StackingMode, OSC_HAO3/S2O3 cases | Task 18 |
| §4.3 deletions: src/module/filter_classifier | Task 19 |
| §5.1 Phase A streaming I/O routing | Task 9 |
| §5.2 Phase B Q-solve + multi-source OIII merge | Task 10 |
| §5.3 ColorComposer Lab/LCH default + continuum-subtract opt-in | Task 6 (lib) + Task 11 (hookup) |
| §6.1 startup config errors (missing DB, malformed override, k_continuum missing) | Task 3 (DB load), Task 6 (composer continuum) |
| §6.2 per-frame errors (BAYERPAT mismatch, multi-camera) | unchanged from existing engine; verified in Task 9 |
| §6.3 filter/camera resolution tiered policy | Task 2 (classifier), Task 9 (router branch for unknown-on-Bayer) |
| §6.4 Phase B math edges (singular Q, negative clamp, gamut clip) | Tasks 4, 6, 10 |
| §6.5 observability per-frame log + Phase B summary | Task 9 (mid-batch warn), Task 12 (FITS keyword annotation); Phase B summary log to be added in Task 10's progress messages |
| §7.1 unit tests (boundaries) | Tasks 1-6 each ship their unit tests |
| §7.2 integration tests | Tasks 9-11 + Task 20 (test util) |
| §7.3 E2E new + preserved baselines | Task 21 |
| §7.4 real-data validation | Task 22 |
| §7.5 performance regression budget | run during Task 23 e2e check |

**Placeholder scan:** No "TBD", "TODO", or "implement later" appear in any task body. All code blocks contain compileable code (modulo accessor name verification noted in Tasks 7, 9, 11). All commands have explicit expected outputs.

**Type consistency check:**
- `Filter` struct → consistent across Tasks 1, 2, 7, 9
- `FilterClass` enum → consistent (5 values + UNKNOWN)
- `ChannelConfig::slot_index()` → consistent (Tasks 7, 9, 10)
- `DerivedSlots` struct → consistent across Tasks 6, 11
- `sRGBPixel` struct → consistent across Tasks 6, 11
- `QEDatabase`, `ChannelDecomposer`, `ColorComposer` constructor signatures → consistent across Tasks 3-12
- `ExecuteResult` fields → grew across Tasks 9-11 (`ok`, `error`, `composed`, `derived`, `gamut_clipped_count`); consistent in each downstream consumer (Task 12)
- `Photosite` enum (R/G/B/MONO_PEAK) → used consistently across Tasks 3, 4

**Gaps identified during self-review:**
- §6.5 "Phase B summary" log line is mentioned in spec but no task produces it. Add to Task 10, Step 3 — at the end of the Q-solve loop, log:
  ```cpp
  if (progress) {
      std::ostringstream oss;
      oss << "Cube has " << cube.channel_config.n_channels << " raw slots, "
          << result.derived.slots.size() << " derived semantic slots\n"
          << "Negative-emission clamp: " << result.derived.negative_clamped_count
          << " pixels";
      progress->message(oss.str().c_str());
  }
  ```
- §6.5 "Composer summary" log line — add to Task 11, Step 4 after the per-pixel composer loop:
  ```cpp
  if (progress) {
      std::ostringstream oss;
      oss << "Output mode: Lab/LCH calibrated palette (default)\n"
          << "Out-of-gamut soft-clipped: " << result.gamut_clipped_count << " pixels";
      progress->message(oss.str().c_str());
  }
  ```

These edits should be applied to the relevant tasks during implementation; they're small additions that don't change the task structure.

---

## Plan complete

24 tasks total. Estimated effort: 2-3 weeks for an engineer working full-time on it; longer if real-data validation in Task 22 surfaces palette tuning needs that loop back to Task 6.

The plan minimises coupling by phasing in this order:
1. **Foundations (Tasks 1-6)** — pure types + libs, fully unit-testable, can be parallelised across multiple agents
2. **Cube/voxel (Task 7)** — small, additive
3. **Pipeline refactor (Tasks 8-11)** — sequential, each depends on the previous
4. **Module + UI (Tasks 12-14)** — sequential, depend on Phase 3
5. **Data + tooling (Tasks 15-17)** — independent, can be done any time after Task 3
6. **Cleanup (Tasks 18-19)** — must come after Tasks 9-14 so call sites are migrated
7. **Tests + validation (Tasks 20-22)** — final verification before release
8. **Release (Tasks 23-24)** — last






