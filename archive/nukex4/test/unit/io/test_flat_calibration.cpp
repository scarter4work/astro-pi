#include "catch_amalgamated.hpp"
#include "nukex/io/flat_calibration.hpp"

using namespace nukex;

TEST_CASE("FlatCalibration: median of known values", "[flat]") {
    Image img(5, 1, 1);
    img.at(0, 0, 0) = 1.0f;
    img.at(1, 0, 0) = 3.0f;
    img.at(2, 0, 0) = 2.0f;
    img.at(3, 0, 0) = 5.0f;
    img.at(4, 0, 0) = 4.0f;
    // Sorted: 1, 2, 3, 4, 5 → median = 3
    REQUIRE(FlatCalibration::median(img) == Catch::Approx(3.0f));
}

TEST_CASE("FlatCalibration: median of even count", "[flat]") {
    Image img(4, 1, 1);
    img.at(0, 0, 0) = 1.0f;
    img.at(1, 0, 0) = 2.0f;
    img.at(2, 0, 0) = 3.0f;
    img.at(3, 0, 0) = 4.0f;
    // Sorted: 1, 2, 3, 4 → median = (2+3)/2 = 2.5
    REQUIRE(FlatCalibration::median(img) == Catch::Approx(2.5f));
}

TEST_CASE("FlatCalibration: apply corrects vignetting", "[flat]") {
    // Simulate vignetting: center bright, edges dim
    Image light(5, 5, 1);
    Image flat(5, 5, 1);

    // Flat has center = 1.0, edges = 0.5 (simulating vignetting)
    flat.fill(1.0f);
    flat.at(0, 0, 0) = 0.5f;
    flat.at(4, 0, 0) = 0.5f;
    flat.at(0, 4, 0) = 0.5f;
    flat.at(4, 4, 0) = 0.5f;

    // Light has uniform true signal of 0.6
    // But edges appear dimmer due to vignetting: 0.6 * 0.5 = 0.3
    light.fill(0.6f);
    light.at(0, 0, 0) = 0.3f;
    light.at(4, 0, 0) = 0.3f;
    light.at(0, 4, 0) = 0.3f;
    light.at(4, 4, 0) = 0.3f;

    FlatCalibration::apply(light, flat);

    // After correction, all pixels should be ~0.6
    REQUIRE(light.at(2, 2, 0) == Catch::Approx(0.6f));
    REQUIRE(light.at(0, 0, 0) == Catch::Approx(0.6f));
    REQUIRE(light.at(4, 4, 0) == Catch::Approx(0.6f));
}

TEST_CASE("FlatCalibration: min_flat_value prevents divide by near-zero", "[flat]") {
    Image light(3, 3, 1);
    light.fill(0.5f);

    Image flat(3, 3, 1);
    flat.fill(1.0f);
    flat.at(1, 1, 0) = 0.001f;  // Near-zero (dust mote)

    FlatCalibration::apply(light, flat, 0.01f);

    // The near-zero pixel should be clamped to 0.01, giving 0.5/0.01 = 50
    REQUIRE(light.at(1, 1, 0) == Catch::Approx(50.0f));
    // Normal pixels: 0.5 / 1.0 = 0.5
    REQUIRE(light.at(0, 0, 0) == Catch::Approx(0.5f));
}

TEST_CASE("FlatCalibration: dimension mismatch throws", "[flat]") {
    Image light(10, 10, 1);
    Image flat(20, 20, 1);

    REQUIRE_THROWS_AS(
        FlatCalibration::apply(light, flat),
        std::invalid_argument
    );
}

TEST_CASE("FlatCalibration: single-channel flat applied to multi-channel light", "[flat]") {
    Image light(4, 4, 3);
    light.fill(0.8f);

    Image flat(4, 4, 1);
    flat.fill(0.5f);

    FlatCalibration::apply(light, flat);

    // 0.8 / 0.5 = 1.6 for all channels
    for (int ch = 0; ch < 3; ch++) {
        REQUIRE(light.at(2, 2, ch) == Catch::Approx(1.6f));
    }
}
