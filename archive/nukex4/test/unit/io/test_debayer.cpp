#include "catch_amalgamated.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>

using namespace nukex;

TEST_CASE("Debayer: RGGB synthetic 4x4 pattern", "[debayer]") {
    // Create a synthetic RGGB Bayer pattern:
    // Row 0: R  G  R  G
    // Row 1: G  B  G  B
    // Row 2: R  G  R  G
    // Row 3: G  B  G  B
    //
    // R pixels = 0.8, G pixels = 0.5, B pixels = 0.2
    Image raw(4, 4, 1);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);
            if (even_x && even_y)       raw.at(x, y, 0) = 0.8f;  // R
            else if (!even_x && even_y) raw.at(x, y, 0) = 0.5f;  // G on R row
            else if (even_x && !even_y) raw.at(x, y, 0) = 0.5f;  // G on B row
            else                        raw.at(x, y, 0) = 0.2f;  // B
        }
    }

    Image rgb = DebayerEngine::debayer(raw, BayerPattern::RGGB);

    REQUIRE(rgb.width() == 4);
    REQUIRE(rgb.height() == 4);
    REQUIRE(rgb.n_channels() == 3);

    // At an interior R pixel (2,2): R should be exact, G/B from true neighbors
    REQUIRE(rgb.at(2, 2, 0) == Catch::Approx(0.8f));   // R direct
    REQUIRE(rgb.at(2, 2, 1) == Catch::Approx(0.5f));   // G interpolated (neighbors are G=0.5)
    REQUIRE(rgb.at(2, 2, 2) == Catch::Approx(0.2f));   // B interpolated (diagonals are B=0.2)

    // At a B pixel (1,1): all 4 diagonal R neighbors and 4 cardinal G neighbors are interior
    REQUIRE(rgb.at(1, 1, 2) == Catch::Approx(0.2f));   // B direct
    REQUIRE(rgb.at(1, 1, 1) == Catch::Approx(0.5f));   // G interpolated
    REQUIRE(rgb.at(1, 1, 0) == Catch::Approx(0.8f));   // R interpolated

    // Interior G pixel on R row (1,0): G direct
    REQUIRE(rgb.at(1, 0, 1) == Catch::Approx(0.5f));   // G direct

    // Interior G pixel on B row (0,1): G direct
    REQUIRE(rgb.at(0, 1, 1) == Catch::Approx(0.5f));   // G direct
}

TEST_CASE("Debayer: output preserves constant image", "[debayer]") {
    // A constant image should debayer to the same constant in all channels
    Image raw(20, 20, 1);
    raw.fill(0.42f);

    Image rgb = DebayerEngine::debayer(raw, BayerPattern::RGGB);

    for (int y = 1; y < 19; y++) {  // skip edges (boundary effects)
        for (int x = 1; x < 19; x++) {
            REQUIRE(rgb.at(x, y, 0) == Catch::Approx(0.42f).margin(0.001f));
            REQUIRE(rgb.at(x, y, 1) == Catch::Approx(0.42f).margin(0.001f));
            REQUIRE(rgb.at(x, y, 2) == Catch::Approx(0.42f).margin(0.001f));
        }
    }
}

TEST_CASE("Debayer: all four Bayer patterns produce 3-channel output", "[debayer]") {
    Image raw(8, 8, 1);
    raw.fill(0.5f);

    for (auto pattern : {BayerPattern::RGGB, BayerPattern::BGGR,
                          BayerPattern::GRBG, BayerPattern::GBRG}) {
        Image rgb = DebayerEngine::debayer(raw, pattern);
        REQUIRE(rgb.n_channels() == 3);
        REQUIRE(rgb.width() == 8);
        REQUIRE(rgb.height() == 8);
    }
}

TEST_CASE("Debayer: throws on non-single-channel input", "[debayer]") {
    Image rgb(8, 8, 3);
    REQUIRE_THROWS_AS(
        DebayerEngine::debayer(rgb, BayerPattern::RGGB),
        std::invalid_argument
    );
}

TEST_CASE("Debayer: throws on NONE pattern", "[debayer]") {
    Image raw(8, 8, 1);
    REQUIRE_THROWS_AS(
        DebayerEngine::debayer(raw, BayerPattern::NONE),
        std::invalid_argument
    );
}

TEST_CASE("Debayer: real FITS file debayer", "[debayer][integration]") {
    std::string path = "/home/scarter4work/projects/processing/M16/"
                       "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";
    if (!std::filesystem::exists(path)) {
        SKIP("Test FITS file not available");
    }

    auto result = FITSReader::read(path);
    REQUIRE(result.success);
    REQUIRE(result.image.n_channels() == 1);

    Image rgb = DebayerEngine::debayer(result.image, BayerPattern::RGGB);
    REQUIRE(rgb.n_channels() == 3);
    REQUIRE(rgb.width() == result.image.width());
    REQUIRE(rgb.height() == result.image.height());

    // Verify output is not all zeros and values are in [0, 1]
    float sum = 0.0f;
    for (int ch = 0; ch < 3; ch++) {
        for (int y = 100; y < 200; y++) {
            for (int x = 100; x < 200; x++) {
                float v = rgb.at(x, y, ch);
                REQUIRE(v >= 0.0f);
                REQUIRE(v <= 1.0f);
                sum += v;
            }
        }
    }
    REQUIRE(sum > 0.0f);  // not all black
}
