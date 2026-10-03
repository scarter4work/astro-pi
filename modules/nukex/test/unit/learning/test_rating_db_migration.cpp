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
