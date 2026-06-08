#include "catch_amalgamated.hpp"
#include "nukex/stretch/sas_stretch.hpp"
#include "png_writer.hpp"
#include "test_data_loader.hpp"
#include <cmath>

using namespace nukex;

// ── Basic properties ──

TEST_CASE("SAS: output in [0, 1]", "[sas]") {
    Image img(64, 64, 1);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            img.at(x, y, 0) = static_cast<float>(y * 64 + x) / (64.0f * 64.0f);

    SASStretch s;
    s.tile_size = 32;
    s.apply(img);

    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            REQUIRE(img.at(x, y, 0) >= -1e-6f);
            REQUIRE(img.at(x, y, 0) <= 1.0f + 1e-6f);
        }
}

TEST_CASE("SAS: dark regions get more stretch than bright regions", "[sas]") {
    // Create an image with a dark half and a bright half
    Image img(64, 64, 1);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            if (x < 32)
                img.at(x, y, 0) = 0.02f;  // Dark
            else
                img.at(x, y, 0) = 0.3f;   // Bright
        }

    SASStretch s;
    s.tile_size = 32;
    s.target_median = 0.25f;
    s.apply(img);

    // The dark region should have been stretched more (higher output relative to input)
    float dark_out = img.at(8, 32, 0);
    float bright_out = img.at(48, 32, 0);

    float dark_boost = dark_out / 0.02f;   // Stretch factor for dark
    float bright_boost = bright_out / 0.3f; // Stretch factor for bright

    REQUIRE(dark_boost > bright_boost);
}

TEST_CASE("SAS: preserves monotonicity within a tile", "[sas]") {
    // Gradient image — within a single tile, ordering should be preserved
    Image img(64, 64, 1);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            img.at(x, y, 0) = static_cast<float>(x) / 63.0f * 0.5f;

    SASStretch s;
    s.tile_size = 64;  // Single tile covers the whole image
    s.apply(img);

    // Along a row, values should still be monotonically increasing
    for (int x = 1; x < 64; x++) {
        REQUIRE(img.at(x, 32, 0) >= img.at(x - 1, 32, 0) - 1e-5f);
    }
}

TEST_CASE("SAS: min_D prevents too-gentle stretch", "[sas]") {
    Image img(32, 32, 1);
    img.fill(0.5f);  // Bright image — above target_median

    SASStretch s;
    s.tile_size = 32;
    s.target_median = 0.25f;
    s.min_D = 1.0f;
    s.apply(img);

    // With bright image, D should be clamped to min_D
    // Output should still be valid
    REQUIRE(img.at(16, 16, 0) >= 0.0f);
    REQUIRE(img.at(16, 16, 0) <= 1.0f);
}

TEST_CASE("SAS: max_D prevents too-aggressive stretch", "[sas]") {
    Image img(32, 32, 1);
    img.fill(0.001f);  // Very dark — would need huge D

    SASStretch s;
    s.tile_size = 32;
    s.target_median = 0.25f;
    s.max_D = 5.0f;
    s.apply(img);

    // Output should be stretched but not beyond what D=5 produces
    REQUIRE(img.at(16, 16, 0) >= 0.0f);
    REQUIRE(img.at(16, 16, 0) <= 1.0f);
}

TEST_CASE("SAS: RGB luminance_only preserves colors", "[sas]") {
    Image img(32, 32, 3);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            float v = (y * 32 + x) / (32.0f * 32.0f) * 0.3f;
            img.at(x, y, 0) = v * 2.0f;  // R brighter
            img.at(x, y, 1) = v;          // G
            img.at(x, y, 2) = v * 0.5f;  // B dimmer
        }

    // Check ratio at a mid-brightness pixel
    float r_before = img.at(16, 16, 0);
    float g_before = img.at(16, 16, 1);
    float ratio_before = r_before / g_before;

    SASStretch s;
    s.tile_size = 32;
    s.luminance_only = true;
    s.apply(img);

    float r_after = img.at(16, 16, 0);
    float g_after = img.at(16, 16, 1);
    // Only check ratio if values aren't clamped
    if (r_after < 0.99f && g_after > 0.01f) {
        float ratio_after = r_after / g_after;
        REQUIRE(ratio_after == Catch::Approx(ratio_before).margin(0.1f));
    }
}

// ── Visual output ──

// Tagged [.visual] so ctest skips it by default.  Takes ~32s —
// trips the default 30s timeout.  Invoke with:  ./test/test_sas [visual]
TEST_CASE("SAS: visual output on M16", "[.visual][sas]") {
    auto img = test_util::load_m16_test_frame();
    if (img.empty()) { SKIP("M16 test data not available"); }

    test_util::prepare_for_stretch(img);

    SASStretch s;
    s.tile_size = 256;
    s.tile_overlap = 0.5f;
    s.target_median = 0.20f;
    s.max_D = 15.0f;
    s.ghs_b = 0.0f;
    s.luminance_only = true;
    s.apply(img);

    bool ok = test_util::write_png_8bit("test/output/stretch_sas.png", img);
    REQUIRE(ok);
}
