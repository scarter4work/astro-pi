#include "catch_amalgamated.hpp"
#include "nukex/calibration/qe_database.hpp"

#include <filesystem>
#include <fstream>

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

// ── Camera identification: ids + explicit aliases, never fuzzy ──

TEST_CASE("QEDatabase: normalize_camera_key keeps only lowercase ASCII alphanumerics",
          "[qe_database][camera_id]") {
    REQUIRE(QEDatabase::normalize_camera_key("ZWO ASI585MC Air") == "zwoasi585mcair");
    REQUIRE(QEDatabase::normalize_camera_key("  asi-585_mc ") == "asi585mc");
    REQUIRE(QEDatabase::normalize_camera_key("") == "");
    REQUIRE(QEDatabase::normalize_camera_key("--") == "");
}

TEST_CASE("QEDatabase: camera ids match case/punctuation-insensitively",
          "[qe_database][camera_id]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("minimal_db.json").string()).ok);
    REQUIRE(db.resolve_camera_id("ASI585MC")  == "ASI585MC");   // exact
    REQUIRE(db.resolve_camera_id("asi585mc")  == "ASI585MC");
    REQUIRE(db.resolve_camera_id("ASI-585MC") == "ASI585MC");
    REQUIRE(db.lookup_camera_qe("asi585mc", 656.3, Photosite::R) == Catch::Approx(0.73).margin(0.01));
}

TEST_CASE("QEDatabase: explicit aliases resolve; colour and mono never cross",
          "[qe_database][camera_id]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("aliases_db.json").string()).ok);

    REQUIRE(db.resolve_camera_id("ZWO ASI585MC Air")   == "asi585mc");
    REQUIRE(db.resolve_camera_id("zwo asi585mc pro")   == "asi585mc");
    REQUIRE(db.resolve_camera_id("ZWO ASI585MM Pro")   == "asi585mm");
    REQUIRE(db.lookup_camera_qe("ZWO ASI585MC Air", 656.3, Photosite::R) == Catch::Approx(0.73));

    // Not listed -> unknown. No prefix/suffix stripping, no nearest match.
    REQUIRE(db.resolve_camera_id("ZWO ASI585MM Air").empty()); // not an alias of anything
    REQUIRE(db.resolve_camera_id("ZWO ASI585").empty());       // colour/mono ambiguous
    REQUIRE(db.resolve_camera_id("ASI585MC Air").empty());     // alias is the full string only
    REQUIRE(db.resolve_camera_id("").empty());
    REQUIRE_FALSE(db.has_camera("ZWO ASI585MM Air"));
    REQUIRE(db.confidence("ZWO ASI585MM Air") == QEConfidence::UNKNOWN);
    REQUIRE(db.lookup_camera_qe("ZWO ASI585MM Air", 656.3, Photosite::R) == 0.0);
}

TEST_CASE("QEDatabase: an alias claimed by two cameras is a loud load error",
          "[qe_database][camera_id]") {
    QEDatabase db;
    auto r = db.load_shipped(fixture("aliases_collision.json").string());
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("ambiguous") != std::string::npos);
    REQUIRE(r.error.find("zwoasi585pro") != std::string::npos);
    // Rejected document merged nothing.
    REQUIRE(db.n_cameras() == 0);
    REQUIRE_FALSE(db.has_camera("asi585mc"));
}

TEST_CASE("QEDatabase: override naming an existing id case-insensitively replaces that record",
          "[qe_database][camera_id]") {
    QEDatabase db;
    REQUIRE(db.load_shipped(fixture("aliases_db.json").string()).ok);
    const int n_before = db.n_cameras();

    auto path = fs::temp_directory_path() / "qe_alias_override.json";
    {
        std::ofstream f(path);
        f << R"({ "cameras": {
            "ASI585MC": { "sensor": "IMX585", "type": "OSC", "bayer": "RGGB",
                          "qe": { "656": { "R": 0.99, "G": 0.10, "B": 0.01 } },
                          "confidence": "low" },
            "ZWO ASI585MM Pro": { "sensor": "IMX585", "type": "mono",
                          "qe": { "656": { "mono_pk": 0.42 } }, "confidence": "low" }
        } })";
    }
    REQUIRE(db.load_override(path.string()).ok);

    // "ASI585MC" == id asi585mc -> replaced in place, aliases still reach it.
    REQUIRE(db.resolve_camera_id("ASI585MC") == "asi585mc");
    REQUIRE(db.lookup_camera_qe("ZWO ASI585MC Air", 656.3, Photosite::R) == Catch::Approx(0.99));
    // Override naming an existing ALIAS becomes its own record; the aliased
    // camera's data under its id is untouched.
    REQUIRE(db.resolve_camera_id("ZWO ASI585MM Pro") == "ZWO ASI585MM Pro");
    REQUIRE(db.lookup_camera_qe("ZWO ASI585MM Pro", 656.3, Photosite::MONO_PEAK) == Catch::Approx(0.42));
    REQUIRE(db.lookup_camera_qe("asi585mm", 656.3, Photosite::MONO_PEAK) == Catch::Approx(0.75));
    REQUIRE(db.n_cameras() == n_before + 1);
}
