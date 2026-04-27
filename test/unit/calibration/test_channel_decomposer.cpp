#include "catch_amalgamated.hpp"
#include "nukex/calibration/channel_decomposer.hpp"
#include "nukex/calibration/qe_database.hpp"

#include <filesystem>
#include <fstream>

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

TEST_CASE("ChannelDecomposer: synthetic Hα+OIII recovered to within 1e-6",
          "[decomposer]") {
    QEDatabase db = loaded_db();
    ChannelDecomposer d(db);

    // Inject: Ha=0.5, OIII=0.3 → predict (R, G, B) via Q, then solve back
    auto Q = d.build_q("ASI585MC", "HaO3");
    Eigen::Vector2d truth(0.5, 0.3);
    Eigen::Vector3d rgb = Q * truth;

    Eigen::VectorXd recovered = d.solve("ASI585MC", "HaO3", rgb);
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
    Eigen::VectorXd recovered = d.solve("ASI585MC", "S2O3", rgb);
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
