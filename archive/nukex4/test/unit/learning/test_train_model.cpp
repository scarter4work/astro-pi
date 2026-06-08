#include "catch_amalgamated.hpp"
#include "nukex/learning/rating_db.hpp"
#include "nukex/learning/train_model.hpp"

#include <sqlite3.h>
#include <filesystem>

using namespace nukex::learning;
namespace fs = std::filesystem;

namespace {
fs::path tmp_train_db() {
    auto p = fs::temp_directory_path() / "nukex_test_train.sqlite";
    fs::remove(p);
    return p;
}

void seed_veralux_rows(sqlite3* db, int n) {
    for (int i = 0; i < n; ++i) {
        RunRecord r;
        r.run_id[0] = static_cast<std::uint8_t>(i);
        r.created_at_unix = 1700000000 + i;
        r.stretch_name = "VeraLux";
        r.target_class = 0;
        r.filter_class = 0;
        r.per_channel_stats.fill(0.5 + 0.01 * i);
        r.bright_concentration = 0.2 + 0.01 * i;
        r.color_rg = 1.0;
        r.color_bg = 1.0;
        r.fwhm_median = 3.0;
        r.star_count = 100 + i;
        r.params_json = R"({"log_D": )" + std::to_string(2.0 + 0.1 * i)
                      + R"(, "protect_b": 6.0, "convergence_power": 3.5})";
        r.rating_brightness = 0;
        r.rating_saturation = 0;
        r.rating_color = 0;
        r.rating_star_bloat = 0;
        r.rating_overall = 4;
        REQUIRE(insert_run(db, r));
    }
}
} // namespace

TEST_CASE("train_one_stretch: zero rows returns empty per_param",
          "[learning][train_model]") {
    auto path = tmp_train_db();
    sqlite3* db = open_rating_db(path.string());
    auto s = train_one_stretch(db, "VeraLux", 1.0);
    REQUIRE(s.per_param.empty());
    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("train_one_stretch: below min_rows returns empty",
          "[learning][train_model]") {
    auto path = tmp_train_db();
    sqlite3* db = open_rating_db(path.string());
    seed_veralux_rows(db, 5);
    auto s = train_one_stretch(db, "VeraLux", 1.0, 8);
    REQUIRE(s.per_param.empty());
    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("train_one_stretch: fits one coefficient set per param",
          "[learning][train_model]") {
    auto path = tmp_train_db();
    sqlite3* db = open_rating_db(path.string());
    seed_veralux_rows(db, 20);

    auto s = train_one_stretch(db, "VeraLux", 1.0, 8);
    REQUIRE(s.stretch_name == "VeraLux");
    REQUIRE(s.per_param.count("log_D") == 1);
    REQUIRE(s.per_param.count("protect_b") == 1);
    REQUIRE(s.per_param.count("convergence_power") == 1);
    REQUIRE(s.per_param.at("log_D").n_train_rows == 20);
    REQUIRE(s.per_param.at("log_D").coefficients.size() > 0);
    close_rating_db(db);
    fs::remove(path);
}

TEST_CASE("train_one_stretch: rows for other stretches are ignored",
          "[learning][train_model]") {
    auto path = tmp_train_db();
    sqlite3* db = open_rating_db(path.string());
    seed_veralux_rows(db, 20);
    // Insert 20 GHS rows as well
    for (int i = 0; i < 20; ++i) {
        RunRecord r;
        r.run_id[0] = static_cast<std::uint8_t>(100 + i);
        r.stretch_name = "GHS";
        r.params_json = R"({"stretch": 6.0, "protect": 0.25, "symmetry": 0.5})";
        r.rating_brightness = 0; r.rating_saturation = 0;
        r.rating_color = 0;      r.rating_star_bloat = 0; r.rating_overall = 3;
        r.per_channel_stats.fill(0.5);
        r.color_rg = 1.0; r.color_bg = 1.0;
        REQUIRE(insert_run(db, r));
    }

    auto v = train_one_stretch(db, "VeraLux", 1.0, 8);
    auto g = train_one_stretch(db, "GHS",     1.0, 8);
    REQUIRE(v.per_param.count("log_D")    == 1);
    REQUIRE(v.per_param.count("stretch")  == 0);  // not a VeraLux param name
    REQUIRE(g.per_param.count("stretch")  == 1);
    REQUIRE(g.per_param.count("log_D")    == 0);
    close_rating_db(db);
    fs::remove(path);
}
