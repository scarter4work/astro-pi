#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/io/image.hpp"

namespace nukex {

/// Compute projective homography using RANSAC + Direct Linear Transform (DLT).
class HomographyComputer {
public:
    struct Config {
        int   ransac_iterations = 500;
        float inlier_threshold  = 1.5f;   // pixels
        int   min_matches       = 8;
    };

    /// Compute homography from source to reference using matched star pairs.
    /// Uses RANSAC to reject outliers, then DLT on inlier set.
    static AlignmentResult compute(
        const StarCatalog& source,
        const StarCatalog& reference,
        const std::vector<std::pair<int, int>>& matches,
        const Config& config);

    /// Compute homography using default config.
    static AlignmentResult compute(
        const StarCatalog& source,
        const StarCatalog& reference,
        const std::vector<std::pair<int, int>>& matches)
    {
        return compute(source, reference, matches, Config{});
    }

    /// Apply homography to an image using bilinear interpolation.
    /// Pixels outside the transformed boundary get value 0.
    static Image warp(const Image& source, const HomographyMatrix& H,
                      int output_width, int output_height);

    /// Correct a meridian-flipped homography by pre-multiplying with
    /// a 180-degree rotation about the image center.
    static HomographyMatrix correct_meridian_flip(
        const HomographyMatrix& H, int width, int height);

private:
    /// Solve for homography from exactly 4 point correspondences using DLT.
    static HomographyMatrix dlt_4point(
        const float src_x[4], const float src_y[4],
        const float dst_x[4], const float dst_y[4]);

    /// Solve for homography from N point correspondences using DLT (SVD).
    static HomographyMatrix dlt_npoint(
        const std::vector<float>& src_x, const std::vector<float>& src_y,
        const std::vector<float>& dst_x, const std::vector<float>& dst_y);

    /// Count inliers for a given homography.
    static int count_inliers(
        const HomographyMatrix& H,
        const StarCatalog& source,
        const StarCatalog& reference,
        const std::vector<std::pair<int, int>>& matches,
        float threshold,
        std::vector<bool>& inlier_mask);
};

} // namespace nukex
