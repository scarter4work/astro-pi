#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/alignment/star_matcher.hpp"
#include "nukex/alignment/homography.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// High-level frame alignment: detect → match → homography → warp.
///
/// The first frame becomes the reference (H = identity). Subsequent frames
/// are aligned to the reference catalog. Meridian flips are detected and
/// corrected automatically. Failed alignments keep weight × 0.5.
class FrameAligner {
public:
    struct Config {
        StarDetector::Config star_config;
        StarMatcher::Config  match_config;
        HomographyComputer::Config homography_config;
    };

    FrameAligner() = default;
    explicit FrameAligner(const Config& config);

    /// Align a frame to the reference. Returns the aligned image and alignment result.
    /// If this is the first frame, it becomes the reference (H = identity).
    ///
    /// The input image should be single-channel (luminance or raw Bayer).
    /// For star detection, channel 0 is used.
    struct AlignedFrame {
        Image image;              // warped image, aligned to reference
        AlignmentResult alignment;
        StarCatalog stars;        // detected stars (pre-alignment coordinates)
        int frame_index;
    };

    AlignedFrame align(const Image& frame, int frame_index);

    /// Get the reference catalog (for inspection/debugging).
    const StarCatalog& reference_catalog() const { return ref_catalog_; }

    /// Has a reference frame been set?
    bool has_reference() const { return has_ref_; }

    /// Reset the aligner (clear reference).
    void reset();

private:
    Config config_;
    StarCatalog ref_catalog_;
    bool has_ref_ = false;
    int ref_width_ = 0;
    int ref_height_ = 0;
};

} // namespace nukex
