#include "catch_amalgamated.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/alignment/types.hpp"
#include "nukex/io/image.hpp"
#include "nukex/io/fits_reader.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>

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

// --- green as the detection channel -------------------------------------

TEST_CASE("default_reference_channel: green for colour, channel 0 for mono",
          "[star_detector]") {
    REQUIRE(nukex::default_reference_channel(1) == 0);
    REQUIRE(nukex::default_reference_channel(2) == 0);
    REQUIRE(nukex::default_reference_channel(3) == 1);
    REQUIRE(nukex::default_reference_channel(4) == 1);
}

TEST_CASE("StarDetector detects on green, not on channel 0",
          "[star_detector]") {
    // Red carries one star, green carries three. Detecting on channel 0 finds
    // one; detecting on green finds three. This is the whole point: on an OSC
    // frame channel 0 is red, and red is the channel we least want to trust.
    nukex::Image img(120, 120, 3);
    img.fill(0.01f);

    auto blob = [&](int ch, float cx, float cy) {
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                float r2 = float(dx * dx + dy * dy);
                img.at(int(cx) + dx, int(cy) + dy, ch) += 0.6f * std::exp(-r2 / 3.0f);
            }
    };

    blob(0, 30, 30);                 // red: one star
    blob(1, 30, 30);
    blob(1, 70, 40);                 // green: three stars
    blob(1, 50, 85);

    nukex::StarDetector::Config cfg;
    cfg.snr_multiplier = 3.0f;

    auto cat = nukex::StarDetector::detect(img, cfg);
    REQUIRE(cat.size() == 3);

    // And the channel is overridable, which is how the ladder tests in
    // Task 3 pin a specific plane.
    cfg.channel = 0;
    REQUIRE(nukex::StarDetector::detect(img, cfg).size() == 1);
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

// ── Crowded fields: the exclusion radius and cancel ─────────────────

namespace {

/// 2000x1500, the left 45% a flat "nebula" well above median + 5 sigma, so
/// every noise peak there is a candidate (~25k of them).
Image crowded_field() {
    Image img(2000, 1500, 1);
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 0.003f);
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++)
            img.at(x, y, 0) = (x < 900 ? 0.2f : 0.05f) + noise(rng);
    return img;
}

struct CancelledObserver : ProgressObserver {
    void begin_phase(const std::string&, int) override {}
    void advance(int, const std::string&) override {}
    void end_phase() override {}
    void message(const std::string&) override {}
    bool is_cancelled() const override { return true; }
};

} // namespace

TEST_CASE("StarDetector: the exclusion radius drops a dimmer star closer than it, across grid cells",
          "[star_detector][exclusion]") {
    // Pairs of sharp stars (sigma 1 px, so each peak is one pixel and both are
    // local maxima): a bright one, then a dimmer one at an exact offset. With
    // radius 5 the dimmer survives iff dx^2 + dy^2 >= 25. The bright stars sit
    // on x = 99 / y = 99, so every dimmer partner is in a neighbouring cell
    // of a radius-sized grid.
    struct Pair { int bx, by, dx, dy; bool dimmer_kept; };
    const std::vector<Pair> pairs = {
        {99, 99, 4, 0, false},  {199, 99, 5, 0, true},  {299, 99, 0, 4, false},
        {399, 99, 0, 5, true},  {99, 199, 4, 2, false}, {199, 199, 4, 3, true},
        {299, 199, -4, 0, false}, {399, 199, -5, 0, true}, {99, 299, -4, -2, false},
        {199, 299, -3, -4, true}, {299, 299, 4, -2, false}, {399, 299, 4, -4, true},
    };
    Image img(500, 400, 1);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.001f);
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++) img.at(x, y, 0) = 0.1f + noise(rng);
    auto draw = [&](int sx, int sy, float amp) {
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++)
                img.at(sx + dx, sy + dy, 0) += amp * std::exp(-(dx * dx + dy * dy) / 2.0f);
    };
    for (const auto& p : pairs) {
        draw(p.bx, p.by, 0.5f);
        draw(p.bx + p.dx, p.by + p.dy, 0.4f);
    }
    StarDetector::Config cfg;
    cfg.exclusion_radius = 5;
    cfg.max_stars = 10000;
    const auto cat = StarDetector::detect(img, cfg);
    auto found = [&](int x, int y) {
        for (const auto& s : cat.stars)
            if (std::abs(s.x - x) < 0.75f && std::abs(s.y - y) < 0.75f) return true;
        return false;
    };
    for (const auto& p : pairs) {
        INFO("bright (" << p.bx << "," << p.by << ") offset (" << p.dx << "," << p.dy << ")");
        REQUIRE(found(p.bx, p.by));
        REQUIRE(found(p.bx + p.dx, p.by + p.dy) == p.dimmer_kept);
    }
}

TEST_CASE("StarDetector: a set cancel stops detection and returns no stars",
          "[star_detector][crowded][cancel]") {
    const Image img = crowded_field();
    StarDetector::Config cfg;
    cfg.max_stars = 1000000;
    REQUIRE(StarDetector::detect(img, cfg).stars.size() > 1000);   // the field has work in it

    CancelledObserver obs;
    CancelPoll cancel(obs);
    REQUIRE(cancel.check());
    const auto t0 = std::chrono::steady_clock::now();
    const auto cat = StarDetector::detect(img, cfg, &cancel);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(cat.stars.empty());
    INFO(s << " s");
    REQUIRE(s < 0.5);
}
