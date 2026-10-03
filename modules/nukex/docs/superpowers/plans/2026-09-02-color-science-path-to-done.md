# Color Science Overhaul — Path to v5.0.0.0 (Tasks 14–24, reconciled)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship NukeX v5.0.0.0 — finish the remaining eleven tasks of the color-science overhaul plus two defects the reconciliation surfaced, and put a signed package in a public PixInsight repository.

**Architecture:** Tasks 1–13 of `2026-04-27-color-science-overhaul.md` are shipped at `0f1b8fe` (66/66 ctest green after a clean rebuild on Fedora 44 / GCC 16). This document *supersedes* that plan's Tasks 14–24. Every task below was re-derived from the code as it stands on 2026-09-02; the April text drifted on file names, APIs, encodings, corpus paths and, in two places, on design (camera lookup and the research→shipped filter mapping). Task numbers are kept so commits and memories keep cross-referencing; inserted fixes are 14b and 15b.

**Tech Stack:** C++17, CMake, Catch2 v3 (amalgamated in `third_party/catch2`), nlohmann/json, system Eigen 3 (`find_package(Eigen3)`), cfitsio (FetchContent), vendored SQLite (`sqlite3_vendored`), PCL, Python 3 + pytest 9 (tools), PJSR (E2E harness).

**Spec:** `docs/superpowers/specs/2026-04-26-color-science-overhaul-design.md`. Original plan: `docs/superpowers/plans/2026-04-27-color-science-overhaul.md` (Tasks 1–13 remain authoritative there).

## Global Constraints

- **Rebuild first.** The machine moved to Fedora 44 / GCC 16.2.1 / glog 0.7.1 after the May build. A stale `build/` fails 17 tests on `libglog.so.0`. `cd build && cmake .. && make clean && make -j$(nproc)` once per checkout; expect `ctest` → `100% tests passed, 0 tests failed out of 66`.
- **Tests register in `test/CMakeLists.txt`** via `nukex_add_test(<name> <path-relative-to-test/> <libs…>)`. There are no per-directory CMakeLists under `test/unit/`. `NUKEX_TEST_FIXTURES_DIR` is defined globally for every test target.
- **Two `nukex::FilterClass` enums coexist until Task 19.** Old: `src/module/filter_classifier.hpp` (4 values). New: `src/lib/core/include/nukex/core/filter.hpp` (`UNKNOWN=0, BROADBAND_L=1, BROADBAND_RGB=2, BROADBAND_OSC=3, NARROWBAND_SINGLE=4, DUAL_NB_OSC=5`). A translation unit may include only one of them.
- **Engine lookup keys.** `FilterClassifier` emits canonical filter names (`HaO3`, `S2O3`, `L-eXtreme`, `L-eNhance`, `L-Ultimate`, `ALP-T`, `Ha`, `OIII`, `SII`, `L`, `R`, `G`, `B`, `OSC`, `L_unnamed`) and `Filter.camera = FITS INSTRUME verbatim`. `QEDatabase::lookup_filter`/`build_q` are keyed by those names. The shipped database must be keyed the same way.
- **No `lenient()`, no stubs, no TODOs, no silent fallbacks.** Loud errors. Root-cause fixes only.
- **Release workflow (CLAUDE.md):** never `make install`; bump `src/module/NukeXVersion.h` + release date; clean build; ctest; `tools/release.sh package`; commit version bump + package together; push. `tools/release.sh` refuses a stale build and needs `/tmp/.pi_codesign_pass`.
- **Commit trailer** on every commit:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
  ```
- **Memory directory** for this project is `~/.claude/projects/-home-scarter4work-projects-nukex5/memory/` (the April plan said nukex4).

## Execution order

| Wave | Tasks | Dependencies | Notes |
|---|---|---|---|
| 1 | 14, 14b, 15, 15b | none | Independent files; run as parallel subagents. 14 is the heaviest. |
| 2 | 16, 17, 18, 19, 20 | 16←15+15b; 17←16; 18,19←14; 20←15b | 18 and 19 touch the same CMake file — run sequentially. |
| 3 | 21, 22 | all of wave 2; PixInsight desktop | 21 installs the dev module; 22 needs human eyes. |
| 4 | 23, 24 | 21, 22 | Release + memory closeout. |

---

## Wave 1

### Task 14: Migrate `lastRun.filter_class` + `stretch_auto_selector` to the 5-class enum; rating DB `user_version` 1 → 2

**Files:**
- Modify: `src/lib/learning/include/nukex/learning/rating_db.hpp`
- Modify: `src/lib/learning/src/rating_db.cpp`
- Create: `test/unit/learning/test_rating_db_migration.cpp`
- Modify: `test/unit/learning/test_rating_db.cpp:35-40` (asserts `user_version == 1`)
- Modify: `test/CMakeLists.txt` (register test; link `nukex4_io` into `nukex4_module_testlib`)
- Modify: `src/module/NukeXInstance.cpp:11,26,78-104,664,706-707`
- Modify: `src/module/stretch_auto_selector.hpp`, `src/module/stretch_auto_selector.cpp`
- Modify: `src/module/stretch_factory.hpp:4,33`, `src/module/stretch_factory.cpp:35,43-47`
- Modify: `src/module/RatingDialog.h:32-34`, `src/module/RatingDialog.cpp:53,74`
- Modify: `test/unit/module/test_stretch_auto_selector.cpp`, `test/unit/module/test_stretch_factory.cpp`

**Interfaces:**
- Consumes: `nukex::FilterClassifier::classify(const FrameMetadata&) -> Filter` (`nukex/io/filter_classifier.hpp`), `nukex::FITSReader::read_headers(path) -> FrameMetadata` (`nukex/io/fits_reader.hpp`), `filter_class_name(FilterClass)` (`nukex/core/filter.hpp`, returns `"BROADBAND_L"` etc.).
- Produces: `constexpr int nukex::learning::kRatingDbSchemaVersion = 2`; `int nukex::learning::rating_db_schema_version(sqlite3*)`; `AutoSelection select_auto(const FrameMetadata&)`; `build_primary(PrimaryStretch, const FrameMetadata&, std::string&, const Phase8Context*)`; rating ints `1..5` per the table below.

**Why the April table was wrong.** The DB does not store raw v4 enum integers. It stores `filter_class_to_rating_int()` output — the rating-axis encoding `0 = LRGB_MONO or LRGB_COLOR (collapsed)`, `1 = BAYER_RGB`, `2 = NARROWBAND`, `3 = reserved S2O3 (never written)`. The April mapping would have moved narrowband rows into the OSC bucket. The user chose this corrected table on 2026-05-01:

| v1 stored code | meaning | v2 code | new enum |
|---|---|---|---|
| 0 | LRGB_MONO / LRGB_COLOR | 1 | BROADBAND_L |
| 1 | BAYER_RGB | 3 | BROADBAND_OSC |
| 2 | NARROWBAND | 4 | NARROWBAND_SINGLE |
| 3 | reserved, never written | 4 | NARROWBAND_SINGLE (defensive) |

New writer encoding (`filter_class_to_rating_int`): `BROADBAND_L=1, BROADBAND_RGB=2, BROADBAND_OSC=3, NARROWBAND_SINGLE=4, DUAL_NB_OSC=5, UNKNOWN=0`. Only `RatingDialog` consumes the value (color axis visibility); the ridge-regression trainer selects by `stretch_name` only.

- [ ] **Step 1: Write the failing migration test**

Create `test/unit/learning/test_rating_db_migration.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/learning/rating_db.hpp"

#include <sqlite3.h>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace nukex::learning;

namespace {

// The exact v4.0.1.0 column order (select_runs_for_stretch reads by index),
// stamped user_version = 1. Written with the raw C API so the open-path
// migration under test cannot touch the setup.
const char* kV1Schema = R"SQL(
CREATE TABLE runs (
    run_id           BLOB PRIMARY KEY,
    created_at       INTEGER NOT NULL,
    stretch_name     TEXT NOT NULL,
    target_class     INTEGER NOT NULL,
    filter_class     INTEGER NOT NULL,
    stat_median_r REAL, stat_median_g REAL, stat_median_b REAL,
    stat_mad_r    REAL, stat_mad_g    REAL, stat_mad_b    REAL,
    stat_p50_r    REAL, stat_p50_g    REAL, stat_p50_b    REAL,
    stat_p95_r    REAL, stat_p95_g    REAL, stat_p95_b    REAL,
    stat_p99_r    REAL, stat_p99_g    REAL, stat_p99_b    REAL,
    stat_p999_r   REAL, stat_p999_g   REAL, stat_p999_b   REAL,
    stat_skew_r   REAL, stat_skew_g   REAL, stat_skew_b   REAL,
    stat_sat_frac_r REAL, stat_sat_frac_g REAL, stat_sat_frac_b REAL,
    stat_bright_concentration REAL,
    stat_color_rg  REAL, stat_color_bg REAL,
    stat_fwhm_median REAL,
    stat_star_count  INTEGER,
    params_json TEXT NOT NULL,
    rating_brightness INTEGER NOT NULL,
    rating_saturation INTEGER NOT NULL,
    rating_color      INTEGER,
    rating_star_bloat INTEGER NOT NULL,
    rating_overall    INTEGER NOT NULL
);
PRAGMA user_version = 1;
)SQL";

void insert_v1_row(sqlite3* db, unsigned char id, int filter_class) {
    const char* sql =
        "INSERT INTO runs(run_id, created_at, stretch_name, target_class, filter_class,"
        " params_json, rating_brightness, rating_saturation, rating_star_bloat, rating_overall)"
        " VALUES(?, 1, 'GHS', 0, ?, '{}', 0, 0, 0, 3);";
    sqlite3_stmt* s = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK);
    unsigned char run_id[16] = {};
    run_id[0] = id;
    sqlite3_bind_blob(s, 1, run_id, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, filter_class);
    REQUIRE(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

fs::path fresh_path(const char* name) {
    auto p = fs::temp_directory_path() / name;
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

} // namespace

TEST_CASE("open_rating_db: v1 filter_class codes are remapped to the 5-class encoding once",
          "[learning][rating_db][migration]") {
    auto path = fresh_path("nukex_rating_v1_migration.sqlite");
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw, kV1Schema, nullptr, nullptr, nullptr) == SQLITE_OK);
        insert_v1_row(raw, 1, 0);   // LRGB_MONO / LRGB_COLOR collapsed
        insert_v1_row(raw, 2, 1);   // BAYER_RGB
        insert_v1_row(raw, 3, 2);   // NARROWBAND
        insert_v1_row(raw, 4, 3);   // reserved S2O3, never written by v4
        sqlite3_close(raw);
    }

    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    REQUIRE(rating_db_schema_version(db) == kRatingDbSchemaVersion);

    auto rows = select_runs_for_stretch(db, "GHS");
    REQUIRE(rows.size() == 4);
    int n_bb_l = 0, n_bb_osc = 0, n_nb_single = 0, n_other = 0;
    for (const auto& r : rows) {
        switch (r.filter_class) {
            case 1:  ++n_bb_l;      break;
            case 3:  ++n_bb_osc;    break;
            case 4:  ++n_nb_single; break;
            default: ++n_other;     break;
        }
    }
    REQUIRE(n_bb_l      == 1);
    REQUIRE(n_bb_osc    == 1);
    REQUIRE(n_nb_single == 2);
    REQUIRE(n_other     == 0);
    close_rating_db(db);

    // A second open must be a no-op: re-running the CASE would move 1 -> 3.
    db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    REQUIRE(rating_db_schema_version(db) == kRatingDbSchemaVersion);
    n_bb_l = 0;
    for (const auto& r : select_runs_for_stretch(db, "GHS")) if (r.filter_class == 1) ++n_bb_l;
    REQUIRE(n_bb_l == 1);
    close_rating_db(db);
}

TEST_CASE("open_rating_db: a fresh DB is stamped with the current schema version",
          "[learning][rating_db][migration]") {
    auto path = fresh_path("nukex_rating_fresh_version.sqlite");
    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    REQUIRE(rating_db_schema_version(db) == kRatingDbSchemaVersion);
    close_rating_db(db);
}
```

Register in `test/CMakeLists.txt` directly under the `test_rating_db` line:

```cmake
nukex_add_test(test_rating_db_migration unit/learning/test_rating_db_migration.cpp nukex4_learning sqlite3_vendored)
```

- [ ] **Step 2: Verify it fails to build**

Run: `cd build && cmake .. > /dev/null && make test_rating_db_migration 2>&1 | grep -E "error" | head -3`
Expected: `error: 'rating_db_schema_version' was not declared` (and `kRatingDbSchemaVersion`).

- [ ] **Step 3: Declare the version API**

In `src/lib/learning/include/nukex/learning/rating_db.hpp`, directly above `sqlite3* open_rating_db(...)`:

```cpp
// Schema version stamped in SQLite `PRAGMA user_version`.
//   1 = v4.0.1.0 layout. filter_class holds the v4 rating-axis codes
//       (0 mono-or-LRGB-color, 1 Bayer RGB, 2 narrowband, 3 reserved).
//   2 = identical layout; filter_class holds the 5-class FilterClass rating
//       ints (1 BROADBAND_L, 2 BROADBAND_RGB, 3 BROADBAND_OSC,
//       4 NARROWBAND_SINGLE, 5 DUAL_NB_OSC, 0 UNKNOWN). open_rating_db()
//       migrates 1 -> 2 in place on first open.
constexpr int kRatingDbSchemaVersion = 2;

// Reads PRAGMA user_version. Returns -1 if the handle cannot answer.
int rating_db_schema_version(sqlite3* db);
```

Update the `open_rating_db` doc comment: replace "Applies schema v1 if the DB is empty." with "Applies the schema if the DB is empty and migrates older user_version stamps forward."

- [ ] **Step 4: Implement stamping + migration in `rating_db.cpp`**

1. Delete the line `PRAGMA user_version = 1;` from `kSchemaV1` (it re-stamped 1 on every open, which would undo the migration).
2. Replace `apply_pragmas_and_schema` with:

```cpp
int read_user_version(sqlite3* db) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &s, nullptr) != SQLITE_OK) return -1;
    int v = -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return v;
}

bool set_user_version(sqlite3* db, int v) {
    const std::string sql = "PRAGMA user_version = " + std::to_string(v) + ";";
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

// v1 rows carry the v4 rating-axis encoding written by the old
// NukeXInstance::filter_class_to_rating_int:
//   0 = LRGB_MONO or LRGB_COLOR (collapsed), 1 = BAYER_RGB,
//   2 = NARROWBAND, 3 = reserved S2O3 (never written).
// v2 rows carry FilterClass rating ints (see rating_db.hpp).
// One CASE expression: sequential `UPDATE … WHERE filter_class = N` would
// chain (a row moved 0 -> 1 would then match the 1 -> 3 rule).
bool migrate_v1_to_v2(sqlite3* db) {
    const char* sql =
        "BEGIN IMMEDIATE;"
        "UPDATE runs SET filter_class = CASE filter_class"
        "   WHEN 0 THEN 1"
        "   WHEN 1 THEN 3"
        "   WHEN 2 THEN 4"
        "   WHEN 3 THEN 4"
        "   ELSE filter_class END;"
        "PRAGMA user_version = 2;"
        "COMMIT;";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

bool apply_pragmas_and_schema(sqlite3* db) {
    char* err = nullptr;
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }
    // A garbage-bytes file fails here with SQLITE_NOTADB -> -1 -> false,
    // which open_rating_db() treats as corruption (rename + retry).
    const int before = read_user_version(db);
    if (before < 0) return false;

    if (sqlite3_exec(db, kSchemaV1, nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }
    if (before == 0) return set_user_version(db, kRatingDbSchemaVersion); // brand-new file
    if (before == 1) return migrate_v1_to_v2(db);
    return true; // already current (or newer: leave untouched)
}
```

3. Add the public accessor after `close_rating_db`:

```cpp
int rating_db_schema_version(sqlite3* db) {
    return db ? read_user_version(db) : -1;
}
```

- [ ] **Step 5: Update the existing schema assertion**

In `test/unit/learning/test_rating_db.cpp` lines 35–40, the "user_version == 1" block: change the expected value to `kRatingDbSchemaVersion` (2) and the comment to `// user_version == kRatingDbSchemaVersion`.

- [ ] **Step 6: Run the learning tests**

Run: `cd build && make test_rating_db test_rating_db_migration test_train_model 2>&1 | grep -E "error|warning: unused" ; ctest -R "rating_db|train_model" --output-on-failure 2>&1 | tail -4`
Expected: `100% tests passed, 0 tests failed out of 3`.

- [ ] **Step 7: Migrate `stretch_auto_selector` to `FrameMetadata` + the lib classifier**

Replace `src/module/stretch_auto_selector.hpp` with:

```cpp
#ifndef __NukeX_stretch_auto_selector_h
#define __NukeX_stretch_auto_selector_h

#include "nukex/core/filter.hpp"
#include "nukex/core/frame_metadata.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include <memory>
#include <string>

namespace nukex {

struct AutoSelection {
    std::unique_ptr<StretchOp> op;
    std::string log_line;
};

/// Primary entry point: classify (lib FilterClassifier) + select + build a
/// rationale log line. `meta` populates the log with the FITS header values
/// that drove the classification (FILTER / BAYERPAT / INSTRUME) so a user
/// can trace "why BROADBAND_L?" in the Process Console.
AutoSelection select_auto(const FrameMetadata& meta);

/// FilterClass-only overload (empty header detail in the log line).
AutoSelection select_auto(FilterClass cls);

} // namespace nukex

#endif
```

Replace `src/module/stretch_auto_selector.cpp` with:

```cpp
#include "stretch_auto_selector.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include <sstream>

namespace nukex {

namespace {

std::unique_ptr<StretchOp> make_champion(FilterClass /*cls*/) {
    return std::make_unique<VeraLuxStretch>();
}

const char* champion_name(FilterClass /*cls*/) {
    return "VeraLux";
}

} // namespace

AutoSelection select_auto(const FrameMetadata& meta) {
    FilterClassifier classifier;
    const Filter f = classifier.classify(meta);

    AutoSelection sel;
    sel.op = make_champion(f.cls);
    std::ostringstream oss;
    oss << "Auto: classified as " << filter_class_name(f.cls)
        << " (FITS FILTER='" << meta.filter
        << "', BAYERPAT='" << meta.bayer_pattern
        << "', INSTRUME='" << meta.instrument
        << "') -> " << champion_name(f.cls);
    if (!classifier.last_warning().empty()) {
        oss << " | " << classifier.last_warning();
    }
    sel.log_line = oss.str();
    return sel;
}

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

- [ ] **Step 8: Migrate `stretch_factory` to `FrameMetadata`**

`src/module/stretch_factory.hpp`: replace `#include "fits_metadata.hpp"` (line 4) with `#include "nukex/core/frame_metadata.hpp"`; change the `build_primary` parameter (line 33) to `const FrameMetadata& meta`.

`src/module/stretch_factory.cpp`: change the parameter (line 35) to `const FrameMetadata& meta`; update the comment at lines 43–46 to read `(FILTER/BAYERPAT/INSTRUME)`.

- [ ] **Step 9: Migrate `NukeXInstance.cpp`**

1. Line 11: `#include "filter_classifier.hpp"` → `#include "nukex/io/filter_classifier.hpp"`.
2. Line 26: `#include "fits_metadata.hpp"` → `#include "nukex/io/fits_reader.hpp"`.
3. Replace lines 78–104 (comment block + `filter_class_to_rating_int`) with:

```cpp
// Phase 8 rating-DB filter-class encoding (rating_db.hpp schema v2).
//
// RatingDialog shows the color-balance axis only for classes whose output
// carries broadband chrominance: BROADBAND_RGB (2) and BROADBAND_OSC (3).
// Luminance, single-line narrowband and dual-NB composites hide it.
int filter_class_to_rating_int( nukex::FilterClass fc )
{
   switch ( fc )
   {
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

4. Line 664: `nukex::FITSMetadata meta = nukex::read_fits_metadata( light_paths.front() );` → `nukex::FrameMetadata meta = nukex::FITSReader::read_headers( light_paths.front() );`
5. Lines 706–707: replace with

```cpp
         {
            nukex::FilterClassifier classifier;
            lastRun.filter_class = filter_class_to_rating_int( classifier.classify( meta ).cls );
         }
```

- [ ] **Step 10: Update `RatingDialog` color-axis rule**

`src/module/RatingDialog.h` lines 32–34 → 

```cpp
    // filter_class: rating-DB schema v2 ints (1 BROADBAND_L, 2 BROADBAND_RGB,
    // 3 BROADBAND_OSC, 4 NARROWBAND_SINGLE, 5 DUAL_NB_OSC, 0 UNKNOWN).
    // The color axis is shown only when has_color_axis(filter_class).
    RatingDialog(int filter_class);
    static bool has_color_axis(int filter_class) { return filter_class == 2 || filter_class == 3; }
```

`src/module/RatingDialog.cpp` line 53: `if (filter_class_ == 1 /* Bayer_RGB */)` → `if (has_color_axis(filter_class_))`; line 74 likewise.

- [ ] **Step 11: Migrate the module tests**

Replace `test/unit/module/test_stretch_auto_selector.cpp` with:

```cpp
#include "catch_amalgamated.hpp"
#include "stretch_auto_selector.hpp"
#include "nukex/core/frame_metadata.hpp"
#include "nukex/stretch/veralux_stretch.hpp"

using namespace nukex;

static FrameMetadata meta_of(const std::string& filter, const std::string& bayer,
                             const std::string& instrument) {
    FrameMetadata m;
    m.filter        = filter;
    m.bayer_pattern = bayer;
    m.instrument    = instrument;
    return m;
}

TEST_CASE("select_auto: BROADBAND_L picks VeraLux and logs the class name", "[module][auto_selector]") {
    auto sel = select_auto(FilterClass::BROADBAND_L);
    REQUIRE(sel.op != nullptr);
    REQUIRE(dynamic_cast<VeraLuxStretch*>(sel.op.get()) != nullptr);
    REQUIRE(sel.log_line.find("BROADBAND_L") != std::string::npos);
    REQUIRE(sel.log_line.find("VeraLux")     != std::string::npos);
}

TEST_CASE("select_auto: every class produces a non-null op + non-empty log", "[module][auto_selector]") {
    for (FilterClass c : {FilterClass::UNKNOWN, FilterClass::BROADBAND_L, FilterClass::BROADBAND_RGB,
                          FilterClass::BROADBAND_OSC, FilterClass::NARROWBAND_SINGLE,
                          FilterClass::DUAL_NB_OSC}) {
        auto sel = select_auto(c);
        REQUIRE(sel.op != nullptr);
        REQUIRE(!sel.log_line.empty());
    }
}

TEST_CASE("select_auto(meta): log line carries FILTER/BAYERPAT/INSTRUME + class", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("L", "", "ASI2600MM"));
    REQUIRE(dynamic_cast<VeraLuxStretch*>(sel.op.get()) != nullptr);
    REQUIRE(sel.log_line.find("FILTER='L'")           != std::string::npos);
    REQUIRE(sel.log_line.find("BAYERPAT=''")          != std::string::npos);
    REQUIRE(sel.log_line.find("INSTRUME='ASI2600MM'") != std::string::npos);
    REQUIRE(sel.log_line.find("BROADBAND_L")          != std::string::npos);
    REQUIRE(sel.log_line.find("VeraLux")              != std::string::npos);
}

TEST_CASE("select_auto(meta): Bayer without FILTER classifies as BROADBAND_OSC", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("", "RGGB", "ZWO ASI2400MC Pro"));
    REQUIRE(sel.log_line.find("BAYERPAT='RGGB'") != std::string::npos);
    REQUIRE(sel.log_line.find("BROADBAND_OSC")   != std::string::npos);
}

TEST_CASE("select_auto(meta): dual-NB FILTER on Bayer classifies as DUAL_NB_OSC", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("HaO3", "RGGB", "ZWO ASI2400MC Pro"));
    REQUIRE(sel.log_line.find("FILTER='HaO3'") != std::string::npos);
    REQUIRE(sel.log_line.find("DUAL_NB_OSC")   != std::string::npos);
}

TEST_CASE("select_auto(meta): single-line FILTER on mono classifies as NARROWBAND_SINGLE", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("Ha", "", "ASI2600MM"));
    REQUIRE(sel.log_line.find("FILTER='Ha'")       != std::string::npos);
    REQUIRE(sel.log_line.find("NARROWBAND_SINGLE") != std::string::npos);
}

TEST_CASE("select_auto(meta): unknown FILTER on mono appends the classifier warning", "[module][auto_selector]") {
    auto sel = select_auto(meta_of("5", "", "Asi294mc Pro"));
    REQUIRE(sel.log_line.find("BROADBAND_L")       != std::string::npos);
    REQUIRE(sel.log_line.find("Unknown filter '5'") != std::string::npos);
}
```

In `test/unit/module/test_stretch_factory.cpp`: replace `#include "fits_metadata.hpp"` (line 3) with `#include "nukex/core/frame_metadata.hpp"` and every `FITSMetadata` with `FrameMetadata` (`sed -i 's/FITSMetadata/FrameMetadata/g'` is exact — the field assignments used are only `.filter`).

In `test/CMakeLists.txt`, the module test library: `target_link_libraries(nukex4_module_testlib PUBLIC cfitsio nukex4_stretch)` → `target_link_libraries(nukex4_module_testlib PUBLIC cfitsio nukex4_stretch nukex4_io)`.

- [ ] **Step 12: Full build + ctest**

Run: `cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error|Error" ; ctest 2>&1 | tail -3`
Expected: no errors; `100% tests passed, 0 tests failed out of 67`.

- [ ] **Step 13: Commit**

```bash
git add src/lib/learning test/unit/learning test/CMakeLists.txt src/module test/unit/module
git commit -m "$(cat <<'EOF'
refactor(module): 5-class filter enum for ratings + auto-selector; rating DB user_version 1 -> 2

NukeXInstance, stretch_auto_selector and stretch_factory now consume the
lib FilterClassifier over FrameMetadata (FITSReader::read_headers) instead
of the module-local 4-class classifier over FITSMetadata. lastRun.filter_class
becomes the 5-class rating int (1 BROADBAND_L .. 5 DUAL_NB_OSC, 0 UNKNOWN);
RatingDialog shows the color axis for BROADBAND_RGB and BROADBAND_OSC.

rating_db: PRAGMA user_version is no longer re-stamped on every open.
Fresh DBs are stamped 2; v1 DBs are migrated in one CASE-UPDATE transaction:
  0 (mono or LRGB-color, collapsed) -> 1 BROADBAND_L
  1 (Bayer RGB)                     -> 3 BROADBAND_OSC
  2 (narrowband)                    -> 4 NARROWBAND_SINGLE
  3 (reserved, never written)       -> 4 NARROWBAND_SINGLE
Deviates from the April plan's table, which assumed raw v4 enum values
were stored and would have moved narrowband rows into the OSC bucket.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

### Task 14b: Classifier — Bayer-aware broadband names (L / L-Pro / UV-IR-cut / CLS on OSC)

**Why.** `known_table()` maps `"l"`/`"luminance"` to `BROADBAND_L` regardless of Bayer, and knows none of the broadband light-pollution names in the research database. Two consequences: an OSC frame tagged `FILTER='L'` gets a one-slot mono config with a Bayer pattern (wrong), and the user's own M16 corpus (`FILTER='LPro'`, RGGB) is rejected at batch start as "not in QE DB" — a hard regression versus v4, which stacked it. Spec §6.3 reserves the loud failure for *unknown* names; these names are known broadband.

**Files:**
- Modify: `src/lib/io/src/filter_classifier.cpp`
- Modify: `test/unit/io/test_filter_classifier.cpp`

**Interfaces:**
- Produces: unchanged `FilterClassifier::classify` signature; new behavior: broadband names resolve to `BROADBAND_OSC` (name `"OSC"`) when `bayer_pattern` is non-empty, else `BROADBAND_L` (name `"L"`).

- [ ] **Step 1: Write the failing tests** (append to `test/unit/io/test_filter_classifier.cpp`)

```cpp
TEST_CASE("FilterClassifier: broadband names on Bayer resolve to BROADBAND_OSC", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"L", "Luminance", "LPro", "L-Pro", "UV/IR Cut", "CLS-CCD", "LPS-D1", "L1"}) {
        Filter f = c.classify(make_meta(name, "RGGB", "ZWO ASI2400MC Pro"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_OSC);
        REQUIRE(f.name == "OSC");
        REQUIRE(c.last_warning().empty());
    }
}

TEST_CASE("FilterClassifier: broadband LPR names on mono resolve to BROADBAND_L named L", "[filter_classifier]") {
    FilterClassifier c;
    for (const char* name : {"LPro", "UV-IR-Cut", "CLS", "LPS-D2"}) {
        Filter f = c.classify(make_meta(name, "", "ASI2600MM"));
        INFO(name);
        REQUIRE(f.cls  == FilterClass::BROADBAND_L);
        REQUIRE(f.name == "L");
        REQUIRE(c.last_warning().empty());
    }
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cd build && make test_filter_classifier_io 2>&1 | grep -E "error" ; ./test/test_filter_classifier_io 2>&1 | tail -3`
Expected: 2 failed test cases (`BROADBAND_L` / `UNKNOWN` where `BROADBAND_OSC` expected; `L` name where `OSC` expected).

- [ ] **Step 3: Implement**

In `src/lib/io/src/filter_classifier.cpp`:

1. Remove the `"l"` and `"luminance"` rows from `known_table()`.
2. Add below `known_table()`:

```cpp
// Broadband names whose class depends on the sensor: OSC on a Bayer frame,
// luminance on a mono frame. Normalised (lowercase, alphanumerics only).
// Sources: research/qe_database_research.json types `luminance` and
// `broadband-LPR`, plus the bare L aliases moved out of known_table().
const std::unordered_set<std::string>& broadband_any_names() {
    static const std::unordered_set<std::string> names = {
        "l", "lum", "luminance",
        "lpro", "lpr", "lps", "lpsd1", "lpsd2", "lpsd3", "lpsv4",
        "uvir", "uvircut", "uvirblock", "irblock", "uvcut",
        "cls", "clsccd",
        "l1", "l2", "l3",           // Astronomik L1/L2/L3 UV-IR block
    };
    return names;
}
```

(add `#include <unordered_set>`.)

3. In `classify`, after `const std::string normalized = normalize_name(meta.filter);` and the empty-name branch, before `lookup_known`:

```cpp
    if (broadband_any_names().count(normalized)) {
        if (is_bayer) {
            out.cls  = FilterClass::BROADBAND_OSC;
            out.name = "OSC";
        } else {
            out.cls  = FilterClass::BROADBAND_L;
            out.name = "L";
        }
        out.bandwidth = BandwidthSpec{550.0, 300.0};
        return out;
    }
```

- [ ] **Step 4: Run the classifier + engine tests**

Run: `cd build && make -j$(nproc) 2>&1 | grep -E "error" ; ctest -R "filter|channel_config_from_filter|engine_config|cache_sig" --output-on-failure 2>&1 | tail -3`
Expected: all pass (the existing "L / R / G / B broadband on mono" case still passes: mono `L` → `BROADBAND_L`).

- [ ] **Step 5: Commit**

```bash
git add src/lib/io/src/filter_classifier.cpp test/unit/io/test_filter_classifier.cpp
git commit -m "$(cat <<'EOF'
fix(io): broadband filter names resolve by sensor type (OSC on Bayer, L on mono)

L / Luminance / L-Pro / LPS / UV-IR-cut / CLS on a Bayer frame are plain
OSC broadband, not a one-slot luminance config with a Bayer pattern, and
not an unknown-FILTER batch rejection. Spec 6.3 keeps the loud failure
for genuinely unknown names; these are known broadband names from the
research DB (types luminance, broadband-LPR).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

### Task 15: `tools/import_qe_research.py` — research JSON → engine-keyed shipping JSON

**Why the April version cannot ship.** The research file keys 87 filters by product name (`Astrodon-Ha-3nm-50mm`) with a `passes` array; cameras carry `bayer_pattern` (null for mono), 48 of 55 inherit QE from a `sensors` block that holds `Gr`/`Gb`/`mono_pk`. The loader (`qe_database.cpp`) reads `bayer`, `lines[{name,wavelength_nm,fwhm_nm}]`, and photosites `R/G/B/Gr/Gb/*`; the engine looks filters up by the classifier's canonical names. The April script passed `filters` through untouched — the engine would find no `HaO3`.

**Files:**
- Create: `tools/import_qe_research.py`
- Create: `tools/test_import_qe_research.py`

**Interfaces:**
- Consumes: `research/qe_database_research.json` (`_meta`, `sensors`, `cameras`, `filters`).
- Produces: `share/qe_database.json` with `schema_version: 1`, `cameras` (keys as in research, lowercase model ids, plus `generic_sony_imx_osc`), `filters` (canonical keys `HaO3, S2O3, L-eXtreme, L-eNhance, L-Ultimate, ALP-T, Ha, OIII, SII` plus every product name).

- [ ] **Step 1: Write the tests** — `tools/test_import_qe_research.py`

```python
import json
import pathlib
import subprocess

REPO = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tools" / "import_qe_research.py"
RESEARCH = REPO / "research" / "qe_database_research.json"


def run(src, dst):
    return subprocess.run(["python3", str(SCRIPT), str(src), str(dst)],
                          capture_output=True, text=True, check=False)


def sensor(qe, **extra):
    return {"manufacturer": "Sony Semiconductor", "type": "both-variants", "qe": qe, **extra}


def osc_cam(sensor_name, **extra):
    return {"manufacturer": "ZWO", "sensor": sensor_name, "type": "OSC", "bayer_pattern": "RGGB",
            "qe_inherits_from_sensor": True, "confidence": "high", "source_urls": [], "notes": "", **extra}


BASE = {
    "_meta": {"researcher": "test"},
    "sensors": {
        "IMX585": sensor({"501": {"R": 0.03, "Gr": 0.85, "Gb": 0.87, "B": 0.5, "mono_pk": 0.91},
                          "656": {"R": 0.73, "Gr": 0.32, "Gb": 0.30, "B": 0.03, "mono_pk": 0.81}}),
    },
    "cameras": {
        "asi585mc": osc_cam("IMX585"),
        "asi585mm": {"manufacturer": "ZWO", "sensor": "IMX585", "type": "mono", "bayer_pattern": None,
                     "qe_inherits_from_sensor": True, "confidence": "medium", "source_urls": [], "notes": ""},
    },
    "filters": {
        "Optolong-LeXtreme-7nm": {"type": "dual-narrowband",
                                  "passes": [{"center_nm": 656.3, "fwhm_nm": 7.0}, {"center_nm": 500.7, "fwhm_nm": 7.0}]},
        "SVBony-SV220-3nm": {"type": "dual-narrowband",
                             "passes": [{"center_nm": 656.3, "fwhm_nm": 3.0}, {"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Antlia-ALP-T-SII-OIII-3nm": {"type": "dual-narrowband",
                                      "passes": [{"center_nm": 672.4, "fwhm_nm": 3.0}, {"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Optolong-LeNhance": {"type": "tri-narrowband",
                              "passes": [{"center_nm": 656.3, "fwhm_nm": 24}, {"center_nm": 500.7, "fwhm_nm": 10}, {"center_nm": 486.1, "fwhm_nm": 10}]},
        "Optolong-LUltimate-3nm": {"type": "dual-narrowband",
                                   "passes": [{"center_nm": 656.3, "fwhm_nm": 3.0}, {"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Antlia-ALP-T-Ha-OIII-5nm": {"type": "dual-narrowband",
                                     "passes": [{"center_nm": 656.3, "fwhm_nm": 5.0}, {"center_nm": 500.7, "fwhm_nm": 5.0}]},
        "Astrodon-Ha-3nm-50mm": {"type": "narrowband-single", "passes": [{"center_nm": 656.3, "fwhm_nm": 3.0}]},
        "Astrodon-OIII-3nm-50mm": {"type": "narrowband-single", "passes": [{"center_nm": 500.7, "fwhm_nm": 3.0}]},
        "Astrodon-SII-3nm-50mm": {"type": "narrowband-single", "passes": [{"center_nm": 672.4, "fwhm_nm": 3.0}]},
        "Optolong-Lpro": {"type": "broadband-LPR", "passes": [{"center_nm": 540, "fwhm_nm": 300}]},
    },
}


def write(tmp_path, doc):
    src = tmp_path / "research.json"
    src.write_text(json.dumps(doc))
    return src, tmp_path / "shipped.json"


def test_drops_meta_and_research_only_fields(tmp_path):
    src, dst = write(tmp_path, BASE)
    r = run(src, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    assert "_meta" not in out
    assert out["schema_version"] == 1
    cam = out["cameras"]["asi585mc"]
    for k in ("qe_inherits_from_sensor", "bayer_pattern", "source_urls", "notes"):
        assert k not in cam


def test_osc_camera_inherits_sensor_qe_with_G_mean_and_bayer_key(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    cam = json.loads(dst.read_text())["cameras"]["asi585mc"]
    assert cam["bayer"] == "RGGB"
    assert cam["qe"]["656"] == {"R": 0.73, "G": 0.31, "B": 0.03}


def test_mono_camera_ships_mono_pk_only_and_no_bayer(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    cam = json.loads(dst.read_text())["cameras"]["asi585mm"]
    assert "bayer" not in cam
    assert cam["qe"]["656"] == {"mono_pk": 0.81}


def test_canonical_dual_nb_entries_use_median_fwhm(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    f = json.loads(dst.read_text())["filters"]
    hao3 = f["HaO3"]
    assert hao3["type"] == "DUAL_NB"
    assert [(l["name"], l["wavelength_nm"]) for l in hao3["lines"]] == [("Ha", 656.3), ("OIII", 500.7)]
    assert hao3["lines"][0]["fwhm_nm"] == 4.0          # median of 7.0, 3.0, 3.0, 5.0 (dual-narrowband products only)
    s2o3 = f["S2O3"]
    assert [l["name"] for l in s2o3["lines"]] == ["SII", "OIII"]
    assert s2o3["lines"][0]["fwhm_nm"] == 3.0


def test_classifier_product_canonicals_present_and_hb_dropped(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    f = json.loads(dst.read_text())["filters"]
    assert [l["name"] for l in f["L-eXtreme"]["lines"]] == ["Ha", "OIII"]
    assert [l["name"] for l in f["L-eNhance"]["lines"]] == ["Ha", "OIII"]   # Hb dropped: same photosites as OIII
    assert "Optolong-LeNhance" in f                                          # product entry kept (informational)


def test_single_line_canonicals_and_broadband_products(tmp_path):
    src, dst = write(tmp_path, BASE)
    assert run(src, dst).returncode == 0
    f = json.loads(dst.read_text())["filters"]
    assert f["Ha"]["lines"] == [{"name": "Ha", "wavelength_nm": 656.3, "fwhm_nm": 3.0}]
    assert f["Optolong-Lpro"] == {"type": "BROADBAND", "lines": []}


def test_generic_sony_osc_camera_is_mean_of_sony_osc_cameras(tmp_path):
    doc = json.loads(json.dumps(BASE))
    doc["sensors"]["IMX571"] = sensor({"501": {"R": 0.07, "Gr": 0.89, "Gb": 0.89, "B": 0.6},
                                       "656": {"R": 0.47, "Gr": 0.06, "Gb": 0.04, "B": 0.05}})
    doc["cameras"]["asi2600mc"] = osc_cam("IMX571")
    src, dst = write(tmp_path, doc)
    assert run(src, dst).returncode == 0
    g = json.loads(dst.read_text())["cameras"]["generic_sony_imx_osc"]
    assert g["type"] == "OSC" and g["bayer"] == "RGGB" and g["confidence"] == "low"
    assert g["qe"]["656"]["R"] == round((0.73 + 0.47) / 2, 4)
    assert g["qe"]["656"]["G"] == round((0.31 + 0.05) / 2, 4)


def test_missing_required_field_fails_loud(tmp_path):
    doc = json.loads(json.dumps(BASE))
    del doc["cameras"]["asi585mc"]["confidence"]
    src, dst = write(tmp_path, doc)
    r = run(src, dst)
    assert r.returncode == 1
    assert "asi585mc" in r.stderr and "confidence" in r.stderr
    assert not dst.exists()


def test_missing_canonical_filter_fails_loud(tmp_path):
    doc = json.loads(json.dumps(BASE))
    del doc["filters"]["Antlia-ALP-T-SII-OIII-3nm"]     # no S2O3 source left
    src, dst = write(tmp_path, doc)
    r = run(src, dst)
    assert r.returncode == 1
    assert "S2O3" in r.stderr


def test_real_research_file_round_trip(tmp_path):
    dst = tmp_path / "shipped.json"
    r = run(RESEARCH, dst)
    assert r.returncode == 0, r.stderr
    out = json.loads(dst.read_text())
    for key in ("HaO3", "S2O3", "L-eXtreme", "L-eNhance", "L-Ultimate", "ALP-T", "Ha", "OIII", "SII"):
        assert key in out["filters"], key
    assert "generic_sony_imx_osc" in out["cameras"]
    assert len(out["cameras"]) >= 56
    assert out["cameras"]["asi2400mc"]["bayer"] == "RGGB"
```

- [ ] **Step 2: Run to verify failure**

Run: `python3 -m pytest tools/test_import_qe_research.py -q 2>&1 | tail -3`
Expected: 10 failed (`No such file` for the script).

- [ ] **Step 3: Implement `tools/import_qe_research.py`**

```python
#!/usr/bin/env python3
"""Transform research/qe_database_research.json -> share/qe_database.json.

The research file is keyed the way a researcher thinks: product filter names
with `passes`, cameras that inherit QE from a `sensors` block, Gr/Gb/mono_pk
photosites. The engine is keyed the way FilterClassifier + QEDatabase think:
canonical filter names ("HaO3", "L-eXtreme", ...) with emission `lines`, one
resolved QE block per camera with R/G/B (OSC) or mono_pk (mono) photosites.
This script is the only bridge between the two. It is deterministic and
refuses to write a database the engine cannot consume.

Usage: import_qe_research.py <research.json> <shipped.json>
"""
import argparse
import json
import statistics
import sys

SCHEMA_VERSION = 1
GENERIC_OSC_KEY = "generic_sony_imx_osc"

# Emission lines the engine solves for. Hb is intentionally absent from the
# canonical Q-solve set: at 486 nm it lands on the same B/G photosites as
# OIII (501 nm), so a Q matrix with both columns is rank-deficient on an
# RGB sensor and ChannelDecomposer would throw SingularQError.
LINES = {"Ha": 656.3, "OIII": 500.7, "SII": 672.4, "Hb": 486.1}
Q_SOLVE_LINES = ("Ha", "OIII", "SII")
LINE_TOL_NM = 4.0

# Canonical dual-NB keys emitted by FilterClassifier::known_table(), by the
# set of Q-solve lines their passes cover.
CANONICAL_DUAL = {frozenset(("Ha", "OIII")): "HaO3", frozenset(("SII", "OIII")): "S2O3"}

# Product entries that FilterClassifier maps to their own canonical name.
PRODUCT_CANONICAL = {
    "Optolong-LeXtreme-7nm":    "L-eXtreme",
    "Optolong-LeNhance":        "L-eNhance",
    "Optolong-LUltimate-3nm":   "L-Ultimate",
    "Antlia-ALP-T-Ha-OIII-5nm": "ALP-T",
}
REQUIRED_CANONICAL = tuple(CANONICAL_DUAL.values()) + tuple(PRODUCT_CANONICAL.values()) + Q_SOLVE_LINES

TYPE_MAP = {
    "narrowband-single": "NARROWBAND",
    "dual-narrowband": "DUAL_NB", "tri-narrowband": "DUAL_NB",
    "quad-narrowband": "DUAL_NB", "narrowband-multi": "DUAL_NB",
    "luminance": "BROADBAND", "broadband-RGB": "BROADBAND", "broadband-LPR": "BROADBAND",
}
BAYER_OK = {"RGGB", "BGGR", "GRBG", "GBRG"}
REQUIRED_CAMERA_FIELDS = ("sensor", "type", "confidence")


class TransformError(ValueError):
    pass


def nearest_line(center_nm):
    name, wl = min(LINES.items(), key=lambda kv: abs(kv[1] - center_nm))
    return name if abs(wl - center_nm) <= LINE_TOL_NM else None


def passes_to_lines(passes):
    """Research `passes` -> engine `lines`, keeping only passes on a known emission line."""
    out = []
    for p in passes:
        name = nearest_line(float(p["center_nm"]))
        if name is None:
            continue
        out.append({"name": name, "wavelength_nm": LINES[name], "fwhm_nm": float(p["fwhm_nm"])})
    return out


def q_solve_lines(lines):
    return [l for l in lines if l["name"] in Q_SOLVE_LINES]


def median_lines(entries):
    """Same line set across several products -> one entry with median FWHM per line."""
    by_name = {}
    order = []
    for lines in entries:
        for l in lines:
            if l["name"] not in by_name:
                order.append(l["name"])
                by_name[l["name"]] = []
            by_name[l["name"]].append(l["fwhm_nm"])
    return [{"name": n, "wavelength_nm": LINES[n], "fwhm_nm": round(statistics.median(by_name[n]), 2)}
            for n in order]


def transform_filters(filters):
    out = {}
    dual_sources = {}    # canonical key -> [lines, ...]
    single_sources = {}  # line name -> [lines, ...]

    for name, f in filters.items():
        ftype = TYPE_MAP.get(f.get("type"))
        if ftype is None:
            raise TransformError(f"Filter '{name}': unknown type {f.get('type')!r}")
        lines = passes_to_lines(f.get("passes", []))
        out[name] = {"type": ftype, "lines": lines}

        q_lines = q_solve_lines(lines)
        if f.get("type") == "dual-narrowband":   # tri/quad products carry extra passes; keep them out of the medians
            key = CANONICAL_DUAL.get(frozenset(l["name"] for l in q_lines))
            if key:
                dual_sources.setdefault(key, []).append(q_lines)
        elif ftype == "NARROWBAND" and len(q_lines) == 1:
            single_sources.setdefault(q_lines[0]["name"], []).append(q_lines)

        if name in PRODUCT_CANONICAL:
            out[PRODUCT_CANONICAL[name]] = {"type": "DUAL_NB", "lines": q_lines}

    for key, sources in dual_sources.items():
        # Ha before OIII, SII before OIII: the order the classifier documents.
        lines = median_lines(sources)
        lines.sort(key=lambda l: -l["wavelength_nm"])
        out[key] = {"type": "DUAL_NB", "lines": lines}
    for line_name, sources in single_sources.items():
        out[line_name] = {"type": "NARROWBAND", "lines": median_lines(sources)}

    missing = [k for k in REQUIRED_CANONICAL if k not in out]
    if missing:
        raise TransformError(f"Research data yields no source for canonical filter(s): {missing}")
    return out


def resolved_qe(name, cam, sensors):
    if cam.get("qe_inherits_from_sensor"):
        s = sensors.get(cam.get("sensor"))
        if s is None:
            raise TransformError(f"Camera '{name}' inherits from sensor {cam.get('sensor')!r} which is not in `sensors`")
        return s.get("qe", {})
    return cam.get("qe", {})


def photosites(name, cam_type, qe_block):
    """Collapse research photosites to what the loader consumes."""
    out = {}
    for wl, sites in qe_block.items():
        if cam_type == "mono":
            if "mono_pk" in sites:
                out[str(wl)] = {"mono_pk": float(sites["mono_pk"])}
            continue
        greens = [sites[k] for k in ("G", "Gr", "Gb") if k in sites]
        if "R" not in sites or "B" not in sites or not greens:
            raise TransformError(f"Camera '{name}': wavelength {wl} lacks R/G/B photosites: {sorted(sites)}")
        out[str(wl)] = {"R": float(sites["R"]),
                        "G": round(statistics.mean(float(g) for g in greens), 4),
                        "B": float(sites["B"])}
    return out


def transform_camera(name, cam, sensors):
    missing = [k for k in REQUIRED_CAMERA_FIELDS if k not in cam]
    if missing:
        raise TransformError(f"Camera '{name}' missing required field(s): {missing}")
    out = {"sensor": cam["sensor"], "type": cam["type"], "confidence": cam["confidence"]}
    if "manufacturer" in cam:
        out["manufacturer"] = cam["manufacturer"]
    if cam["type"] == "OSC":
        bayer = cam.get("bayer_pattern")
        if bayer not in BAYER_OK:
            raise TransformError(f"Camera '{name}': OSC camera needs bayer_pattern in {sorted(BAYER_OK)}, got {bayer!r}")
        out["bayer"] = bayer
    out["qe"] = photosites(name, cam["type"], resolved_qe(name, cam, sensors))
    if cam["type"] == "OSC" and len(out["qe"]) < 2:
        raise TransformError(f"Camera '{name}': OSC camera needs QE at >= 2 wavelengths")
    return out


def generic_sony_osc(cameras, sensors):
    """Mean R/G/B per wavelength over Sony-sensor OSC cameras; the spec 6.3 unknown-INSTRUME fallback."""
    members = [c for c in cameras.values()
               if c["type"] == "OSC" and "sony" in sensors.get(c["sensor"], {}).get("manufacturer", "").lower()]
    if not members:
        raise TransformError("No Sony-sensor OSC cameras found; cannot derive generic_sony_imx_osc")
    wavelengths = set.intersection(*(set(c["qe"]) for c in members))
    qe = {}
    for wl in sorted(wavelengths, key=int):
        qe[wl] = {site: round(statistics.mean(c["qe"][wl][site] for c in members), 4) for site in ("R", "G", "B")}
    return {"sensor": "generic", "manufacturer": "generic", "type": "OSC", "bayer": "RGGB",
            "confidence": "low", "qe": qe}


def transform(research):
    sensors = research.get("sensors", {})
    cameras = {}
    mono_without_qe = []
    for name, cam in research.get("cameras", {}).items():
        cameras[name] = transform_camera(name, cam, sensors)
        if cam.get("type") == "mono" and not cameras[name]["qe"]:
            mono_without_qe.append(name)
    cameras[GENERIC_OSC_KEY] = generic_sony_osc(cameras, sensors)
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_from": "research/qe_database_research.json via tools/import_qe_research.py",
        "cameras": cameras,
        "filters": transform_filters(research.get("filters", {})),
    }, mono_without_qe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()
    with open(args.input) as f:
        research = json.load(f)
    try:
        shipped, mono_without_qe = transform(research)
    except TransformError as e:
        print(f"import_qe_research: {e}", file=sys.stderr)
        return 1
    if mono_without_qe:
        print(f"note: {len(mono_without_qe)} mono camera(s) ship without a QE block "
              f"(sensor has no mono_pk; mono frames never enter the Q-solve): {mono_without_qe}",
              file=sys.stderr)
    with open(args.output, "w") as f:
        json.dump(shipped, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"wrote {args.output}: {len(shipped['cameras'])} cameras, {len(shipped['filters'])} filters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

`chmod +x tools/import_qe_research.py`.

- [ ] **Step 4: Run the tests**

Run: `python3 -m pytest tools/test_import_qe_research.py -q 2>&1 | tail -3`
Expected: `10 passed`.

- [ ] **Step 5: Commit**

```bash
git add tools/import_qe_research.py tools/test_import_qe_research.py
git commit -m "$(cat <<'EOF'
feat(tools): import_qe_research.py maps research JSON onto the engine's lookup keys

The engine looks filters up by FilterClassifier's canonical names (HaO3,
S2O3, L-eXtreme, L-eNhance, L-Ultimate, ALP-T) and cameras by INSTRUME-
derived keys; the research file keys 87 filters by product name with a
`passes` array. The transform derives the canonical entries from the
products (median FWHM per line, Hb dropped from the Q-solve set because
it shares photosites with OIII), keeps product entries for override
reference, resolves sensor inheritance, collapses Gr/Gb to G, renames
bayer_pattern -> bayer, ships mono_pk for mono cameras, and derives the
spec-6.3 generic_sony_imx_osc fallback camera as the mean of Sony OSC
cameras. Fails loud on any missing canonical key or camera field.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

### Task 15b: Camera resolution — INSTRUME normalisation + generic-camera fallback (spec §6.3)

**Why.** `Filter.camera` is the FITS `INSTRUME` string verbatim. Real headers on this machine read `ZWO ASI2400MC Pro`, `Asi294mc Pro`, `ATR585M`. `QEDatabase::has_camera` / `lookup_camera_qe` do an exact `unordered_map::find` on keys like `asi2400mc`. Every real dual-NB stack would fail Phase B with `UnknownCameraError`. Spec §6.3's "unknown INSTRUME → generic Sony IMX OSC values + loud warning" is not implemented anywhere.

**Files:**
- Modify: `src/lib/calibration/include/nukex/calibration/qe_database.hpp`
- Modify: `src/lib/calibration/src/qe_database.cpp`
- Modify: `test/unit/calibration/test_qe_database.cpp`
- Modify: `src/lib/stacker/include/nukex/stacker/stacking_engine.hpp` (`ExecuteResult`)
- Modify: `src/lib/stacker/src/stacking_engine.cpp:346-350`
- Modify: `src/module/NukeXInstance.cpp` (composed-window keywords, near line 641)
- Modify: `test/fixtures/qe/minimal_db.json` (add `generic_sony_imx_osc`)

**Interfaces:**
- Produces: `static std::string QEDatabase::normalize_camera_key(const std::string&)`; `std::string QEDatabase::resolve_camera(const std::string& instrume) const` (normalised DB key or `""`); `static constexpr const char* QEDatabase::kGenericOSCCamera = "generic_sony_imx_osc"`; `bool ExecuteResult::qe_generic_camera_fallback`.
- Consumers: `StackingEngine` (dual-NB camera set), `ChannelDecomposer::build_q` (unchanged — it calls `has_camera`/`lookup_camera_qe`, which now normalise).

- [ ] **Step 1: Write the failing tests** (append to `test/unit/calibration/test_qe_database.cpp`)

```cpp
TEST_CASE("QEDatabase: camera keys normalise (case, punctuation)", "[qe_database]") {
    REQUIRE(QEDatabase::normalize_camera_key("ZWO ASI2400MC Pro") == "zwoasi2400mcpro");
    REQUIRE(QEDatabase::normalize_camera_key("asi_585-MC")        == "asi585mc");
}

TEST_CASE("QEDatabase: lookups are case-insensitive", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.has_camera("asi585mc"));
    REQUIRE(db.has_camera("ASI585MC"));
    REQUIRE(db.lookup_camera_qe("asi585mc", 656.3, Photosite::R) ==
            db.lookup_camera_qe("ASI585MC", 656.3, Photosite::R));
}

TEST_CASE("QEDatabase: resolve_camera maps a real INSTRUME onto a DB key", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.resolve_camera("ASI585MC")            == "asi585mc");   // exact
    REQUIRE(db.resolve_camera("ZWO ASI2600MC Pro")   == "asi2600mc");  // key is a substring
    REQUIRE(db.resolve_camera("asi2600mc-pro")       == "asi2600mc");
    REQUIRE(db.resolve_camera("ATR585M")             == "");           // no key contained
    REQUIRE(db.resolve_camera("ASI585")              == "");           // partial key does not count
    REQUIRE(db.resolve_camera("")                    == "");
}

TEST_CASE("QEDatabase: resolve_camera prefers the longest contained key", "[qe_database]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.load_override(fixture("override_camera_pro.json").string()).ok);
    REQUIRE(db.resolve_camera("ZWO ASI2600MC Pro") == "asi2600mcpro");
    REQUIRE(db.resolve_camera("ZWO ASI2600MC")     == "asi2600mc");
}
```

Create `test/fixtures/qe/override_camera_pro.json`:

```json
{
  "schema_version": 1,
  "cameras": {
    "ASI2600MC-Pro": {
      "sensor": "IMX571",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": { "501": { "R": 0.08, "G": 0.89, "B": 0.60 }, "656": { "R": 0.46, "G": 0.05, "B": 0.04 } },
      "confidence": "medium"
    }
  }
}
```

Add `generic_sony_imx_osc` to `test/fixtures/qe/minimal_db.json` `cameras` (a copy of the `ASI585MC` block with `"sensor": "generic"` and `"confidence": "low"`).

- [ ] **Step 2: Run to verify failure**

Run: `cd build && make test_qe_database 2>&1 | grep -E "error" | head -3`
Expected: `'normalize_camera_key' is not a member of 'nukex::QEDatabase'`.

- [ ] **Step 3: Implement in `QEDatabase`**

`qe_database.hpp`, inside `class QEDatabase` public section:

```cpp
    // Key used for the spec-6.3 unknown-INSTRUME fallback. Shipped by
    // tools/import_qe_research.py as the mean of Sony-sensor OSC cameras.
    static constexpr const char* kGenericOSCCamera = "generic_sony_imx_osc";

    // Lowercase, alphanumerics only. Applied to every camera key on load and
    // to every camera argument on lookup, so "ASI585MC" == "asi585mc".
    static std::string normalize_camera_key(const std::string& raw);

    // Maps a FITS INSTRUME value onto a DB camera key: exact normalised match
    // first, else the longest DB key contained in the normalised INSTRUME
    // ("ZWO ASI2400MC Pro" -> "asi2400mc"). Returns "" when nothing matches;
    // callers decide between failing loud and kGenericOSCCamera.
    std::string resolve_camera(const std::string& instrume) const;
```

`qe_database.cpp`:

```cpp
std::string QEDatabase::normalize_camera_key(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string QEDatabase::resolve_camera(const std::string& instrume) const {
    const std::string key = normalize_camera_key(instrume);
    if (key.empty()) return {};
    if (cameras_.count(key)) return key;
    std::string best;
    for (const auto& kv : cameras_) {
        if (kv.first.size() > best.size() && key.find(kv.first) != std::string::npos) {
            best = kv.first;
        }
    }
    return best;
}
```

(add `#include <cctype>`.) In `parse_and_merge`, store `cameras_[normalize_camera_key(name)] = std::move(cam);`. In `has_camera`, `confidence`, and `lookup_camera_qe`, look up `normalize_camera_key(name)` instead of `name`. Filters are untouched (canonical names are exact by construction).

- [ ] **Step 4: Run the calibration tests**

Run: `cd build && make test_qe_database test_channel_decomposer 2>&1 | grep -E "error" ; ctest -R "qe_database|decomposer" --output-on-failure 2>&1 | tail -3`
Expected: 2/2 pass (decomposer's `build_q("ASI585MC", …)` keeps working through normalisation).

- [ ] **Step 5: Engine — resolve at the dual-NB camera set, fall back loudly**

`stacking_engine.hpp`, in `ExecuteResult` after `n_frames_rejected_filter`:

```cpp
        bool qe_generic_camera_fallback = false; // spec 6.3: INSTRUME not in QE DB, generic Sony OSC QE used
```

`stacking_engine.cpp`: next to the existing `dual_nb_cameras` declaration add `std::set<std::string> unknown_instrume_warned;`. Replace lines 347–350 with:

```cpp
        // Track DUAL_NB_OSC cameras for the Q-solve mixed-camera guard.
        // Resolve the raw INSTRUME onto a DB key here so the guard compares
        // DB identities ("asi2400mc"), not header spellings.
        if (frame_filter.cls == FilterClass::DUAL_NB_OSC) {
            std::string key = qe_database_->resolve_camera(frame_filter.camera);
            if (key.empty()) {
                key = QEDatabase::kGenericOSCCamera;
                result.qe_generic_camera_fallback = true;
                if (unknown_instrume_warned.insert(frame_filter.camera).second) {
                    obs.message("Camera '" + frame_filter.camera +
                                "' unknown; using generic Sony IMX OSC QE values. "
                                "Output marked as low-confidence (NUKEX_QE_CONFIDENCE).");
                }
            }
            dual_nb_cameras.insert(key);
        }
```

Ensure `result` is the `ExecuteResult` being built in that scope (it is — the same object that later receives `result.derived`). Add `#include <set>` if absent.

- [ ] **Step 6: Module — mark the composed window**

In `src/module/NukeXInstance.cpp`, directly after the `NUKEX_GAMUT_CLIPPED` keyword append (≈ line 646):

```cpp
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_QE_CONFIDENCE",
          result.qe_generic_camera_fallback ? "generic-fallback" : "database",
          "QE source for Phase B: camera entry or generic Sony OSC fallback" ) );
```

- [ ] **Step 7: Full build + ctest**

Run: `cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -3`
Expected: `100% tests passed` (same binary count; the new cases run inside test_qe_database).

- [ ] **Step 8: Commit**

```bash
git add src/lib/calibration src/lib/stacker src/module/NukeXInstance.cpp test/unit/calibration test/fixtures/qe
git commit -m "$(cat <<'EOF'
fix(calibration): resolve FITS INSTRUME onto QE DB keys; generic OSC fallback per spec 6.3

QEDatabase keyed cameras by exact string while Filter.camera carries the
raw INSTRUME ("ZWO ASI2400MC Pro", "Asi294mc Pro"); every real dual-NB
stack would have thrown UnknownCameraError at Phase B. Keys are now
normalised (lowercase alphanumerics) on load and lookup, and
resolve_camera() maps an INSTRUME onto the longest contained key. When
nothing matches, the engine uses generic_sony_imx_osc, warns once per
INSTRUME in the Process Console, sets ExecuteResult::qe_generic_camera_fallback,
and the module writes NUKEX_QE_CONFIDENCE='generic-fallback' on the
composed window.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

## Wave 2

### Task 16: Generate + ship `share/qe_database.json`; resolve it from the PixInsight base directory

**Why the April version cannot ship.** The engine's default `qe_database_path = "share/qe_database.json"` is relative to the process working directory, which under PixInsight is not the install root. The root `CMakeLists.txt` has no `install()` rules to hang a rule on, and `tools/release.sh` tars only `bin/`. PCL exposes the install root as `PixInsightSettings::GlobalString("Application/BaseDirectory")` (`<pcl/GlobalSettings.h>`); the Phase 8 code already assumes `<base>/share` but hard-codes `/opt/PixInsight`.

**Files:**
- Create: `share/qe_database.json` (generated by Task 15's script)
- Modify: `src/module/NukeXInstance.cpp:186-200, 436-447, 664-680`
- Modify: `tools/release.sh:81-95`
- Modify: `.gitignore`

- [ ] **Step 1: Generate**

```bash
mkdir -p share
python3 tools/import_qe_research.py research/qe_database_research.json share/qe_database.json
```
Expected: `wrote share/qe_database.json: 56 cameras, 96 filters` (87 products + 9 canonical), plus the informational note listing mono cameras without `mono_pk`.

- [ ] **Step 2: Sanity-check against the loader**

```bash
python3 - <<'EOF'
import json
db = json.load(open('share/qe_database.json'))
assert db['schema_version'] == 1
for k in ("HaO3","S2O3","L-eXtreme","L-eNhance","L-Ultimate","ALP-T"):
    assert len(db['filters'][k]['lines']) == 2, k
print('cameras', len(db['cameras']), 'filters', len(db['filters']))
print('asi2400mc', db['cameras']['asi2400mc']['qe']['656'])
print('generic  ', db['cameras']['generic_sony_imx_osc']['qe']['656'])
EOF
ls -la share/qe_database.json
```
Expected: assertions pass; file between 60 KB and 200 KB.

Then prove the C++ loader accepts it: `cd build && ./test/test_qe_database` still passes (fixture-based), and run a one-line probe:

```bash
cat > /tmp/qe_probe.cpp <<'EOF'
#include "nukex/calibration/qe_database.hpp"
#include <cstdio>
int main(int, char** argv) {
    nukex::QEDatabase db;
    auto r = db.load_shipped(argv[1]);
    if (!r.ok) { std::printf("FAIL %s\n", r.error.c_str()); return 1; }
    std::printf("ok cameras=%d filters=%d resolve('ZWO ASI2400MC Pro')='%s' HaO3 lines=%zu\n",
                db.n_cameras(), db.n_filters(), db.resolve_camera("ZWO ASI2400MC Pro").c_str(),
                db.lookup_filter("HaO3").lines.size());
    return 0;
}
EOF
g++ -std=c++17 -I src/lib/calibration/include -I src/lib/core/include -I src/lib/io/include /tmp/qe_probe.cpp \
    build/src/lib/calibration/libnukex4_calibration.a build/src/lib/io/libnukex4_io.a build/src/lib/core/libnukex4_core.a \
    -o /tmp/qe_probe && /tmp/qe_probe share/qe_database.json
```
Expected: `ok cameras=56 filters=96 resolve('ZWO ASI2400MC Pro')='asi2400mc' HaO3 lines=2`. (If the link pulls FITS symbols, append `build/_deps/cfitsio-build/libcfitsio.a -lz`; the probe is throw-away.)

- [ ] **Step 3: Module — one share-root resolver**

In `src/module/NukeXInstance.cpp`, add near the top (after the includes) `#include <pcl/GlobalSettings.h>` and:

```cpp
// <PixInsight base>/share — where the release tarball deposits
// qe_database.json (Task 16) and where the Phase 8 bootstrap will live.
static std::string PIShareRoot()
{
   const std::string base =
       pcl::PixInsightSettings::GlobalString( "Application/BaseDirectory" ).ToUTF8().c_str();
   return base + "/share";
}
```

Then:
1. Lines 195 and 673: replace `const std::string share_root = "/opt/PixInsight/share";` with `const std::string share_root = PIShareRoot();` and delete the surrounding comments that describe the hard-coded path (lines 666–675 block: keep the `user_data_root` lines).
2. Lines 442–447: after `config.qe_override_path = …` add `config.qe_database_path = PIShareRoot() + "/qe_database.json";` and replace the four-line comment with `// Shipped QE database lives beside the module install: <base>/share/qe_database.json.`
3. Lines 461–468 (the `!result.ok` message): replace `"Task 16 in progress"` wording with `"the release package was not fully installed"`.

- [ ] **Step 4: Package `share/` in the release tarball**

In `tools/release.sh` `package_release()`, after the two `cp` lines:

```bash
    echo "=== Staging share/ ==="
    rm -rf "${REPO_DIR}/share"
    mkdir -p "${REPO_DIR}/share"
    cp "${REPO}/share/qe_database.json" "${REPO_DIR}/share/"
```

and change the tar line to `tar -C "${REPO_DIR}" -czf "${TAR}" bin/ share/`. Update the header comment ("Produces a PI-compatible release tarball … bin/ + share/"). In `.gitignore`, under `repository/bin/` add `repository/share/`.

- [ ] **Step 5: Verify the tarball layout without the signing step**

```bash
mkdir -p /tmp/nukex_tar_probe/bin /tmp/nukex_tar_probe/share
cp share/qe_database.json /tmp/nukex_tar_probe/share/ && touch /tmp/nukex_tar_probe/bin/NukeX-pxm.so
tar -C /tmp/nukex_tar_probe -czf /tmp/nukex_tar_probe.tar.gz bin/ share/ && tar -tzf /tmp/nukex_tar_probe.tar.gz
```
Expected: `bin/`, `bin/NukeX-pxm.so`, `share/`, `share/qe_database.json`. PixInsight extracts update packages relative to its base directory, so `share/` lands beside `bin/`. The end-to-end proof is Task 23 Step 9 (install through the updater, confirm the file exists).

- [ ] **Step 6: Build**

Run: `cd build && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -2`
Expected: no errors, all pass.

- [ ] **Step 7: Commit**

```bash
git add share/qe_database.json src/module/NukeXInstance.cpp tools/release.sh .gitignore
git commit -m "$(cat <<'EOF'
feat(share): ship qe_database.json; resolve it under PixInsight's base directory

Generated from research/qe_database_research.json by
tools/import_qe_research.py (56 cameras incl. generic_sony_imx_osc,
96 filters incl. the 9 canonical keys the classifier emits). The module
now sets qe_database_path from
PixInsightSettings::GlobalString("Application/BaseDirectory") + /share,
and the Phase 8 bootstrap share_root uses the same resolver instead of a
hard-coded /opt/PixInsight. release.sh stages share/ into the tarball
next to bin/.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

### Task 17: Document the override file + correct the remediation messages

**Why changed.** There is no default `~/.nukex4/qe_overrides.json`: the override is whatever file the user picks with the Browse button (`instance.qeOverridePath`, empty = none). The engine's batch-rejection message still tells users to edit `~/.nukex4/qe_overrides.json`.

**Files:**
- Create: `docs/qe_overrides_format.md`
- Modify: `src/lib/stacker/src/stacking_engine.cpp:199-201`
- Modify: `src/lib/io/src/filter_classifier.cpp` (mono unknown-filter warning text)

- [ ] **Step 1: Write the doc** — `docs/qe_overrides_format.md`

````markdown
# QE override file

NukeX ships a quantum-efficiency database (`<PixInsight>/share/qe_database.json`,
56 cameras, 96 filters). To add a camera or filter it does not know, or to
replace shipped values with your own measurements, write a JSON file with
the same shape and select it with **QE override file… → Browse** in the
NukeX interface. Leave the field empty to use the shipped database only.

## Schema

```json
{
  "schema_version": 1,
  "cameras": {
    "<camera-key>": {
      "sensor": "IMX585",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "501": { "R": 0.03, "G": 0.85, "B": 0.50 },
        "656": { "R": 0.73, "G": 0.32, "B": 0.03 }
      },
      "confidence": "high"
    }
  },
  "filters": {
    "<filter-key>": {
      "type": "DUAL_NB",
      "lines": [
        { "name": "Ha",   "wavelength_nm": 656.3, "fwhm_nm": 7.0 },
        { "name": "OIII", "wavelength_nm": 500.7, "fwhm_nm": 7.0 }
      ]
    }
  }
}
```

- `qe` keys are wavelengths in nm; values are QE fractions 0–1 per photosite.
  OSC cameras use `R`, `G`, `B`; mono cameras use `mono_pk`.
- `confidence` is `high`, `medium` or `low`. It is informational.
- `type` on filters is informational (`DUAL_NB`, `NARROWBAND`, `BROADBAND`).

## How keys are matched

**Cameras** are matched against the FITS `INSTRUME` keyword after normalising
both to lowercase alphanumerics, and a database key may be a substring of the
header value. `INSTRUME = 'ZWO ASI2400MC Pro'` matches the shipped key
`asi2400mc`. Use the model name as the key (`asi2400mc`, `qhy268c`), not the
full header string. When no key matches, NukeX uses `generic_sony_imx_osc`,
prints a warning in the Process Console, and writes
`NUKEX_QE_CONFIDENCE = 'generic-fallback'` on the composed image.

**Filters** are matched by the *canonical* name the classifier derives from
the FITS `FILTER` keyword, not by the raw header text. Recognised spellings
(case and punctuation ignored): `HaO3`/`HaOIII` → `HaO3`; `S2O3`/`SIIOIII` →
`S2O3`; `L-eXtreme`, `L-eNhance`, `L-Ultimate`, `ALP-T`; `Ha`/`Halpha`,
`OIII`/`O3`, `SII`/`S2`; `L`/`Luminance`/`L-Pro`/`LPS`/`UV-IR-cut`/`CLS`
(broadband); `R`, `G`, `B`. A dual-narrowband filter with any other name on a
Bayer camera stops the batch at start — rename the `FILTER` keyword to one of
the spellings above, or set it to one of the canonical names and add a
matching `filters` entry here. Mono frames with an unknown `FILTER` are
treated as luminance with a warning.

## Override semantics

- `cameras` and `filters` merge with the shipped database.
- An entry whose key collides with a shipped key **replaces the whole entry**.
- New keys are added. `"cameras": {}` / `"filters": {}` is a valid no-op.

## Errors

Malformed JSON stops the batch with the parser's line and column. A camera
that exists but lacks QE at a needed wavelength yields a singular Q matrix
and stops Phase B with the camera and filter named.
````

- [ ] **Step 2: Correct the engine message**

`src/lib/stacker/src/stacking_engine.cpp` lines 199–201: replace with

```cpp
        err.error = "FILTER='" + first_filter.name + "' on Bayer frame not in QE DB. "
                    "Rename FILTER to a known spelling, or add the filter to a "
                    "qe_overrides.json file and select it with the QE override picker "
                    "(see docs/qe_overrides_format.md). Remove FILTER to stack as plain OSC.";
```

(`test_phase_a_router`'s unknown-filter case asserts the substring `qe_overrides.json` — still present.)

In `filter_classifier.cpp`, the mono warning: `"If this is a narrowband filter, add it to qe_overrides.json."` → `"If this is a narrowband filter, rename FILTER to Ha/OIII/SII or add it to a qe_overrides.json selected in the NukeX interface."`

- [ ] **Step 3: Build + targeted tests**

Run: `cd build && make -j$(nproc) 2>&1 | grep -E "error" ; ctest -R "filter_classifier|stacking_engine" 2>&1 | tail -2`
Expected: pass.

- [ ] **Step 4: Commit**

```bash
git add docs/qe_overrides_format.md src/lib/stacker/src/stacking_engine.cpp src/lib/io/src/filter_classifier.cpp
git commit -m "$(cat <<'EOF'
docs: QE override file format + accurate remediation messages

Documents the override schema, how INSTRUME and FILTER are matched onto
database keys (normalised camera substrings; canonical filter names), the
generic-camera fallback, and replace-on-collision merge semantics. Engine
and classifier messages no longer point at a ~/.nukex4 path that nothing
reads; the override is the file picked in the interface.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

### Task 18: Delete `StackingMode`, `from_mode`, `output_rgb_mapping`, `is_mono`, `stacking_mode_name`

**State today.** No production code references these (verified: only `channel_config.{hpp,cpp}` and four test files). `is_mono()` reads `mode`, which `from_filter()` never sets — it is already wrong for every v5 config and has no callers.

**Files:**
- Modify: `src/lib/core/include/nukex/core/channel_config.hpp`, `src/lib/core/src/channel_config.cpp`
- Rewrite: `test/unit/core/test_channel_config.cpp`
- Modify: `test/unit/core/test_cube.cpp:7,17,27,35,43`, `test/unit/combine/test_output_assembler.cpp:9,33`, `test/unit/combine/test_spatial_context.cpp:34`

- [ ] **Step 1: Confirm zero production callers**

Run: `grep -rnE "from_mode|output_rgb_mapping|StackingMode|is_mono|stacking_mode_name" src/ | grep -v channel_config`
Expected: no output. Any hit means a missed migration — stop and fix it first.

- [ ] **Step 2: Migrate the tests first (they define the new surface)**

Add to each of the three test files that used `from_mode` a local helper and swap the calls:

```cpp
static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}
```

- `test_cube.cpp`: `from_mode(StackingMode::OSC_RGB)` → `cfg_for(FilterClass::BROADBAND_OSC, "OSC")` and the `n_channels == 3` assertion on line 11 becomes `== 4` (R, G, B, synthesised L); `from_mode(StackingMode::MONO_L)` → `cfg_for(FilterClass::BROADBAND_L, "L")`; `from_mode(StackingMode::OSC_HAO3)` → `cfg_for(FilterClass::DUAL_NB_OSC, "HaO3")` and the per-voxel `n_channels == 2` on line 31 becomes `== 3` (`R_HaO3`, `G_HaO3`, `B_HaO3`). Add `#include "nukex/core/filter.hpp"`.
- `test_output_assembler.cpp:9,33` and `test_spatial_context.cpp:34`: `from_mode(StackingMode::MONO_L)` → `cfg_for(FilterClass::BROADBAND_L, "L")`.

Replace `test/unit/core/test_channel_config.cpp` entirely:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/filter.hpp"

using namespace nukex;

static ChannelConfig cfg_for(FilterClass cls, const char* name) {
    Filter f;
    f.cls  = cls;
    f.name = name;
    return ChannelConfig::from_filter(f);
}

TEST_CASE("ChannelConfig: channel_index_for_name / slot_index / slot_name over a merged LRGB config", "[channel]") {
    ChannelConfig cfg = cfg_for(FilterClass::BROADBAND_L, "L");
    for (const char* c : {"R", "G", "B"}) {
        cfg = ChannelConfig::merge(cfg, cfg_for(FilterClass::BROADBAND_RGB, c));
    }
    REQUIRE(cfg.n_channels == 4);
    REQUIRE(cfg.channel_index_for_name("L") == 0);
    REQUIRE(cfg.channel_index_for_name("R") == 1);
    REQUIRE(cfg.channel_index_for_name("G") == 2);
    REQUIRE(cfg.channel_index_for_name("B") == 3);
    REQUIRE(cfg.channel_index_for_name("Ha") == -1);
    REQUIRE(cfg.slot_index("G") == cfg.channel_index_for_name("G"));
    REQUIRE(cfg.slot_name(3) == "B");
}

TEST_CASE("ChannelConfig: bayer defaults to NONE for mono classes and RGGB for Bayer classes", "[channel]") {
    REQUIRE(cfg_for(FilterClass::BROADBAND_L, "L").bayer        == BayerPattern::NONE);
    REQUIRE(cfg_for(FilterClass::NARROWBAND_SINGLE, "Ha").bayer == BayerPattern::NONE);
    REQUIRE(cfg_for(FilterClass::BROADBAND_OSC, "OSC").bayer    == BayerPattern::RGGB);
    REQUIRE(cfg_for(FilterClass::DUAL_NB_OSC, "HaO3").bayer     == BayerPattern::RGGB);
}
```

- [ ] **Step 3: Delete the symbols**

`channel_config.hpp`: remove `enum class StackingMode`, `stacking_mode_name()`, the `StackingMode mode` member, `output_rgb_mapping[3]`, the `from_mode` declaration, and `bool is_mono() const;`.
`channel_config.cpp`: remove `ChannelConfig::from_mode` and `ChannelConfig::is_mono` definitions.

- [ ] **Step 4: Build + ctest**

Run: `cd build && cmake .. > /dev/null && make -j$(nproc) 2>&1 | grep -E "error" ; ctest 2>&1 | tail -2`
Expected: no errors, all pass.

- [ ] **Step 5: Commit**

```bash
git add src/lib/core test/unit/core test/unit/combine
git commit -m "$(cat <<'EOF'
chore(core): remove StackingMode, from_mode, output_rgb_mapping, is_mono

Dead since Tasks 7-12 routed everything through ChannelConfig::from_filter.
is_mono() read a `mode` field that from_filter never set. Tests that used
from_mode as setup now build configs from Filter values; Bayer-class
channel counts follow from_filter (OSC = R,G,B,L; dual-NB = 3 raw slots).
Spec 4.3 dead-code list.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

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

`src/module/CMakeLists.txt`: remove `fits_metadata.cpp` and `filter_classifier.cpp` from `MODULE_SOURCES`; remove the cfitsio block at lines 56–59 (`# cfitsio: fits_metadata.cpp …` through `target_link_libraries(NukeX-pxm PRIVATE cfitsio)`) — `nukex4_io` carries cfitsio transitively.

`test/CMakeLists.txt`: in `nukex4_module_testlib` remove the two source lines for `fits_metadata.cpp` and `filter_classifier.cpp`; change its link line to `target_link_libraries(nukex4_module_testlib PUBLIC nukex4_stretch nukex4_io)`; delete the `nukex_add_test(test_fits_metadata …)` and `nukex_add_test(test_filter_classifier …)` lines. Update the comment `# Module-layer tests (fits_metadata and future module code)` → `# Module-layer tests (auto-selector, stretch factory, Phase 8 fallback)`.

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

### Task 20: Synthetic-FITS writer; un-gate the 8 integration tests; add the generic-camera case

**State today.** `test/integration/test_phase_a_router.cpp` and `test_phase_b_qsolve.cpp` hold their real bodies inside `#if WIRED_BY_TASK_20` with a `#define WIRED_BY_TASK_20 0` and `SKIP(...)` placeholders. They call `test_util::write_synthetic_bayer / write_synthetic_mono / write_synthetic_q_solved_hao3 / write_synthetic_q_solved_s2o3` and include `test_data_loader.hpp` under `using namespace nukex;`, so the writer lives in `namespace nukex::test_util`. `test_util` is a static library declared in `test/CMakeLists.txt` (not `test/util/CMakeLists.txt`). No synthetic FITS writer exists anywhere in the tree.

**Files:**
- Create: `test/util/synthetic_fits.hpp`, `test/util/synthetic_fits.cpp`
- Modify: `test/CMakeLists.txt` (`test_util` sources + links)
- Modify: `test/integration/test_phase_a_router.cpp`, `test/integration/test_phase_b_qsolve.cpp`

**Interfaces:**
- Consumes: `nukex::QEDatabase::load_shipped`, `nukex::ChannelDecomposer::build_q(camera, filter) -> Eigen::MatrixXd (3×N)`, cfitsio.
- Produces (namespace `nukex::test_util`):

```cpp
void write_synthetic_bayer(const std::string& path, int w, int h, const std::string& bayer,
                           const std::string& instrument, const std::string& filter, float uniform_value);
void write_synthetic_mono (const std::string& path, int w, int h,
                           const std::string& instrument, const std::string& filter, float uniform_value);
void write_synthetic_q_solved_hao3(const std::string& path, int w, int h, const std::string& camera,
                                   float ha, float oiii, const std::string& instrume = "");
void write_synthetic_q_solved_s2o3(const std::string& path, int w, int h, const std::string& camera,
                                   float sii, float oiii, const std::string& instrume = "");
```
`instrume` empty → the camera name is written as INSTRUME; non-empty → written verbatim (lets a test exercise the Task 15b generic fallback while the Q matrix still comes from `camera`).

- [ ] **Step 1: Write the writer's own unit test** — create `test/unit/io/test_synthetic_fits.cpp`

```cpp
#include "catch_amalgamated.hpp"
#include "synthetic_fits.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"

#include <filesystem>

using namespace nukex;
namespace fs = std::filesystem;

TEST_CASE("synthetic_fits: Bayer frame round-trips headers, size and photosite layout", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_bayer.fits";
    test_util::write_synthetic_bayer(p.string(), 32, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.image.width()  == 32);
    REQUIRE(r.image.height() == 16);
    REQUIRE(r.image.n_channels() == 1);
    REQUIRE(r.metadata.bayer_pattern == "RGGB");
    REQUIRE(r.metadata.instrument    == "ASI585MC");
    REQUIRE(r.metadata.filter        == "HaO3");
    REQUIRE(r.image.at(0, 0, 0) == Catch::Approx(0.5f));
}

TEST_CASE("synthetic_fits: mono frame has no BAYERPAT", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_mono.fits";
    test_util::write_synthetic_mono(p.string(), 8, 8, "ASI2600MM", "L", 0.25f);
    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.metadata.bayer_pattern.empty());
    REQUIRE(r.metadata.filter == "L");
}

TEST_CASE("synthetic_fits: q-solved HaO3 frame's photosites invert back to the targets", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_qsolved.fits";
    test_util::write_synthetic_q_solved_hao3(p.string(), 8, 8, "ASI585MC", 0.5f, 0.3f);
    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);

    QEDatabase db;
    REQUIRE(db.load_shipped(std::string(NUKEX_TEST_FIXTURES_DIR) + "/qe/minimal_db.json").ok);
    ChannelDecomposer dec(db);
    // RGGB: (0,0)=R, (1,0)=G, (1,1)=B
    Eigen::Vector3d rgb(r.image.at(0, 0, 0), r.image.at(1, 0, 0), r.image.at(1, 1, 0));
    Eigen::VectorXd lines = dec.solve("ASI585MC", "HaO3", rgb);
    REQUIRE(lines(0) == Catch::Approx(0.5).margin(1e-6));
    REQUIRE(lines(1) == Catch::Approx(0.3).margin(1e-6));
}

TEST_CASE("synthetic_fits: instrume override is written verbatim", "[synthetic_fits]") {
    auto p = fs::temp_directory_path() / "synthetic_instrume.fits";
    test_util::write_synthetic_q_solved_hao3(p.string(), 8, 8, "ASI585MC", 0.5f, 0.3f, "Unknown Cam X");
    REQUIRE(FITSReader::read_headers(p.string()).instrument == "Unknown Cam X");
}
```

Register in `test/CMakeLists.txt` after the `test_util` block:

```cmake
nukex_add_test(test_synthetic_fits unit/io/test_synthetic_fits.cpp test_util nukex4_calibration nukex4_io)
```

- [ ] **Step 2: Run to verify failure**

Run: `cd build && cmake .. 2>&1 | tail -1 && make test_synthetic_fits 2>&1 | grep -E "error" | head -2`
Expected: `synthetic_fits.hpp: No such file or directory`.

- [ ] **Step 3: Implement** — `test/util/synthetic_fits.hpp`

```cpp
#pragma once
#include <string>

namespace nukex { namespace test_util {

// Single-frame FITS writers for integration tests. Float32 pixels; headers
// BAYERPAT / INSTRUME / FILTER written only when non-empty.
void write_synthetic_bayer(const std::string& path, int w, int h, const std::string& bayer,
                           const std::string& instrument, const std::string& filter, float uniform_value);

void write_synthetic_mono(const std::string& path, int w, int h,
                          const std::string& instrument, const std::string& filter, float uniform_value);

// RGGB Bayer frames whose (R, G, B) photosite values are Q * (line1, line2)
// for the (camera, filter) Q matrix from test/fixtures/qe/minimal_db.json,
// so a Q-solve recovers exactly (line1, line2) at every pixel.
// `instrume` empty -> INSTRUME = camera; otherwise written verbatim.
void write_synthetic_q_solved_hao3(const std::string& path, int w, int h, const std::string& camera,
                                   float ha, float oiii, const std::string& instrume = "");
void write_synthetic_q_solved_s2o3(const std::string& path, int w, int h, const std::string& camera,
                                   float sii, float oiii, const std::string& instrume = "");

}} // namespace nukex::test_util
```

`test/util/synthetic_fits.cpp`:

```cpp
#include "synthetic_fits.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"

#include <fitsio.h>
#include <Eigen/Dense>

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace nukex { namespace test_util {

namespace {

void write_fits(const std::string& path, int w, int h, const std::vector<float>& pixels,
                const std::string& bayer, const std::string& instrument, const std::string& filter) {
    fitsfile* fp = nullptr;
    int status = 0;
    std::remove(path.c_str());
    fits_create_file(&fp, path.c_str(), &status);
    long naxes[2] = {w, h};
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &status);
    fits_write_img(fp, TFLOAT, 1, static_cast<long>(w) * h,
                   const_cast<float*>(pixels.data()), &status);
    if (!bayer.empty())      fits_update_key_str(fp, "BAYERPAT", bayer.c_str(),      nullptr, &status);
    if (!instrument.empty()) fits_update_key_str(fp, "INSTRUME", instrument.c_str(), nullptr, &status);
    if (!filter.empty())     fits_update_key_str(fp, "FILTER",   filter.c_str(),     nullptr, &status);
    fits_close_file(fp, &status);
    if (status != 0) {
        char msg[FLEN_ERRMSG];
        fits_get_errstatus(status, msg);
        throw std::runtime_error(std::string("synthetic_fits: ") + msg + " writing " + path);
    }
}

// Lay (r, g, b) onto a 2x2 Bayer mosaic. pattern[(y%2)*2 + x%2] names the photosite.
std::vector<float> bayerize(int w, int h, const std::string& pattern, float r, float g, float b) {
    if (pattern.size() != 4) throw std::runtime_error("synthetic_fits: BAYERPAT must be 4 chars");
    std::vector<float> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const char site = pattern[(y % 2) * 2 + (x % 2)];
            out[static_cast<size_t>(y) * w + x] = site == 'R' ? r : site == 'B' ? b : g;
        }
    }
    return out;
}

const QEDatabase& fixture_db() {
    static const QEDatabase db = [] {
        QEDatabase d;
        auto r = d.load_shipped(std::string(NUKEX_TEST_FIXTURES_DIR) + "/qe/minimal_db.json");
        if (!r.ok) throw std::runtime_error("synthetic_fits: " + r.error);
        return d;
    }();
    return db;
}

void write_q_solved(const std::string& path, int w, int h, const std::string& camera,
                    const std::string& filter, double line1, double line2, const std::string& instrume) {
    ChannelDecomposer dec(fixture_db());
    Eigen::MatrixXd Q = dec.build_q(camera, filter);          // 3 x 2
    Eigen::Vector3d rgb = Q * Eigen::Vector2d(line1, line2);
    auto pixels = bayerize(w, h, "RGGB",
                           static_cast<float>(rgb(0)), static_cast<float>(rgb(1)), static_cast<float>(rgb(2)));
    write_fits(path, w, h, pixels, "RGGB", instrume.empty() ? camera : instrume, filter);
}

} // namespace

void write_synthetic_bayer(const std::string& path, int w, int h, const std::string& bayer,
                           const std::string& instrument, const std::string& filter, float uniform_value) {
    write_fits(path, w, h, bayerize(w, h, bayer, uniform_value, uniform_value, uniform_value),
               bayer, instrument, filter);
}

void write_synthetic_mono(const std::string& path, int w, int h,
                          const std::string& instrument, const std::string& filter, float uniform_value) {
    write_fits(path, w, h, std::vector<float>(static_cast<size_t>(w) * h, uniform_value),
               "", instrument, filter);
}

void write_synthetic_q_solved_hao3(const std::string& path, int w, int h, const std::string& camera,
                                   float ha, float oiii, const std::string& instrume) {
    write_q_solved(path, w, h, camera, "HaO3", ha, oiii, instrume);
}

void write_synthetic_q_solved_s2o3(const std::string& path, int w, int h, const std::string& camera,
                                   float sii, float oiii, const std::string& instrume) {
    write_q_solved(path, w, h, camera, "S2O3", sii, oiii, instrume);
}

}} // namespace nukex::test_util
```

`test/CMakeLists.txt` `test_util` block:

```cmake
add_library(test_util STATIC
    util/png_writer.cpp
    util/test_data_loader.cpp
    util/synthetic_fits.cpp
)
target_include_directories(test_util PUBLIC
    "${CMAKE_SOURCE_DIR}/test/util"
    "${CMAKE_SOURCE_DIR}/third_party/stb"
    "${CMAKE_SOURCE_DIR}/third_party/catch2"
)
target_link_libraries(test_util PUBLIC nukex4_io nukex4_stretch nukex4_calibration PRIVATE cfitsio)
```

- [ ] **Step 4: Run the writer test**

Run: `cd build && cmake .. > /dev/null && make test_synthetic_fits 2>&1 | grep -E "error" ; ./test/test_synthetic_fits 2>&1 | tail -2`
Expected: `All tests passed (…)`.

- [ ] **Step 5: Un-gate the integration tests**

In both `test/integration/test_phase_a_router.cpp` and `test_phase_b_qsolve.cpp`:
1. Delete `#define WIRED_BY_TASK_20 0` and the header comment block that describes the gate (phase_a lines 3–22; phase_b lines 3–22 equivalent).
2. Replace `#if WIRED_BY_TASK_20` / `#include "test_data_loader.hpp"` / `#endif` with `#include "synthetic_fits.hpp"`.
3. In every `TEST_CASE`, delete the `#if WIRED_BY_TASK_20` line, the `#else` … `SKIP(...)` … `#endif` lines, leaving the real body.

Append to `test_phase_b_qsolve.cpp` (exercises Task 15b end to end):

```cpp
TEST_CASE("Phase B Q-solve: unknown INSTRUME falls back to generic_sony_imx_osc with a warning",
          "[.integration][phase_b]") {
    // Photosites are engineered from ASI585MC's Q; the fixture's generic
    // camera is a copy of ASI585MC, so the fallback recovers the same lines.
    auto tmp = fs::temp_directory_path() / "phase_b_generic.fits";
    test_util::write_synthetic_q_solved_hao3(tmp.string(), 16, 16, "ASI585MC", 0.5f, 0.3f,
                                              /*instrume*/"Unknown Cam X");
    StackingEngine::Config cfg;
    cfg.qe_database_path = (fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json").string();
    StackingEngine engine(cfg);
    auto result = engine.execute({tmp.string()}, {}, nullptr);

    REQUIRE(result.ok);
    REQUIRE(result.qe_generic_camera_fallback);
    REQUIRE(result.derived.slots.at("Ha")[8 * 16 + 8] == Catch::Approx(0.5f).margin(0.02f));
}
```

- [ ] **Step 6: Run the integration binaries with the opt-in tag**

Run:
```bash
cd build && make test_phase_a_router test_phase_b_qsolve 2>&1 | grep -E "error"
./test/integration/test_phase_a_router "[integration]" 2>&1 | tail -3
./test/integration/test_phase_b_qsolve  "[integration]" 2>&1 | tail -3
```
Expected: phase_a `All tests passed (… in 5 test cases)`; phase_b `All tests passed (… in 5 test cases)`.

If a Q-solve case misses its ±0.02 target while the writer's own unit test passes, the discrepancy is inside the engine's Phase A/B numeric path (per-frame normalisation, welford selection), not the writer: read the engine's Phase A value routing for `DUAL_NB_OSC` before touching any tolerance, and report the root cause in the commit.

- [ ] **Step 7: Full ctest (default set) + commit**

Run: `cd build && ctest 2>&1 | tail -2`
Expected: all pass (the `[.integration]` cases stay opt-in).

```bash
git add test/util/synthetic_fits.hpp test/util/synthetic_fits.cpp test/unit/io/test_synthetic_fits.cpp \
        test/CMakeLists.txt test/integration
git commit -m "$(cat <<'EOF'
test(util): synthetic FITS writer; un-gate the Phase A/B integration tests

nukex::test_util::write_synthetic_{bayer,mono,q_solved_hao3,q_solved_s2o3}
write float32 single frames with BAYERPAT/INSTRUME/FILTER headers; the
q-solved flavours derive photosite values from the fixture QE DB's Q
matrix so the engine's Q-solve recovers the engineered lines. The eight
[.integration] cases written in Tasks 9-10 now run for real, plus a case
for the generic-camera fallback (Task 15b).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

## Wave 3 (needs the PixInsight desktop on this machine)

### Task 21: E2E — preserve the v4 golden bit-for-bit, add the v5 corpora

**What the April text got wrong.** The manifest is `schema_version: 1` with the three stretch sweeps *inside* the `lrgb_mono_ngc7635` case (`dropdown_sweep`); the golden `test/fixtures/golden/lrgb_mono_ngc7635.json` already holds `primary.{stacked,noise,stretched}` + `sweep.{GHS,MTF,ArcSinh}` hashes. Separate `sweep_*` cases do not exist. The spec's "M27 HaO3" corpus does not exist on disk; the dual-NB corpus is **M16 HaO3** (`~/projects/processing/M16`, 30 frames `*HaO3*`, ZWO ASI2400MC Pro, RGGB), mixed in one directory with 58 `LPro` frames. NGC7635 records filter-wheel *slot numbers* as `FILTER` (`'1'`…`'5'`), so it can only serve the luminance case. Real LRGB-mono with proper `FILTER` names is **M27 2025** (`/mnt/qnap/astro_data/9_1_2025/M27`, ATR585M, L24 R12 G12 B24). Plain OSC is **M27 2023** (`/mnt/qnap/astro_data/4_12_2023/M27`, 33 frames, ASI2400MC, no FILTER). The harness hashes `NukeX_stacked/noise/stretched` and does not check `filter_class_expected`.

**Files:**
- Modify: `tools/validate_e2e.js` (`collectLights`, `runPrimary` tags, golden shape)
- Modify: `test/fixtures/e2e_manifest.json`
- Create: `test/fixtures/golden/{bayer_rgb_m27_2023,bayer_nb_hao3_m16,mono_lrgb_m27_2025}.json` (by regen)

- [ ] **Step 0: Install the dev module + database into PixInsight**

```bash
cd build && make -j$(nproc) NukeX-pxm 2>&1 | grep -E "error"
sudo install -D -m 644 share/qe_database.json /opt/PixInsight/share/qe_database.json   # dev staging only; Task 23 proves the updater path
```
In PixInsight: Process → Modules → Install Modules… → select `build/src/module/NukeX-pxm.so` → Install. Confirm Process Explorer shows NukeX with today's build.

- [ ] **Step 1: Verify the regression floor BEFORE any harness change**

Run: `cd build && make e2e 2>&1 | tail -15`
Expected: `lrgb_mono_ngc7635` primary + all three sweeps report golden match (bit-identical to v4.0.1.0). If any hash differs, STOP: the L-only path must be unchanged by v5 (spec §7.3). Find which commit since `0f1b8fe` moved it (bisect with `make e2e`) and fix before continuing. Do not regen.

- [ ] **Step 2: Harness — frame glob/limit and the composed window**

`tools/validate_e2e.js`:

```js
function collectLights(dir, glob, max_frames) {
   var pats = glob ? [glob] : ["*.fit", "*.fits", "*.FIT", "*.FITS"];
   var all = [];
   for (var i = 0; i < pats.length; i++) {
      var found = searchDirectory(dir + "/" + pats[i], false);
      for (var j = 0; j < found.length; j++) all.push(found[j]);
   }
   all.sort();
   if (max_frames && all.length > max_frames) all = all.slice(0, max_frames);
   return all;
}
```
Update both call sites to `collectLights(tc.light_dir, tc.light_glob, tc.max_frames)`. In `runPrimary`, `var tags = ["stacked", "noise", "stretched"];` → `["stacked", "noise", "stretched", "composed"]` (the loop already skips absent windows). In the golden builder (the function that assigns `out.stacked/noise/stretched` from `primary.pixel_hashes`) add `if (primary.pixel_hashes.composed) out.composed = primary.pixel_hashes.composed.fnv1a_hex;`. Update the header comment (steps 1–5) to mention `NukeX_composed`.

- [ ] **Step 3: Manifest**

Replace the three `*_placeholder` cases in `test/fixtures/e2e_manifest.json` with (keep `lrgb_mono_ngc7635` byte-for-byte):

```json
    {
      "name": "bayer_rgb_m27_2023",
      "filter_class_expected": "BROADBAND_OSC",
      "light_dir": "/mnt/qnap/astro_data/4_12_2023/M27",
      "flat_dir": "",
      "primary_stretch": 0,
      "primary_stretch_label": "Auto",
      "finishing_stretch": 0,
      "finishing_stretch_label": "None",
      "wall_time_budget_s": 1500,
      "min_frames_ok_alignment": 30
    },
    {
      "name": "bayer_nb_hao3_m16",
      "filter_class_expected": "DUAL_NB_OSC",
      "light_dir": "/home/scarter4work/projects/processing/M16",
      "light_glob": "*HaO3*.fit",
      "max_frames": 12,
      "flat_dir": "",
      "primary_stretch": 0,
      "primary_stretch_label": "Auto",
      "finishing_stretch": 0,
      "finishing_stretch_label": "None",
      "wall_time_budget_s": 1500,
      "min_frames_ok_alignment": 12,
      "visual_bar": "no green cast in the nebula; Ha red, OIII teal; NUKEX_QE_CONFIDENCE = database"
    },
    {
      "name": "mono_lrgb_m27_2025",
      "filter_class_expected": "BROADBAND_L + BROADBAND_RGB",
      "light_dir": "/mnt/qnap/astro_data/9_1_2025/M27",
      "flat_dir": "",
      "primary_stretch": 0,
      "primary_stretch_label": "Auto",
      "finishing_stretch": 0,
      "finishing_stretch_label": "None",
      "wall_time_budget_s": 1800,
      "min_frames_ok_alignment": 72
    },
    {
      "name": "bayer_nb_s2o3_placeholder",
      "skip": true,
      "skip_reason": "no S2O3 corpus on this machine"
    },
    {
      "name": "lrgbsho_placeholder",
      "skip": true,
      "skip_reason": "no mixed L+R+G+B+HaO3 corpus on this machine"
    }
```
Set `"schema_version": 2` and update `description` to `"v5 E2E corpus: preserved v4 LRGB-mono floor + OSC, dual-NB (M16 HaO3) and LRGB-mono (M27 2025) cases"`.

- [ ] **Step 4: Verify run (new cases unchecked), then regen**

Run: `cd build && make e2e 2>&1 | tail -25`
Expected: `lrgb_mono_ngc7635` still matches; the three new cases execute OK (`execute_ok`, `alignment_all_ok: true`, within budget) and report `golden_check.checked: false` (no golden yet). Any `executeGlobal false` is a real defect — read `build/e2e.log` for the module's message (typical: QE database missing → Step 0 was skipped; unknown camera → Task 15b regressed).

Then: `make e2e-regen 2>&1 | tail -10 && git status --short test/fixtures/golden`
Expected: three new golden files; `lrgb_mono_ngc7635.json` **unmodified** (`git diff --exit-code test/fixtures/golden/lrgb_mono_ngc7635.json` → exit 0). If it shows a diff, the regen has overwritten the floor — `git checkout` it and go back to Step 1.

- [ ] **Step 5: Verify against the new goldens once more**

Run: `cd build && make e2e 2>&1 | grep -E "golden|status" | head -20`
Expected: four cases `golden match`, two skipped.

- [ ] **Step 6: Commit**

```bash
git add tools/validate_e2e.js test/fixtures/e2e_manifest.json test/fixtures/golden
git commit -m "$(cat <<'EOF'
test(e2e): v5 corpora (OSC M27-2023, dual-NB M16 HaO3, LRGB-mono M27-2025); v4 floor preserved

lrgb_mono_ngc7635 (primary + GHS/MTF/ArcSinh sweeps) is byte-identical
to the v4.0.1.0 golden: the L-only path is untouched by the overhaul.
New goldens hash NukeX_stacked/noise/stretched/composed. The harness
gains light_glob + max_frames so the M16 directory's HaO3 frames can be
selected apart from its LPro frames. The spec's "M27 HaO3" set does not
exist on disk; M16 HaO3 is the dual-NB motivating corpus.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

### Task 22: Real-data visual validation (M16 HaO3, M27 LRGB-mono, M27 OSC)

Human eyes required. Everything else is scripted. The dev module and database are already installed from Task 21 Step 0.

**Files:**
- Create: `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/M16_HaO3_v4.0.1.0_baseline.png`, `M16_HaO3_v5.png`, `M16_HaO3_green_histogram.png`, `M27_LRGB_v5.png`, `M27_OSC_v5.png`
- Create: `docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence/README.md`

- [ ] **Step 1: v4 baseline for M16 HaO3**

**Corrected 2026-09-04.** Two things in the original text are wrong on this machine:

1. `output_root` is no longer `/tmp/nukex_e2e` — `/tmp` is tmpfs here and the
   frame cache written there OOM-killed a run. Outputs are now under
   `~/.cache/nukex_e2e/`.
2. **The released v4.0.1.0 binary cannot be used as the baseline: it does not
   load.** It links `libglog.so.0` and Fedora 44 ships only `libglog.so.2`;
   PixInsight reports `NukeX is not defined`.

Use the **v4.0.1.0 source rebuilt with today's toolchain** instead, at
`~/projects/nukex4/build/src/module/NukeX-pxm.so` (already built and verified
this session — it reproduces v5 HEAD byte-for-byte on NGC7635, which is how
we know the overhaul did not disturb the L-only path). That is also the
*better* baseline: same compiler, same libraries, so the only variable left
is the v5 code itself.

Install it, run the same 12 M16 HaO3 frames with Auto stretch, save
`NukeX_stretched` to `~/.cache/nukex_e2e/m16_v4_stretched.fit`, then
re-install the v5 module and re-sign it.

- [ ] **Step 2: Render comparison PNGs + the green histogram**

```bash
python3 - <<'EOF'
import os
import numpy as np, matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from astropy.io import fits
out = "docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence"
def load(p):
    a = fits.getdata(p).astype("float32")
    return np.moveaxis(a, 0, -1) if a.ndim == 3 and a.shape[0] in (3, 4) else a
def png(a, path):
    a = np.clip(a, 0, 1); plt.imsave(path, (a * 255).astype("uint8"))
v4 = load(os.path.expanduser("~/.cache/nukex_e2e/m16_v4_stretched.fit"))
v5 = load(os.path.expanduser("~/.cache/nukex_e2e/bayer_nb_hao3_m16/primary/stretched.fit"))
png(v4, f"{out}/M16_HaO3_v4.0.1.0_baseline.png"); png(v5, f"{out}/M16_HaO3_v5.png")
fig, ax = plt.subplots(figsize=(8, 4))
for name, img in (("v4.0.1.0", v4), ("v5", v5)):
    if img.ndim == 3:
        g_excess = img[..., 1] - 0.5 * (img[..., 0] + img[..., 2])
        ax.hist(g_excess.ravel(), bins=200, range=(-0.3, 0.3), histtype="step", label=name)
ax.set_xlabel("G - (R+B)/2 per pixel"); ax.legend(); fig.savefig(f"{out}/M16_HaO3_green_histogram.png", dpi=120)
png(load(os.path.expanduser("~/.cache/nukex_e2e/mono_lrgb_m27_2025/primary/stretched.fit")), f"{out}/M27_LRGB_v5.png")
png(load(os.path.expanduser("~/.cache/nukex_e2e/bayer_rgb_m27_2023/primary/stretched.fit")), f"{out}/M27_OSC_v5.png")
print("ok")
EOF
```
Expected: five PNGs (8-bit, per `feedback_8bit_png`). Embed them inline in the session for review; do not offer `xdg-open`.

- [ ] **Step 3: Judge against the spec §7.4 bar and record**

Write `visual-evidence/README.md` with one row per image: corpus, frame count, stretch, and the verdict against the bar (M16: no green cast, Ha red / OIII teal, the histogram's v5 curve centred nearer zero than v4's; M27 LRGB: detail preserved, colour natural; M27 OSC: natural colour with synthesised L). Include the `NUKEX_QE_CONFIDENCE` keyword value read from `stretched.fit`'s composed sibling (`fits.getheader(".../composed.fit")["NUKEX_QE_CONFIDENCE"]`) — expected `database` for M16 (ASI2400MC resolves).

If the M16 bar is not met, the fix is in `src/lib/compose` (palette vectors, Task 6 of the April plan) or in the Q-solve inputs (Task 15's `HaO3` line FWHM or the ASI2400MC QE row) — not in the stretch. Open that as a new task in this document before continuing to Task 23.

- [ ] **Step 4: Commit the evidence**

```bash
git add docs/superpowers/specs/2026-04-26-color-science-overhaul/visual-evidence
git commit -m "$(cat <<'EOF'
docs(visual-evidence): v5 vs v4.0.1.0 on M16 HaO3, M27 LRGB-mono, M27 OSC

Side-by-side stretched outputs and the per-pixel green-excess histogram
for the dual-NB motivating case, with the spec 7.4 verdicts in README.md.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
```

---

## Wave 4

### Task 23: Release v5.0.0.0 — version, changelog, README, repository manifest, package, push, updater proof

**State today.** `repository/` does not exist in nukex5 (`tools/release.sh` refuses to run without `repository/updates.xri`); `.gitignore` ignores `repository/*.tar.gz`, yet the PixInsight repository *is* the committed tarball served by raw.githubusercontent (nukex4 force-adds them). There is no README, so a public repository has no install instructions. `CHANGELOG.md` is titled "NukeX v4".

**Decision to confirm with the user before Step 6:** the PixInsight repository URL. Default here: this repo's own `https://raw.githubusercontent.com/scarter4work/nukex5/main/repository/` (v4 users keep v4 until they add the new URL). Alternative: also commit the package into `~/projects/nukex4/repository/` so existing v4 installs auto-update to v5 on their next update check.

**Files:**
- Modify: `src/module/NukeXVersion.h`, `CHANGELOG.md`, `.gitignore`
- Create: `README.md`, `repository/updates.xri`, `repository/<date>-linux-x64-NukeX.tar.gz` (by `release.sh`)

- [ ] **Step 1: Version + date**

`src/module/NukeXVersion.h`: `MAJOR 5, MINOR 0, REVISION 0, BUILD 0`; `RELEASE_YEAR/MONTH/DAY` = output of `date +%Y %m %d` on release day (no leading zeros).

- [ ] **Step 2: CHANGELOG**

Change the title line to `# NukeX — Changelog` and insert above the v4.0.1.0 entry:

```markdown
## v5.0.0.0 — YYYY-MM-DD

Color-science overhaul. NukeX now knows what filter and camera produced
each frame, decomposes dual-narrowband OSC data into its emission lines
through the camera's measured quantum efficiency, and composes colour in
Lab/LCH with a calibrated emission-line palette. The v4.0.0.7 green cast
on HaO3 data is gone at the root: the engine no longer routes every Bayer
frame through the plain-OSC path.

### Added
- Filter taxonomy: BROADBAND_L, BROADBAND_RGB, BROADBAND_OSC, NARROWBAND_SINGLE, DUAL_NB_OSC, resolved from FITS FILTER / BAYERPAT / INSTRUME with a tiered policy (unknown dual-NB names on Bayer stop the batch loudly; unknown mono names warn and stack as luminance).
- Quantum-efficiency database (`share/qe_database.json`): 55 cameras plus a generic Sony OSC fallback, 87 filters plus the canonical HaO3 / S2O3 / L-eXtreme / L-eNhance / L-Ultimate / ALP-T entries. Camera keys match the INSTRUME keyword case-insensitively and by model substring.
- Phase B Q-matrix decomposition (Eigen QR) of dual-NB OSC stacks into Ha / OIII / SII slots; multi-source OIII merge across HaO3 + S2O3 batches.
- ColorComposer: Lab/LCH composite of derived slots with a calibrated emission-line palette (no green quadrant by construction); new `NukeX_composed` window; `NUKEX_GAMUT_CLIPPED` and `NUKEX_QE_CONFIDENCE` provenance keywords.
- OSC-as-LRGB: a rec709 luminance slot synthesised per OSC frame.
- "QE override file…" picker in the interface for user cameras/filters (`docs/qe_overrides_format.md`).
- Broadband light-pollution names (L-Pro, LPS, UV-IR cut, CLS) recognised as plain OSC on Bayer cameras.

### Changed
- Rating DB `user_version` 1 → 2: stored filter classes migrate to the 5-class encoding on first open (pre-v5 narrowband ratings become NARROWBAND_SINGLE).
- Rating popup shows the colour axis for RGB-mono and OSC stacks.
- E2E corpus: v4 LRGB-mono golden preserved bit-identical; new OSC, dual-NB (M16 HaO3) and LRGB-mono (M27) baselines.
- Eigen is taken from the system (`find_package(Eigen3)`), not vendored.

### Removed
- `StackingMode` enum, `ChannelConfig::from_mode`, `output_rgb_mapping`, `is_mono`.
- Module-local `filter_classifier` and `fits_metadata` (superseded by lib/io).
```

- [ ] **Step 3: README**

Create `README.md`:

```markdown
# NukeX

Distribution-fitted image stacking and auto-stretch for PixInsight, with
filter-aware colour science: dual-narrowband OSC data is decomposed into
emission lines through the camera's quantum efficiency and composed with a
calibrated palette.

## Install (PixInsight 1.8.9+, Linux x64)

Resources → Updates → Manage Repositories → Add:

    https://raw.githubusercontent.com/scarter4work/nukex5/main/repository/

then Check for Updates and restart. The package installs `bin/NukeX-pxm.so`
and `share/qe_database.json` under the PixInsight base directory.

## Use

Process → NukeX. Add light frames (and optional flats), pick a stretch
(Auto is recommended), Execute. Outputs: `NukeX_stacked` (linear),
`NukeX_composed` (3-channel sRGB when derived slots exist), `NukeX_stretched`,
`NukeX_noise`. Unknown cameras or filters: see `docs/qe_overrides_format.md`.

## Build from source

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build

Requires PCL at `~/PCL` (or `-DPCLDIR=`), system Eigen 3, glog/Ceres,
OpenCL headers. See `CHANGELOG.md` for release notes.
```

- [ ] **Step 4: Repository manifest + ignore rules**

Create `repository/updates.xri` (the signature line is appended by `release.sh`):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<xri version="1.0">
   <description>
      <title>NukeX — Distribution-Fitted Stacking with Calibrated Colour Science</title>
      <copyright>Copyright (c) 2026 Scott Carter</copyright>
   </description>
   <platform os="linux" arch="x64" version="1.8.0:1.9.9">
      <package fileName="PLACEHOLDER-linux-x64-NukeX.tar.gz" sha1="0000000000000000000000000000000000000000" type="module" releaseDate="20260101">
         <title>NukeX</title>
         <description>NukeX v5.0.0.0 — colour-science overhaul. Filter taxonomy from FITS FILTER/BAYERPAT/INSTRUME; quantum-efficiency database (55 cameras, 87 filters) driving a Q-matrix decomposition of dual-narrowband OSC stacks into Ha/OIII/SII; Lab/LCH ColorComposer with a calibrated emission-line palette; OSC-as-LRGB luminance synthesis; QE override file picker. Fixes the v4 green cast on HaO3 data at the root. Rating DB migrates in place. Package now ships share/qe_database.json beside bin/.</description>
      </package>
   </platform>
</xri>
```

`.gitignore`: delete the line `repository/*.tar.gz` (the tarball is the distribution) and keep `repository/*.xsgn`, `repository/bin/`, `repository/share/`.

- [ ] **Step 5: Clean build, full tests, E2E verify**

```bash
cd build && cmake .. > /dev/null && make clean && make -j$(nproc) 2>&1 | grep -E "error"
ctest 2>&1 | tail -2
./test/integration/test_phase_a_router "[integration]" | tail -1
./test/integration/test_phase_b_qsolve  "[integration]" | tail -1
make e2e 2>&1 | grep -E "golden|status" | head -12
```
Expected: no errors; `100% tests passed`; both integration binaries `All tests passed`; four E2E cases `golden match`.

- [ ] **Step 6: Sign + package** (confirm the repository decision first)

```bash
printf '%s' '***REDACTED-SIGNING-PASSWORD***' > /tmp/.pi_codesign_pass && chmod 600 /tmp/.pi_codesign_pass
tools/release.sh package 2>&1 | tail -12
tar -tzf repository/$(date +%Y%m%d)-linux-x64-NukeX.tar.gz
tail -1 repository/updates.xri | cut -c1-60
```
Expected: `signed: … NukeX-pxm.so / .xsgn`; tarball lists `bin/NukeX-pxm.so`, `bin/NukeX-pxm.xsgn`, `share/qe_database.json`; `updates.xri` ends with `<Signature developerId="scarter4work"`; `fileName`/`sha1`/`releaseDate` patched (grep them).

- [ ] **Step 7: Commit + tag + push**

```bash
git add src/module/NukeXVersion.h CHANGELOG.md README.md .gitignore repository/updates.xri repository/*-linux-x64-NukeX.tar.gz
git commit -m "$(cat <<'EOF'
release: v5.0.0.0 — colour-science overhaul

Filter taxonomy + QE-driven Q-solve + Lab/LCH ColorComposer with a
calibrated emission-line palette. Fixes the v4 green cast on dual-NB OSC
data at the root (stacking_engine.cpp hard-coded OSC_RGB for all Bayer).
Ships share/qe_database.json (55 cameras + generic, 96 filters). Rating
DB user_version 1 -> 2. See CHANGELOG.md.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01NqDedRCQxJy8EbbzWDKMFR
EOF
)"
git tag -a v5.0.0.0 -m "NukeX v5.0.0.0 — colour-science overhaul"
git push origin main && git push origin v5.0.0.0
```

- [ ] **Step 8: Prove the published package installs through the updater**

Remove the dev-staged copies so the proof is real: `sudo rm -f /opt/PixInsight/share/qe_database.json`, and in PixInsight uninstall the dev module (Process → Modules → Install Modules… → remove `NukeX-pxm.so` entry). Then Resources → Updates → Manage Repositories → add the URL from README → Check for Updates → install → restart PixInsight.

Verify:
```bash
ls -la /opt/PixInsight/share/qe_database.json /opt/PixInsight/bin/NukeX-pxm.so
```
and in PixInsight, Process Explorer → NukeX → version reads `5.0.0.0`. Run one 12-frame M16 HaO3 stack from the interface: the Process Console must show no QE-database error and the `NukeX_composed` window must appear. If `share/qe_database.json` is absent after the updater install, the tarball layout is not honoured by PixInsight's updater — that is a release blocker: the fix is to ship the JSON under `bin/` and change `PIShareRoot()` accordingly, then re-package; report which happened.

---

### Task 24: Memory closeout

**Files (all under `~/.claude/projects/-home-scarter4work-projects-nukex5/memory/`):**
- Create: `project_v5000_closeout.md`
- Modify: `MEMORY.md` (replace the "RESUME HERE" line), `project_v5_session_pause_2026-05-01.md` (mark superseded), `project_color_science_brainstorm.md` (checklist items 8–10 → `[x]`)

- [ ] **Step 1: Write the closeout memory**

```markdown
---
name: v5.0.0.0 Color Science Overhaul Closeout
description: v5.0.0.0 SHIPPED <date> from github.com/scarter4work/nukex5. Filter taxonomy + QE Q-solve + Lab/LCH composer; M16 HaO3 green cast fixed; QE DB ships under <PI>/share. Rating DB user_version 2.
type: project
---

**Status <date>:** v5.0.0.0 shipped; updater install verified on this machine (bin/ + share/ delivered).

## What changed vs the April plan
- Task 14 mapping corrected (rating-axis codes, not enum ints); rating DB user_version 1 → 2.
- 14b: broadband LPR names resolve by sensor type (M16 LPro frames stack as OSC again).
- 15: transform derives canonical HaO3/S2O3/… from product entries; ships generic_sony_imx_osc.
- 15b: INSTRUME normalisation + substring resolution; spec 6.3 generic fallback + NUKEX_QE_CONFIDENCE.
- 16: qe_database_path from PixInsightSettings "Application/BaseDirectory"; release.sh ships share/.
- 21: spec's "M27 HaO3" corpus does not exist; M16 HaO3 (~/projects/processing/M16, *HaO3*.fit) is the dual-NB corpus. v4 LRGB-mono golden preserved bit-identical.

## Real-data validation
- M16 HaO3 12 frames: <verdict from visual-evidence/README.md>
- M27 LRGB-mono 2025 (ATR585M): <verdict>
- M27 OSC 2023 (ASI2400MC): <verdict>

## Environment
- Fedora 44 / GCC 16.2.1 / glog 0.7.1: stale build dirs from before the OS upgrade fail on libglog.so.0 — clean rebuild.

## Outcome, 2026-09-04

Tasks 21 and 22 completed differently from the plan, because running the new
corpora on a cube that finally fit in RAM exposed three defects the plan did
not know about. All three were root-caused with measurements, not guessed.

**Task 21 — E2E.** Three of four cases are green.

| case | outcome |
|---|---|
| `lrgb_mono_ngc7635` | verified bit-identical against the frozen golden; `golden_frozen` stays true |
| `bayer_rgb_m27_2023` | golden recorded, 33 of 33 frames aligned |
| `bayer_nb_hao3_m16` | golden recorded, 12 of 12 frames aligned |
| `mono_lrgb_m27_2025` | `skip: true` — the engine refuses multi-filter mono batches |

The harness was also fixed to honour each case's `min_frames_ok_alignment`,
which every case declared and the code ignored in favour of demanding zero
failures.

**Task 22 — visual validation.** The M27 LRGB-mono row is struck: there is no
v5 image to judge, because the configuration is refused. M16 HaO3 and M27 OSC
proceed as written.

**Defects found and their disposition.**

1. *Heap corruption on a mixed-filter batch* — fixed (`6df32dd`). The cube was
   allocated from the first frame and its channel config grown underneath it;
   that only ever worked because the voxel over-provisioned MAX_CHANNELS. The
   slot union is now settled before allocation.
2. *LRGB-mono produces one populated channel of four* — contained, not fixed.
   FrameCache is keyed on geometry so all mono filters share one cache, and
   Phase B assumes every channel has the same frame set. The engine refuses
   such batches; un-refusing them is the next task.
3. *M27 2025 aligns 51 of 72 even after reference selection* — diagnosed as
   cumulative drift at the two temporal ends of a seven-hour session. Needs
   chained or multi-reference alignment.

Also worth noting for whoever picks this up: both integration test binaries
were tagged `[.integration]`, which Catch2 hides, so ctest had never run a
single case in either. That is why a heap corruption survived a green suite.
They are un-gated and cost 0.8 s together.

## Open follow-ups (each needs a decision, per no-stub-tickets)
- S2O3 and LRGBSHO corpora: none on disk; E2E placeholders skip.
- Per-camera Q dispatch for mixed-camera dual-NB batches: engine loud-fails today.
```

- [ ] **Step 2: Index + supersede**

In `MEMORY.md` replace the `project_v5_session_pause_2026-05-01.md` line with `- [project_v5000_closeout.md](project_v5000_closeout.md) — v5.0.0.0 SHIPPED <date>: colour-science overhaul; QE DB under <PI>/share; rating DB v2; M16 HaO3 validated.` and append `- [project_v5_session_pause_2026-05-01.md](…) — superseded by the closeout; keeps the Task 14 mapping rationale.` Prepend `**SUPERSEDED <date> by project_v5000_closeout.md.**` to the pause memory. Tick items 8–10 in `project_color_science_brainstorm.md`.

No commit — memory files are outside the repo.

---

## Self-Review

**Spec coverage (remaining sections):**

| Spec | Task |
|---|---|
| §4.1 `share/qe_database.json`, `tools/import_qe_research.py` | 15, 16 |
| §4.2 rating DB migration, RatingDialog, auto-selector | 14 |
| §4.3 deletions | 18, 19 |
| §6.3 unknown FILTER + Bayer loud fail; unknown INSTRUME → generic + warning | 17 (message), 15b (fallback — previously unimplemented) |
| §6.3 known broadband names on Bayer | 14b (gap the spec table implies but the classifier lacked) |
| §7.2 integration tests | 20 |
| §7.3 E2E preserved floor + new baselines | 21 |
| §7.4 real-data validation + visual evidence | 22 |
| §7.5 performance budget | 21 Step 4 wall-time budgets; Phase B ≤10% checked by comparing `elapsed_s` of `lrgb_mono_ngc7635` against `test/fixtures/phaseB_baseline_ms.txt` in the same run |
| release + memory | 23, 24 |

**Placeholder scan:** none. Every code step shows the code; every run step has an expected output. Data-dependent counts (mono cameras without `mono_pk`) are stated as informational.

**Type consistency:** `FrameMetadata{filter, bayer_pattern, instrument}` used identically in Tasks 14, 14b, 20. `QEDatabase::resolve_camera / normalize_camera_key / kGenericOSCCamera` defined in 15b, consumed in 15b (engine), 16 (probe), 20 (fixture). `ExecuteResult::qe_generic_camera_fallback` defined in 15b, consumed in 15b (module keyword) and 20 (integration test). `write_synthetic_*` signatures identical in 20's header, unit test, and the pre-existing integration bodies (the `instrume` default keeps the six-argument calls valid). `kRatingDbSchemaVersion` / `rating_db_schema_version` defined in 14 Step 3, used in 14 Steps 1 and 5.

**Known risks, stated:**
- Task 20 Step 6: the Q-solve ±0.02 assertions were written in Task 10 against the engine's numeric path without ever running; if they fail, the instruction is to root-cause in the engine, not relax the tolerance.
- Task 21 Step 1: the v4 golden must still match on the L-only path. If it does not, that is the first defect to fix and it blocks everything after.
- Task 23 Step 8: whether PixInsight's updater extracts `share/` beside `bin/` is proven only by the live install; the fallback layout is spelled out.
