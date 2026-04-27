#include "catch_amalgamated.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/io/fits_reader.hpp"
#include <filesystem>
#include <cmath>

using namespace nukex;

/// Create a synthetic star field with Gaussian stars.
static Image create_star_field(int w, int h,
                                const std::vector<std::tuple<float,float,float>>& stars,
                                float bg = 0.05f, float sigma = 3.0f) {
    Image img(w, h, 1);
    img.fill(bg);
    for (const auto& [sx, sy, amp] : stars) {
        int r = static_cast<int>(sigma * 4);
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int px = static_cast<int>(sx + 0.5f) + dx;
                int py = static_cast<int>(sy + 0.5f) + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                float fx = static_cast<float>(px) - sx;
                float fy = static_cast<float>(py) - sy;
                img.at(px, py, 0) += amp * std::exp(-(fx*fx + fy*fy) / (2*sigma*sigma));
            }
        }
    }
    return img;
}

TEST_CASE("FrameAligner: first frame becomes reference", "[aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f},
        {120, 120, 0.55f}, {30, 100, 0.5f}, {170, 100, 0.45f}
    };

    Image frame = create_star_field(200, 200, stars);
    FrameAligner aligner;
    auto result = aligner.align(frame, 0);

    REQUIRE(aligner.has_reference() == true);
    REQUIRE(result.alignment.H.is_identity() == true);
    REQUIRE(result.alignment.alignment_failed == false);
    REQUIRE(result.image.width() == 200);
}

TEST_CASE("FrameAligner: identical frames align to identity", "[aligner]") {
    std::vector<std::tuple<float,float,float>> stars = {
        {50, 50, 0.8f}, {150, 50, 0.7f}, {100, 100, 0.9f},
        {50, 150, 0.6f}, {150, 150, 0.75f}, {80, 80, 0.65f},
        {120, 120, 0.55f}, {30, 100, 0.5f}, {170, 100, 0.45f}
    };

    Image frame = create_star_field(200, 200, stars);

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 3.0f;
    config.match_config.max_distance = 10.0f;
    FrameAligner aligner(config);

    auto ref_result = aligner.align(frame, 0);
    auto result = aligner.align(frame, 1);

    REQUIRE(result.alignment.match.success == true);
    REQUIRE(result.alignment.H.is_identity(1.0f) == true);
    REQUIRE(result.alignment.match.rms_error < 1.0f);
}

TEST_CASE("FrameAligner: real FITS frames", "[aligner][integration]") {
    std::string path1 = "/home/scarter4work/projects/processing/M16/"
                        "Light_M16_300.0s_Bin1_HaO3_20230901-231500_0001.fit";
    std::string path2 = "/home/scarter4work/projects/processing/M16/"
                        "Light_M16_300.0s_Bin1_HaO3_20230901-232001_0002.fit";

    if (!std::filesystem::exists(path1) || !std::filesystem::exists(path2)) {
        SKIP("Test FITS files not available");
    }

    auto r1 = FITSReader::read(path1);
    auto r2 = FITSReader::read(path2);
    REQUIRE(r1.success);
    REQUIRE(r2.success);

    FrameAligner::Config config;
    config.star_config.snr_multiplier = 5.0f;
    config.star_config.max_stars = 200;
    config.match_config.max_distance = 20.0f;

    FrameAligner aligner(config);

    auto ref = aligner.align(r1.image, 0);
    REQUIRE(ref.alignment.H.is_identity());
    INFO("Reference stars: " << ref.stars.size());
    REQUIRE(ref.stars.size() >= 10);

    auto aligned = aligner.align(r2.image, 1);
    INFO("Inliers: " << aligned.alignment.match.n_inliers);
    INFO("RMS error: " << aligned.alignment.match.rms_error);
    REQUIRE(aligned.alignment.match.success == true);
    REQUIRE(aligned.alignment.alignment_failed == false);
    REQUIRE(aligned.alignment.match.n_inliers >= 8);
    REQUIRE(aligned.alignment.match.rms_error < 5.0f);
}

TEST_CASE("FrameAligner: reset clears reference", "[aligner]") {
    FrameAligner aligner;
    Image frame(100, 100, 1);
    frame.fill(0.5f);

    aligner.align(frame, 0);
    REQUIRE(aligner.has_reference() == true);

    aligner.reset();
    REQUIRE(aligner.has_reference() == false);
}
