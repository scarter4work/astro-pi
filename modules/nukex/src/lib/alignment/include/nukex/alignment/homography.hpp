#pragma once

#include "nukex/alignment/channel_registration.hpp"
#include "nukex/alignment/types.hpp"
#include "nukex/core/coverage_mask.hpp"
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

    /// Warp, additionally registering each channel to the reference channel.
    ///
    /// H_inv maps an output pixel to where the REFERENCE channel sees it in
    /// the source. A_c then maps that to where channel c sees it. So the
    /// sample position for channel c is A_c(H_inv(x, y)) -- one resample per
    /// channel, exactly as the plain warp does, with the correction folded in
    /// rather than applied as a second pass.
    ///
    /// An empty `channels`, or an identity entry within it, takes the same
    /// path as the four-argument overload for that channel.
    static Image warp(const Image& source, const HomographyMatrix& H,
                      int output_width, int output_height,
                      const ChannelTransforms& channels);

    /// Warp, recording which output pixels actually received source data.
    ///
    /// Pixels outside the source's coverage are left at 0, and 0 is a legal
    /// pixel value, so the accumulator cannot tell absent data from measured
    /// darkness without this mask. `coverage` is resized to
    /// (output_width, output_height, source.n_channels()) and every pixel
    /// this call samples is marked covered.
    static Image warp(const Image& source, const HomographyMatrix& H,
                      int output_width, int output_height,
                      const ChannelTransforms& channels,
                      CoverageMask& coverage);

private:
    /// Shared body of the warp overloads. `coverage` may be null when the
    /// caller does not need to know which pixels were sampled.
    static Image warp_impl(const Image& source, const HomographyMatrix& H,
                           int output_width, int output_height,
                           const ChannelTransforms& channels,
                           CoverageMask* coverage);

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
