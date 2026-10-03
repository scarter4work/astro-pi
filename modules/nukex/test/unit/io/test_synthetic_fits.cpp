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
