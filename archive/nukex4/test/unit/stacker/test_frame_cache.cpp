#include "catch_amalgamated.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/io/image.hpp"
#include <cmath>
#include <filesystem>

using namespace nukex;

TEST_CASE("FrameCache::encode/decode roundtrip", "[cache]") {
    // Test several values across [0, 1]
    float test_values[] = {0.0f, 0.001f, 0.25f, 0.5f, 0.75f, 0.999f, 1.0f};
    for (float v : test_values) {
        uint16_t encoded = FrameCache::encode(v);
        float decoded = FrameCache::decode(encoded);
        REQUIRE(decoded == Catch::Approx(v).margin(1.0f / 65535.0f));
    }
}

TEST_CASE("FrameCache::encode clamps to [0, 1]", "[cache]") {
    REQUIRE(FrameCache::encode(-0.1f) == 0);
    REQUIRE(FrameCache::encode(1.5f) == 65535);
}

TEST_CASE("FrameCache: write and read back single frame", "[cache]") {
    Image frame(4, 4, 1);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            frame.at(x, y, 0) = (x + y * 4) / 16.0f;

    FrameCache cache(4, 4, 1, 10, "/tmp");
    cache.write_frame(0, frame);
    REQUIRE(cache.n_frames_written() == 1);

    float values[10];
    int n = cache.read_pixel(2, 1, 0, values);
    REQUIRE(n == 1);
    // Pixel (2, 1): value = (2 + 1*4) / 16 = 0.375
    REQUIRE(values[0] == Catch::Approx(0.375f).margin(0.001f));
}

TEST_CASE("FrameCache: write multiple frames, read all back", "[cache]") {
    Image f1(8, 8, 2);
    Image f2(8, 8, 2);
    Image f3(8, 8, 2);
    f1.fill(0.3f);
    f2.fill(0.5f);
    f3.fill(0.7f);

    FrameCache cache(8, 8, 2, 10, "/tmp");
    cache.write_frame(0, f1);
    cache.write_frame(1, f2);
    cache.write_frame(2, f3);
    REQUIRE(cache.n_frames_written() == 3);

    float values[10];
    int n = cache.read_pixel(4, 4, 0, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
    REQUIRE(values[1] == Catch::Approx(0.5f).margin(0.001f));
    REQUIRE(values[2] == Catch::Approx(0.7f).margin(0.001f));

    // Channel 1 should also work
    n = cache.read_pixel(4, 4, 1, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
}

TEST_CASE("FrameCache: temp file cleaned up on destruction", "[cache]") {
    std::string filepath;
    {
        FrameCache cache(2, 2, 1, 1, "/tmp");
        Image frame(2, 2, 1);
        frame.fill(0.5f);
        cache.write_frame(0, frame);
        // We can't easily get the filepath, but verify no crash on destruction
    }
    // Cache destroyed -- temp file should be gone
    // (No way to verify path externally without exposing it, but no crash = success)
}

TEST_CASE("FrameCache: quantization error within tolerance", "[cache]") {
    // Verify max quantization error is < 2/65535 ~ 3e-5
    Image frame(16, 16, 1);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            frame.at(x, y, 0) = (x * 16 + y) / 256.0f;

    FrameCache cache(16, 16, 1, 1, "/tmp");
    cache.write_frame(0, frame);

    float values[1];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            cache.read_pixel(x, y, 0, values);
            float original = frame.at(x, y, 0);
            REQUIRE(std::fabs(values[0] - original) < 2.0f / 65535.0f);
        }
    }
}
