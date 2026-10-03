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
