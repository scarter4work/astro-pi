#include "catch_amalgamated.hpp"
#include "nukex/learning/rating_db.hpp"

#include <sqlite3.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace nukex::learning;
namespace fs = std::filesystem;

namespace {
fs::path tmp_db_path(const std::string& suffix) {
    fs::path p = fs::temp_directory_path() / ("nukex_test_rating_" + suffix + ".sqlite");
    fs::remove(p);
    return p;
}
} // namespace

TEST_CASE("open_rating_db: creates fresh DB with schema", "[learning][rating_db]") {
    const auto path = tmp_db_path("fresh");

    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);

    // runs table must exist
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='runs';";
    REQUIRE(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    // user_version == 1
    stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("open_rating_db: reopens existing DB without clobbering data", "[learning][rating_db]") {
    const auto path = tmp_db_path("reopen");

    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    char* err = nullptr;
    REQUIRE(sqlite3_exec(db,
        "INSERT INTO runs(run_id, created_at, stretch_name, target_class, filter_class,"
        " params_json, rating_brightness, rating_saturation, rating_star_bloat, rating_overall)"
        " VALUES(randomblob(16), 1700000000, 'VeraLux', 0, 0, '{}', 0, 0, 0, 3);",
        nullptr, nullptr, &err) == SQLITE_OK);
    close_rating_db(db);

    // Reopen --> row is still present
    db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT count(*) FROM runs;", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("open_rating_db: corrupt DB is renamed and fresh DB created", "[learning][rating_db]") {
    const auto path = tmp_db_path("corrupt");

    // Write garbage to the DB path
    {
        std::ofstream f(path);
        f << "this is not a SQLite file";
    }

    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);

    // New DB at original path has the runs table
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='runs';",
        -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    REQUIRE(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    // A .corrupt.<ts> sibling exists
    bool found_corrupt_sibling = false;
    for (const auto& entry : fs::directory_iterator(path.parent_path())) {
        const auto n = entry.path().filename().string();
        if (n.find(path.filename().string() + ".corrupt.") == 0) {
            found_corrupt_sibling = true;
            fs::remove(entry.path());
            break;
        }
    }
    REQUIRE(found_corrupt_sibling);

    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("open_rating_db: unwritable path returns nullptr", "[learning][rating_db]") {
    // A path under /proc is not a writable file location.
    sqlite3* db = open_rating_db("/proc/this_cannot_be_a_db");
    REQUIRE(db == nullptr);
}

namespace {
RunRecord make_record(const std::string& stretch) {
    RunRecord r;
    for (int i = 0; i < 16; ++i) r.run_id[i] = static_cast<std::uint8_t>(i);
    r.created_at_unix = 1700000000;
    r.stretch_name = stretch;
    r.target_class = 1;
    r.filter_class = 0;
    r.per_channel_stats.fill(0.5);
    r.bright_concentration = 0.3;
    r.color_rg = 1.0;
    r.color_bg = 1.0;
    r.fwhm_median = 3.2;
    r.star_count = 250;
    r.params_json = R"({"log_D":2.1,"protect_b":6.0,"convergence_power":3.5})";
    r.rating_brightness = 1;
    r.rating_saturation = 0;
    r.rating_color = 0;
    r.rating_star_bloat = -1;
    r.rating_overall = 4;
    return r;
}
} // namespace

TEST_CASE("insert_run: round-trip via select_runs_for_stretch", "[learning][rating_db]") {
    const auto path = tmp_db_path("insert");
    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);

    auto rec = make_record("VeraLux");
    REQUIRE(insert_run(db, rec));

    auto rows = select_runs_for_stretch(db, "VeraLux");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].stretch_name == "VeraLux");
    REQUIRE(rows[0].target_class == 1);
    REQUIRE(rows[0].rating_overall == 4);
    REQUIRE(rows[0].params_json.find("log_D") != std::string::npos);

    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("select_runs_for_stretch: per-stretch isolation", "[learning][rating_db]") {
    const auto path = tmp_db_path("isolated");
    sqlite3* db = open_rating_db(path.string());
    auto v = make_record("VeraLux"); v.run_id[0] = 0xAA;
    auto g = make_record("GHS");     g.run_id[0] = 0xBB;
    REQUIRE(insert_run(db, v));
    REQUIRE(insert_run(db, g));

    REQUIRE(select_runs_for_stretch(db, "VeraLux").size() == 1);
    REQUIRE(select_runs_for_stretch(db, "GHS").size() == 1);
    REQUIRE(select_runs_for_stretch(db, "MTF").empty());

    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("attach_bootstrap: unions user + bootstrap rows for a stretch",
          "[learning][rating_db]") {
    const auto user_path      = tmp_db_path("user");
    const auto bootstrap_path = tmp_db_path("bootstrap");

    // Seed bootstrap with one VeraLux row
    {
        sqlite3* b = open_rating_db(bootstrap_path.string());
        auto r = make_record("VeraLux"); r.run_id[0] = 0x11;
        REQUIRE(insert_run(b, r));
        close_rating_db(b);
    }

    sqlite3* user = open_rating_db(user_path.string());
    auto u = make_record("VeraLux"); u.run_id[0] = 0x22;
    REQUIRE(insert_run(user, u));

    REQUIRE(attach_bootstrap(user, bootstrap_path.string()));
    auto rows = select_runs_for_stretch(user, "VeraLux");
    REQUIRE(rows.size() == 2);

    close_rating_db(user);
    fs::remove(user_path);
    fs::remove(bootstrap_path);
}

TEST_CASE("attach_bootstrap: missing file is a no-op returning true",
          "[learning][rating_db]") {
    const auto user_path = tmp_db_path("nobootstrap");
    sqlite3* user = open_rating_db(user_path.string());
    REQUIRE(attach_bootstrap(user, "/tmp/does_not_exist.sqlite"));
    close_rating_db(user);
    fs::remove(user_path);
}
