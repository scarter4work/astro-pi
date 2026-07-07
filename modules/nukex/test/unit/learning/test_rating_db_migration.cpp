// NukeX — rating DB schema v1 -> v2 migration test.
//
// v1 stored `runs.filter_class` in the OLD UI color-axis space {0,1,2}
// (see NukeXInstance.cpp's pre-migration filter_class_to_rating_int).
// v2 stores the NEW nukex::FilterClass identity codes. The migration is
// lossy on purpose -- see the remap table + rationale in rating_db.cpp.
//
// There are no legacy insert helpers (RunRecord always writes the current
// space), so this test builds a v1 DB the way the migration will find one
// in the wild: open a fresh DB (which creates the runs table), force
// PRAGMA user_version back to 1, and insert rows with old codes directly.
#include "catch_amalgamated.hpp"
#include "nukex/learning/rating_db.hpp"

#include <sqlite3.h>
#include <filesystem>
#include <string>
#include <vector>

using namespace nukex::learning;
namespace fs = std::filesystem;

namespace {

fs::path tmp_db_path(const std::string& suffix) {
    fs::path p = fs::temp_directory_path() / ("nukex_test_rating_migration_" + suffix + ".sqlite");
    fs::remove(p);
    return p;
}

// Insert a minimal legacy row satisfying every NOT NULL / CHECK constraint
// in the runs schema, with `filter_class` set to the given OLD color-axis
// code.
void insert_legacy_row(sqlite3* db, int filter_class) {
    const std::string sql =
        "INSERT INTO runs(run_id, created_at, stretch_name, target_class, filter_class,"
        " params_json, rating_brightness, rating_saturation, rating_star_bloat, rating_overall)"
        " VALUES(randomblob(16), 1700000000, 'VeraLux', 0, " + std::to_string(filter_class) +
        ", '{}', 0, 0, 0, 3);";
    char* err = nullptr;
    REQUIRE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK);
}

} // namespace

TEST_CASE("rating_db migration: v1 color-axis filter_class remaps to v2 identity codes",
          "[learning][rating_db][migration]") {
    const auto path = tmp_db_path("v1v2");

    // Arrange: a v1 DB "in the wild" -- schema present, user_version forced
    // to 1, three rows with old color-axis filter_class values {0,1,2}.
    {
        sqlite3* db = open_rating_db(path.string());
        REQUIRE(db != nullptr);
        char* err = nullptr;
        REQUIRE(sqlite3_exec(db, "PRAGMA user_version=1;", nullptr, nullptr, &err) == SQLITE_OK);
        insert_legacy_row(db, 0); // mono / LRGB-colour -> BROADBAND_L (1)
        insert_legacy_row(db, 1); // Bayer RGB          -> BROADBAND_OSC (3)
        insert_legacy_row(db, 2); // narrowband         -> NARROWBAND_SINGLE (4)
        close_rating_db(db);
    }

    // Act: reopening a v1 DB must run the migration in-place.
    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);

    // Assert: schema now reports v2.
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(stmt, 0) == 2);
    sqlite3_finalize(stmt);

    // Assert: stored filter_class values remapped 0,1,2 -> 1,3,4.
    REQUIRE(sqlite3_prepare_v2(db,
        "SELECT filter_class FROM runs ORDER BY filter_class;", -1, &stmt, nullptr) == SQLITE_OK);
    std::vector<int> got;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        got.push_back(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);
    REQUIRE(got == std::vector<int>{1, 3, 4});

    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("rating_db migration: reopening a fresh v2 DB is a no-op",
          "[learning][rating_db][migration]") {
    const auto path = tmp_db_path("v2noop");

    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    close_rating_db(db);

    db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(stmt, 0) == 2);
    sqlite3_finalize(stmt);

    close_rating_db(db);
    fs::remove(path);
}
