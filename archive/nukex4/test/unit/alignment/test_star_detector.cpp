#include "catch_amalgamated.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/alignment/types.hpp"
#include "nukex/io/image.hpp"
#include "nukex/io/fits_reader.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>

using namespace nukex;

/// Helper: create a synthetic image with Gaussian stars at known positions.
static Image create_star_field(int width, int height,
                                const std::vector<std::tuple<float, float, float>>& stars_xya,
                                float background = 0.1f, float sigma = 3.0f) {
    Image img(width, height, 1);
    img.fill(background);

    for (const auto& [sx, sy, amp] : stars_xya) {
        // Draw a Gaussian star
        int r = static_cast<int>(sigma * 4);
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int px = static_cast<int>(sx + 0.5f) + dx;
                int py = static_cast<int>(sy + 0.5f) + dy;
                if (px < 0 || px >= width || py < 0 || py >= height) continue;

                float fx = static_cast<float>(px) - sx;
                float fy = static_cast<float>(py) - sy;
                float g = amp * std::exp(-(fx*fx + fy*fy) / (2.0f * sigma * sigma));
                img.at(px, py, 0) += g;
            }
        }
    }

    return img;
}

TEST_CASE("StarDetector: detect synthetic stars", "[star_detector]") {
    // Create a 200×200 image with 5 stars at known positions
    std::vector<std::tuple<float, float, float>> star_positions = {
        {50.0f,  50.0f,  0.8f},
        {150.0f, 50.0f,  0.6f},
        {100.0f, 100.0f, 0.9f},
        {50.0f,  150.0f, 0.5f},
        {150.0f, 150.0f, 0.7f}
    };

    Image img = create_star_field(200, 200, star_positions, 0.05f, 3.0f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.max_stars = 10;

    StarCatalog catalog = StarDetector::detect(img, config);

    // Should detect all 5 stars
    REQUIRE(catalog.size() >= 4);  // allow 1 miss due to thresholding
    REQUIRE(catalog.size() <= 10);

    // Brightest star should be near (100, 100) with amplitude 0.9
    REQUIRE(catalog.stars[0].x == Catch::Approx(100.0f).margin(2.0f));
    REQUIRE(catalog.stars[0].y == Catch::Approx(100.0f).margin(2.0f));

    // All detected stars should have positive flux and SNR
    for (const auto& star : catalog.stars) {
        REQUIRE(star.flux > 0.0f);
        REQUIRE(star.snr > 0.0f);
    }
}

TEST_CASE("StarDetector: centroid accuracy on known position", "[star_detector]") {
    // Single bright star at sub-pixel position (50.3, 75.7)
    std::vector<std::tuple<float, float, float>> stars = {
        {50.3f, 75.7f, 0.8f}
    };
    Image img = create_star_field(100, 100, stars, 0.02f, 2.5f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.max_stars = 5;

    StarCatalog catalog = StarDetector::detect(img, config);

    REQUIRE(catalog.size() >= 1);

    // Centroid should be within 0.5 pixel of true position
    REQUIRE(catalog.stars[0].x == Catch::Approx(50.3f).margin(0.5f));
    REQUIRE(catalog.stars[0].y == Catch::Approx(75.7f).margin(0.5f));
}

TEST_CASE("StarDetector: rejects saturated stars", "[star_detector]") {
    std::vector<std::tuple<float, float, float>> stars = {
        {50.0f, 50.0f, 1.0f},  // saturated (peak will be > 0.95)
        {50.0f, 80.0f, 0.3f}   // not saturated
    };
    Image img = create_star_field(100, 100, stars, 0.02f, 2.5f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.saturation_level = 0.95f;

    StarCatalog catalog = StarDetector::detect(img, config);

    // The saturated star should be rejected
    // Only the dim star should remain
    for (const auto& star : catalog.stars) {
        REQUIRE(star.peak <= config.saturation_level);
    }
}

TEST_CASE("StarDetector: empty image returns empty catalog", "[star_detector]") {
    Image img(100, 100, 1);
    img.fill(0.1f);  // uniform — no stars

    StarCatalog catalog = StarDetector::detect(img);
    REQUIRE(catalog.size() == 0);
}

TEST_CASE("StarDetector: max_stars limits output", "[star_detector]") {
    // Create 20 stars
    std::vector<std::tuple<float, float, float>> stars;
    for (int i = 0; i < 20; i++) {
        float x = 20.0f + static_cast<float>(i % 5) * 30.0f;
        float y = 20.0f + static_cast<float>(i / 5) * 30.0f;
        float amp = 0.3f + static_cast<float>(i) * 0.03f;
        stars.emplace_back(x, y, amp);
    }
    Image img = create_star_field(200, 200, stars, 0.02f, 2.0f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.max_stars = 10;

    StarCatalog catalog = StarDetector::detect(img, config);
    REQUIRE(catalog.size() <= 10);
}

TEST_CASE("StarDetector: rejects fully saturated frame quickly", "[star_detector]") {
    // Simulates a dawn/twilight frame clipped to max everywhere.  Without the
    // saturation guard, find_local_maxima's O(n^2) exclusion filter runs over
    // every pixel and hangs for minutes.  With the guard the call must return
    // an empty catalog in well under a second even at this size.
    Image img(500, 500, 1);
    img.fill(1.0f);

    auto t0 = std::chrono::steady_clock::now();
    StarCatalog catalog = StarDetector::detect(img);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    REQUIRE(catalog.size() == 0);
    REQUIRE(elapsed < 500);  // guard makes this O(samples), not O(pixels^2)
}

TEST_CASE("StarDetector: rejects majority-saturated frame (dawn twilight case)",
          "[star_detector]") {
    // Real M27 failure mode: ~60% of the frame clipped to saturation with a
    // gradient across the rest.  max - min is ~0.1 (gradient), so a simple
    // dynamic-range check would not catch this; saturation-fraction must.
    const int W = 1200, H = 1200;
    Image img(W, H, 1);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            // Top 60% of rows saturated; bottom 40% ramps 0.85 -> 0.95.
            float v;
            if (y < (H * 6) / 10) {
                v = 1.0f;
            } else {
                v = 0.85f + 0.10f *
                    static_cast<float>(y - (H * 6) / 10) /
                    static_cast<float>(H - (H * 6) / 10);
            }
            img.at(x, y, 0) = v;
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    StarCatalog catalog = StarDetector::detect(img);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    REQUIRE(catalog.size() == 0);
    REQUIRE(elapsed < 500);
}

TEST_CASE("StarDetector: normal frame with a few saturated stars still detects",
          "[star_detector]") {
    // A well-exposed frame has only a handful of saturated pixels (bright
    // stars' cores).  The guard must not fire on these.
    std::vector<std::tuple<float, float, float>> stars = {
        {60.0f,  60.0f,  0.5f},
        {140.0f, 60.0f,  1.2f},   // saturates peak
        {100.0f, 140.0f, 0.6f},
        {180.0f, 180.0f, 0.4f}
    };
    Image img = create_star_field(250, 250, stars, 0.05f, 3.0f);

    StarDetector::Config config;
    config.snr_multiplier = 3.0f;
    config.max_stars = 10;

    StarCatalog catalog = StarDetector::detect(img, config);
    REQUIRE(catalog.size() >= 3);  // at least the unsaturated ones
}

TEST_CASE("StarDetector: real FITS file", "[star_detector][integration]") {
    std::string path = "/home/scarter4work/projects/processing/M16/"
                       "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";
    if (!std::filesystem::exists(path)) {
        SKIP("Test FITS file not available");
    }

    auto result = FITSReader::read(path);
    REQUIRE(result.success);

    StarDetector::Config config;
    config.snr_multiplier = 5.0f;
    config.max_stars = 200;

    StarCatalog catalog = StarDetector::detect(result.image, config);

    // M16 field should have plenty of stars
    INFO("Detected " << catalog.size() << " stars");
    REQUIRE(catalog.size() >= 20);

    // Stars should be sorted by flux (brightest first)
    for (int i = 1; i < catalog.size(); i++) {
        REQUIRE(catalog.stars[i].flux <= catalog.stars[i-1].flux);
    }

    // All stars should have valid coordinates within image bounds
    for (const auto& star : catalog.stars) {
        REQUIRE(star.x >= 0.0f);
        REQUIRE(star.x < static_cast<float>(result.image.width()));
        REQUIRE(star.y >= 0.0f);
        REQUIRE(star.y < static_cast<float>(result.image.height()));
    }
}

TEST_CASE("HomographyMatrix: identity", "[homography]") {
    auto H = HomographyMatrix::identity();
    REQUIRE(H.is_identity());

    auto [x, y] = H.transform(100.0f, 200.0f);
    REQUIRE(x == Catch::Approx(100.0f));
    REQUIRE(y == Catch::Approx(200.0f));
}

TEST_CASE("HomographyMatrix: rotation detection", "[homography]") {
    HomographyMatrix H;
    // 180-degree rotation about origin
    H(0, 0) = -1.0f; H(0, 1) =  0.0f; H(0, 2) = 0.0f;
    H(1, 0) =  0.0f; H(1, 1) = -1.0f; H(1, 2) = 0.0f;
    H(2, 0) =  0.0f; H(2, 1) =  0.0f; H(2, 2) = 1.0f;

    REQUIRE(std::abs(H.rotation_degrees()) == Catch::Approx(180.0f).margin(1.0f));
    REQUIRE(H.is_meridian_flip() == true);
}

TEST_CASE("HomographyMatrix: small rotation is not meridian flip", "[homography]") {
    HomographyMatrix H;
    float angle = 2.0f * 3.14159f / 180.0f; // 2 degrees
    H(0, 0) = std::cos(angle);  H(0, 1) = -std::sin(angle); H(0, 2) = 0.0f;
    H(1, 0) = std::sin(angle);  H(1, 1) =  std::cos(angle); H(1, 2) = 0.0f;
    H(2, 0) = 0.0f;             H(2, 1) = 0.0f;              H(2, 2) = 1.0f;

    REQUIRE(H.is_meridian_flip() == false);
}
