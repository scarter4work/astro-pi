#include "catch_amalgamated.hpp"
#include "nukex/alignment/reference_selector.hpp"

using namespace nukex;

static FrameQuality q(int stars, float fwhm, bool usable = true) {
    FrameQuality f;
    f.star_count  = stars;
    f.median_fwhm = fwhm;
    f.usable      = usable;
    return f;
}

// The rule is minimal intervention: keep the frame the aligner would have
// adopted anyway -- the first usable one -- unless it is not a viable
// reference, and only then move to the best candidate.
//
// Measured on 2026-09-04, and the reason the rule is not simply "pick the
// sharpest". On NGC7635 all 65 frames detect exactly 200 stars and the first
// frame's FWHM ranks 18th of 65, so it is a perfectly good reference. Swapping
// it for the sharpest frame -- better by 0.2 px -- took that corpus from 65 of
// 65 frames aligned to 59 of 65. The reference is arbitrary among viable
// candidates, and moving it gratuitously costs alignments.

TEST_CASE("select_reference_frame: a viable first frame is kept even when a "
          "sharper one exists", "[reference_selector]") {
    std::vector<FrameQuality> frames = {
        q(200, 4.1f),   // the incumbent: unremarkable, but viable
        q(200, 2.6f),   // sharper, and irrelevant
        q(200, 3.8f),
    };
    REQUIRE(select_reference_frame(frames) == 0);
}

TEST_CASE("select_reference_frame: a star-poor first frame does not become the "
          "reference", "[reference_selector]") {
    // The M27 2025 LRGB-mono regression. Directory order put a B-filter frame
    // first; it yielded 32 stars against 200 for every other frame, so the
    // Groth matcher found no consistent triangle and 71 of 72 frames reported
    // zero inliers.
    std::vector<FrameQuality> frames;
    frames.push_back(q(32, 3.4f));
    for (int i = 0; i < 71; i++) {
        frames.push_back(q(200, 3.2f));
    }
    REQUIRE(select_reference_frame(frames) != 0);
    REQUIRE(frames[select_reference_frame(frames)].star_count == 200);
}

TEST_CASE("select_reference_frame: replacing a poor incumbent takes the "
          "sharpest candidate", "[reference_selector]") {
    // Detection caps at max_stars, so candidates usually tie on count and FWHM
    // is what separates them.
    std::vector<FrameQuality> frames = {
        q(20,  1.0f),   // incumbent, far below the band -- must be replaced
        q(200, 4.1f),
        q(200, 2.6f),
        q(200, 3.8f),
    };
    REQUIRE(select_reference_frame(frames) == 2);
}

TEST_CASE("select_reference_frame: a slightly poorer but much sharper frame can "
          "win the replacement", "[reference_selector]") {
    // A frame within the candidate band on count is a valid reference, and a
    // two-pixel-tighter PSF matters more than five more stars.
    std::vector<FrameQuality> frames = {
        q(20,  1.0f),   // incumbent, not viable
        q(200, 5.0f),
        q(195, 2.5f),
    };
    REQUIRE(select_reference_frame(frames) == 2);
}

TEST_CASE("select_reference_frame: a frame far below the best count is not a "
          "candidate", "[reference_selector]") {
    // Sharpness must not rescue a frame that cannot supply enough triangles.
    std::vector<FrameQuality> frames = {
        q(20,  4.0f),   // incumbent, not viable
        q(200, 4.0f),
        q(30,  1.1f),   // sharpest by far, and useless
    };
    REQUIRE(select_reference_frame(frames) == 1);
}

TEST_CASE("select_reference_frame: an unusable first frame is not the incumbent",
          "[reference_selector]") {
    // The aligner would never have adopted a frame it skipped, so the incumbent
    // is the first USABLE frame.
    std::vector<FrameQuality> frames = {
        q(400, 1.0f, /*usable*/false),   // blown out -- best numbers, still skipped
        q(180, 3.0f),                    // this is what the aligner would take
        q(200, 2.0f),                    // sharper and richer, and still not chosen
    };
    REQUIRE(select_reference_frame(frames) == 1);
}

TEST_CASE("select_reference_frame: an unusable incumbent below the band is "
          "replaced", "[reference_selector]") {
    std::vector<FrameQuality> frames = {
        q(10,  3.0f, /*usable*/false),
        q(20,  3.0f),                    // usable, but far below the best
        q(200, 3.5f),
        q(200, 3.1f),
    };
    REQUIRE(select_reference_frame(frames) == 3);
}

TEST_CASE("select_reference_frame: falls back to the first frame when none is "
          "usable", "[reference_selector]") {
    // Degrades to the historical behaviour rather than failing the stack.
    std::vector<FrameQuality> frames = {
        q(10, 3.0f, false),
        q(12, 3.0f, false),
    };
    REQUIRE(select_reference_frame(frames) == 0);
}

TEST_CASE("select_reference_frame: an unmeasurable FWHM does not beat a "
          "measured one", "[reference_selector]") {
    // compute_median_fwhm returns 0 when it cannot measure; treating that as
    // "infinitely sharp" would hand the reference to the worst frame.
    std::vector<FrameQuality> frames = {
        q(20,  3.0f),   // incumbent, not viable
        q(200, 0.0f),
        q(200, 3.0f),
    };
    REQUIRE(select_reference_frame(frames) == 2);
}

TEST_CASE("select_reference_frame: an empty list selects nothing",
          "[reference_selector]") {
    std::vector<FrameQuality> frames;
    REQUIRE(select_reference_frame(frames) == -1);
}
