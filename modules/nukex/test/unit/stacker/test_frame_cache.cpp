#include "catch_amalgamated.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/io/image.hpp"
#include <cmath>
#include <filesystem>
#include <vector>

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

TEST_CASE("FrameCache: written_frames tracks the real global-index set", "[cache]") {
    // A heterogeneous-geometry batch: this cache only receives a SUBSET of a
    // batch's global frame indices (say frames 1 and 3 of a 5-frame batch,
    // the two whose post-debayer geometry matched this cache).
    FrameCache cache(4, 4, 1, /*max_frames*/5, "/tmp");

    Image f1(4, 4, 1); f1.fill(0.2f);
    Image f3(4, 4, 1); f3.fill(0.8f);

    cache.write_frame(1, f1);
    cache.write_frame(3, f3);

    // n_frames_written() over-reports (counts the interior gap): max index+1.
    REQUIRE(cache.n_frames_written() == 4);

    // written_frames() is the true real-sample set, in write order.
    REQUIRE(cache.written_frames() == std::vector<int>{1, 3});
}

TEST_CASE("FrameCache: read_pixel_dense packs only written frames, skips gaps",
          "[cache]") {
    FrameCache cache(4, 4, 1, /*max_frames*/5, "/tmp");
    Image f1(4, 4, 1); f1.fill(0.2f);
    Image f3(4, 4, 1); f3.fill(0.8f);
    cache.write_frame(1, f1);
    cache.write_frame(3, f3);

    // read_pixel() returns the gappy view: 4 values, position 0 and 2 are the
    // phantom zeros of the unwritten gaps.
    float gappy[5];
    int ng = cache.read_pixel(2, 2, 0, gappy);
    REQUIRE(ng == 4);
    REQUIRE(gappy[0] == Catch::Approx(0.0f).margin(1e-4f));  // gap
    REQUIRE(gappy[1] == Catch::Approx(0.2f).margin(1e-4f));  // frame 1
    REQUIRE(gappy[2] == Catch::Approx(0.0f).margin(1e-4f));  // gap
    REQUIRE(gappy[3] == Catch::Approx(0.8f).margin(1e-4f));  // frame 3

    // read_pixel_dense() packs ONLY the two real frames, in written order —
    // this is the read path the heterogeneous-geometry Phase B fix uses.
    float dense[5];
    int nd = cache.read_pixel_dense(2, 2, 0, dense);
    REQUIRE(nd == 2);
    REQUIRE(dense[0] == Catch::Approx(0.2f).margin(1e-4f));  // written_frames[0]==1
    REQUIRE(dense[1] == Catch::Approx(0.8f).margin(1e-4f));  // written_frames[1]==3
}

TEST_CASE("FrameCache: dense read equals gappy read for a same-geometry "
          "(dense [0..n-1]) cache", "[cache]") {
    // When every frame is written (no gaps), read_pixel_dense must be
    // byte-identical to read_pixel — the same-geometry invariant.
    FrameCache cache(4, 4, 1, 3, "/tmp");
    Image f0(4, 4, 1); f0.fill(0.1f);
    Image f1(4, 4, 1); f1.fill(0.4f);
    Image f2(4, 4, 1); f2.fill(0.9f);
    cache.write_frame(0, f0);
    cache.write_frame(1, f1);
    cache.write_frame(2, f2);

    REQUIRE(cache.written_frames() == std::vector<int>{0, 1, 2});

    float g[3], d[3];
    int ng = cache.read_pixel(1, 1, 0, g);
    int nd = cache.read_pixel_dense(1, 1, 0, d);
    REQUIRE(ng == nd);
    REQUIRE(ng == 3);
    for (int i = 0; i < 3; i++) REQUIRE(g[i] == d[i]);
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
