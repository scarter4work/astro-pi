#pragma once

#include "nukex/io/image.hpp"
#include <vector>
#include <string>

namespace nukex {

/// Flat field calibration.
///
/// Generates a master flat from a set of flat frames (median combination),
/// normalizes it, and divides light frames by it to correct for vignetting,
/// dust, and optical path non-uniformity.
class FlatCalibration {
public:
    /// Build a master flat from a list of flat frame file paths.
    /// Reads each flat, normalizes by its median, then median-combines.
    /// Result is a normalized master flat where median ≈ 1.0.
    static Image build_master_flat(const std::vector<std::string>& flat_paths);

    /// Apply flat correction to a light frame: light /= master_flat.
    /// Both images must have the same dimensions.
    /// Pixels in the master flat below min_flat_value are clamped to avoid
    /// division by near-zero (which amplifies noise in vignetted corners).
    static void apply(Image& light, const Image& master_flat,
                      float min_flat_value = 0.01f);

    /// Compute the median of all pixel values in a single-channel image
    /// or across all channels.
    static float median(const Image& img);
};

} // namespace nukex
