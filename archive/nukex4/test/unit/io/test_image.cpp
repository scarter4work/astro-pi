#include "catch_amalgamated.hpp"
#include "nukex/io/image.hpp"
#include <cmath>

using nukex::Image;

TEST_CASE("Image: construction with dimensions", "[image]") {
    Image img(100, 80, 3);
    REQUIRE(img.width() == 100);
    REQUIRE(img.height() == 80);
    REQUIRE(img.n_channels() == 3);
    REQUIRE(img.total_pixels() == 8000);
    REQUIRE(img.data_size() == 24000);
    REQUIRE(img.empty() == false);
}

TEST_CASE("Image: default construction is empty", "[image]") {
    Image img;
    REQUIRE(img.width() == 0);
    REQUIRE(img.height() == 0);
    REQUIRE(img.empty() == true);
}

TEST_CASE("Image: initialized to zero", "[image]") {
    Image img(10, 10, 1);
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 10; x++)
            REQUIRE(img.at(x, y, 0) == 0.0f);
}

TEST_CASE("Image: pixel access read/write", "[image]") {
    Image img(10, 10, 3);
    img.at(5, 3, 0) = 0.5f;
    img.at(5, 3, 1) = 0.6f;
    img.at(5, 3, 2) = 0.7f;
    REQUIRE(img.at(5, 3, 0) == Catch::Approx(0.5f));
    REQUIRE(img.at(5, 3, 1) == Catch::Approx(0.6f));
    REQUIRE(img.at(5, 3, 2) == Catch::Approx(0.7f));
    // Other pixels unaffected
    REQUIRE(img.at(0, 0, 0) == 0.0f);
}

TEST_CASE("Image: channel_data returns correct pointer", "[image]") {
    Image img(4, 3, 2);
    img.at(2, 1, 0) = 1.0f;
    img.at(2, 1, 1) = 2.0f;
    // Channel 0 offset: y*width + x = 1*4 + 2 = 6
    REQUIRE(img.channel_data(0)[6] == Catch::Approx(1.0f));
    // Channel 1 offset: same position in channel 1's block
    REQUIRE(img.channel_data(1)[6] == Catch::Approx(2.0f));
}

TEST_CASE("Image: channel layout is channel-by-channel", "[image]") {
    Image img(2, 2, 3);
    // Set all pixels in channel 0 to 1, channel 1 to 2, channel 2 to 3
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            img.at(x, y, 0) = 1.0f;
            img.at(x, y, 1) = 2.0f;
            img.at(x, y, 2) = 3.0f;
        }
    // Raw data should be: [1,1,1,1, 2,2,2,2, 3,3,3,3]
    const float* d = img.data();
    for (int i = 0; i < 4; i++) REQUIRE(d[i] == Catch::Approx(1.0f));
    for (int i = 4; i < 8; i++) REQUIRE(d[i] == Catch::Approx(2.0f));
    for (int i = 8; i < 12; i++) REQUIRE(d[i] == Catch::Approx(3.0f));
}

TEST_CASE("Image: clone creates independent copy", "[image]") {
    Image img(5, 5, 1);
    img.at(2, 2, 0) = 42.0f;
    Image copy = img.clone();
    copy.at(2, 2, 0) = 99.0f;
    REQUIRE(img.at(2, 2, 0) == Catch::Approx(42.0f));
    REQUIRE(copy.at(2, 2, 0) == Catch::Approx(99.0f));
}

TEST_CASE("Image: fill sets all pixels", "[image]") {
    Image img(3, 3, 2);
    img.fill(0.5f);
    for (int ch = 0; ch < 2; ch++)
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++)
                REQUIRE(img.at(x, y, ch) == Catch::Approx(0.5f));
}

TEST_CASE("Image: apply transforms all pixels", "[image]") {
    Image img(3, 3, 1);
    img.fill(4.0f);
    img.apply([](float x) { return x * 2.0f; });
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            REQUIRE(img.at(x, y, 0) == Catch::Approx(8.0f));
}

TEST_CASE("Image: move semantics", "[image]") {
    Image img(10, 10, 1);
    img.at(5, 5, 0) = 1.0f;
    Image moved = std::move(img);
    REQUIRE(moved.at(5, 5, 0) == Catch::Approx(1.0f));
    REQUIRE(moved.width() == 10);
    REQUIRE(img.empty() == true);
}
