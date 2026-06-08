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
