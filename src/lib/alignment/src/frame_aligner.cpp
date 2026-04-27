#include "nukex/alignment/frame_aligner.hpp"

namespace nukex {

FrameAligner::FrameAligner(const Config& config) : config_(config) {}

FrameAligner::AlignedFrame FrameAligner::align(const Image& frame, int frame_index) {
    AlignedFrame result;
    result.frame_index = frame_index;

    // Detect stars
    result.stars = StarDetector::detect(frame, config_.star_config);

    if (!has_ref_) {
        // First frame: becomes the reference
        ref_catalog_ = result.stars;
        ref_width_ = frame.width();
        ref_height_ = frame.height();
        has_ref_ = true;

        result.image = frame.clone();
        result.alignment.H = HomographyMatrix::identity();
        result.alignment.match.success = true;
        result.alignment.match.n_inliers = result.stars.size();
        return result;
    }

    // Match stars to reference using triangle similarity matching.
    auto matches = StarMatcher::match(result.stars, ref_catalog_,
                                       config_.match_config);

    // Compute homography
    result.alignment = HomographyComputer::compute(
        result.stars, ref_catalog_, matches, config_.homography_config);

    // Handle meridian flip
    if (result.alignment.is_meridian_flipped && !result.alignment.alignment_failed) {
        result.alignment.H = HomographyComputer::correct_meridian_flip(
            result.alignment.H, ref_width_, ref_height_);
    }

    // Warp image to reference frame
    if (!result.alignment.alignment_failed) {
        result.image = HomographyComputer::warp(
            frame, result.alignment.H, ref_width_, ref_height_);
    } else {
        // Failed alignment: return unwarped frame, weight penalized
        result.image = frame.clone();
    }

    return result;
}

void FrameAligner::reset() {
    ref_catalog_ = StarCatalog{};
    has_ref_ = false;
    ref_width_ = 0;
    ref_height_ = 0;
}

} // namespace nukex
