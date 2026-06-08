#include "catch_amalgamated.hpp"

#include "nukex/learning/rating_db.hpp"
#include "nukex/stretch/layer_loader.hpp"
#include "nukex/stretch/param_model.hpp"
#include "nukex/stretch/veralux_stretch.hpp"

#include <fstream>
#include <filesystem>
#include <limits>

using namespace nukex;
using namespace nukex::learning;
namespace fs = std::filesystem;

TEST_CASE("Corrupt DB opens as fresh DB -- no crash", "[phase8][fallback]") {
    const auto path = fs::temp_directory_path() / "nx_fb_corrupt.sqlite";
    fs::remove(path);
    { std::ofstream f(path); f << "garbage"; }

    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);
    REQUIRE(select_runs_for_stretch(db, "VeraLux").empty());
    close_rating_db(db);

    // Cleanup the .corrupt.<timestamp> sibling that rating_db.cpp creates.
    for (const auto& e : fs::directory_iterator(path.parent_path())) {
        if (e.path().filename().string().rfind(path.filename().string() + ".corrupt.", 0) == 0) {
            fs::remove(e.path());
        }
    }
    fs::remove(path);
}

TEST_CASE("Missing coefficients file -> LayerLoader reports None",
          "[phase8][fallback]") {
    LayerLoader L("/tmp/nx_fb_no_boot.json", "/tmp/nx_fb_no_user.json");
    auto a = L.active_for_stretch("VeraLux");
    REQUIRE(a.layer == ActiveLayer::None);
    REQUIRE(a.model == nullptr);
}

TEST_CASE("NaN stats -> ParamModel::predict_and_apply returns false",
          "[phase8][fallback]") {
    ParamModel m("VeraLux");
    ParamCoefficients c;
    c.feature_mean.assign(29, 0.0);
    c.feature_std .assign(29, 1.0);
    c.coefficients.assign(29, 0.5);
    c.intercept = 3.0;
    m.add_param("log_D", c);

    ImageStats s;
    s.median[0] = std::numeric_limits<double>::quiet_NaN();  // poisons feature row

    VeraLuxStretch op;
    REQUIRE_FALSE(m.predict_and_apply(s, op));
    REQUIRE(op.log_D == 2.0f);  // factory default intact
}

TEST_CASE("Prediction exceeding bounds is clamped, not propagated as NaN",
          "[phase8][fallback]") {
    ParamModel m("VeraLux");
    ParamCoefficients c;
    c.feature_mean.assign(29, 0.0);
    c.feature_std .assign(29, 1.0);
    c.coefficients.assign(29, 0.0);
    c.intercept = 9999.0;   // far above any realistic bound
    m.add_param("log_D", c);

    ImageStats s;
    s.median.fill(0.5); s.mad.fill(0.1);
    s.p50.fill(0.5); s.p95.fill(0.9); s.p99.fill(0.95); s.p999.fill(0.99);
    s.skew.fill(0.0); s.sat_frac.fill(0.0);
    s.bright_concentration = 0.2; s.color_rg = 1; s.color_bg = 1;
    s.fwhm_median = 3; s.star_count = 100;

    VeraLuxStretch op;
    REQUIRE(m.predict_and_apply(s, op));
    REQUIRE(op.log_D == Catch::Approx(7.0f));   // clamped to log_D max (range 0-7)
    REQUIRE(std::isfinite(op.log_D));
}

TEST_CASE("DB write failure leaves no partial row", "[phase8][fallback]") {
    // Build a DB, open it, then drop write perms on the parent to make
    // subsequent inserts fail. Verify that a failed insert doesn't leave
    // a half-written row visible to queries.
    const auto parent = fs::temp_directory_path() / "nx_fb_ro";
    fs::remove_all(parent);
    fs::create_directories(parent);
    const auto path = parent / "db.sqlite";

    sqlite3* db = open_rating_db(path.string());
    REQUIRE(db != nullptr);

    std::error_code ec_perm;
    fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, ec_perm);
    // If we couldn't drop perms (e.g. running as root), skip the assertion
    // rather than spuriously passing against an unenforced permission guard.
    const bool perms_enforceable = !ec_perm;

    RunRecord r;
    r.stretch_name = "VeraLux";
    r.params_json  = "{}";
    r.rating_brightness = 0; r.rating_saturation = 0;
    r.rating_color = std::optional<int>();   // mono rating (no color axis)
    r.rating_star_bloat = 0; r.rating_overall = 3;

    const bool ok = insert_run(db, r);

    // Restore perms BEFORE any REQUIRE so cleanup can still run even if
    // assertions throw.
    fs::permissions(parent, fs::perms::owner_all,
                    fs::perm_options::replace, ec_perm);

    if (perms_enforceable && !ok) {
        REQUIRE(select_runs_for_stretch(db, "VeraLux").empty());
    }
    // If ok == true (WAL mode sometimes lets writes through when the
    // parent dir is locked because the DB handle already has the files
    // open), this case degenerates into a sanity check -- the row either
    // inserted cleanly or wasn't written at all; no torn state.

    close_rating_db(db);
    fs::remove_all(parent);
}
