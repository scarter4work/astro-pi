#include "catch_amalgamated.hpp"
#include "mini_fits_writer.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"

#include <filesystem>

namespace fs = std::filesystem;
using namespace nukex;

TEST_CASE("MiniFITSWriter: bayer frame round-trips through FITSReader",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_bayer.fits";
    test_util::write_synthetic_bayer(p.string(), 32, 16, "RGGB", "ASI585MC", "HaO3", 0.5f);

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.image.width()  == 32);
    REQUIRE(r.image.height() == 16);
    REQUIRE(r.image.n_channels() == 1); // raw mosaic, not pre-debayered
    REQUIRE(r.metadata.bayer_pattern == "RGGB");
    REQUIRE(r.metadata.instrument    == "ASI585MC");
    REQUIRE(r.metadata.filter        == "HaO3");

    // Debayering a uniform-fill mosaic must reconstruct the uniform value
    // in every reconstructed channel (constant field -> constant bilinear
    // interpolation).
    Image rgb = DebayerEngine::debayer(r.image, BayerPattern::RGGB);
    REQUIRE(rgb.n_channels() == 3);
    REQUIRE(rgb.at(5, 5, 0) == Catch::Approx(0.5f).margin(1e-6));
    REQUIRE(rgb.at(5, 5, 1) == Catch::Approx(0.5f).margin(1e-6));
    REQUIRE(rgb.at(5, 5, 2) == Catch::Approx(0.5f).margin(1e-6));
}

TEST_CASE("MiniFITSWriter: mono frame round-trips with no BAYERPAT",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_mono.fits";
    test_util::write_synthetic_mono(p.string(), 24, 12, "ASI2600MM", "L", 0.75f);

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.image.width()  == 24);
    REQUIRE(r.image.height() == 12);
    REQUIRE(r.image.n_channels() == 1);
    REQUIRE(r.metadata.bayer_pattern.empty());
    REQUIRE(r.metadata.instrument == "ASI2600MM");
    REQUIRE(r.metadata.filter     == "L");
    REQUIRE(r.image.at(3, 3, 0) == Catch::Approx(0.75f).margin(1e-6));
}

TEST_CASE("MiniFITSWriter: bayer frame with empty filter has no FILTER key",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_no_filter.fits";
    test_util::write_synthetic_bayer(p.string(), 16, 16, "RGGB", "ASI585MC", "", 0.4f);

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.metadata.filter.empty());
    REQUIRE(r.metadata.bayer_pattern == "RGGB");
}

TEST_CASE("MiniFITSWriter: q-solved HaO3 frame produces R/G/B that recover Ha+OIII",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_qsolve_hao3.fits";
    test_util::write_synthetic_q_solved_hao3(p.string(), 16, 16, "ASI585MC", 0.5f, 0.3f);

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.metadata.bayer_pattern == "RGGB");
    REQUIRE(r.metadata.instrument    == "ASI585MC");
    REQUIRE(r.metadata.filter        == "HaO3");

    // R photosite QE is ~0.73 at Ha and ~0.03 at OIII (minimal_db.json
    // fixture, linearly interpolated to the exact 656.3/500.7 nm line
    // wavelengths by QEDatabase::lookup_camera_qe -- hence the wider
    // margin instead of a tight hand-computed constant). Approximately
    // R = 0.73*0.5 + 0.03*0.3 = 0.374. The end-to-end Q-solve round-trip
    // (recovering Ha=0.5, OIII=0.3) is exercised by the Phase B
    // integration tests -- this just pins the raw-pixel math is in the
    // right ballpark and non-trivial (not a uniform fill).
    Image rgb = DebayerEngine::debayer(r.image, BayerPattern::RGGB);
    REQUIRE(rgb.at(8, 8, 0) == Catch::Approx(0.374f).margin(0.002f));
}

TEST_CASE("MiniFITSWriter: q-solved S2O3 frame carries FILTER=S2O3",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_qsolve_s2o3.fits";
    test_util::write_synthetic_q_solved_s2o3(p.string(), 16, 16, "ASI585MC", 0.4f, 0.2f);

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.metadata.filter == "S2O3");
}

TEST_CASE("MiniFITSWriter: negative-emission HaO3 frame carries FILTER=HaO3",
          "[mini_fits_writer]") {
    auto p = fs::temp_directory_path() / "mini_fits_negative.fits";
    test_util::write_synthetic_negative_emission_hao3(p.string(), 16, 16, "ASI585MC");

    auto r = FITSReader::read(p.string());
    REQUIRE(r.success);
    REQUIRE(r.metadata.filter == "HaO3");
}
