#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/channel_config.hpp"

namespace nukex {

/// Debayer (demosaic) a raw Bayer image into RGB channels.
///
/// Input: single-channel float32 image with Bayer pattern data.
/// Output: 3-channel float32 image (R, G, B).
///
/// Uses bilinear interpolation. This is intentionally simple — for stacking,
/// the debayered output feeds into alignment and accumulation, not direct display.
/// The distribution fitting on the accumulated Z-arrays is what produces the
/// final pixel values, so debayer quality at this stage is not critical.
class DebayerEngine {
public:
    /// Debayer a raw Bayer image to RGB.
    /// Returns a 3-channel image. Input must be single-channel.
    static Image debayer(const Image& raw, BayerPattern pattern);

    /// Equalize Bayer sub-channel backgrounds before debayering.
    ///
    /// Astronomical data without dark/bias calibration has systematic offsets
    /// between the four Bayer sub-pixels (R, Gr, Gb, B). This creates a
    /// checkerboard pattern that becomes visible banding after aggressive
    /// stretching. This function equalizes each sub-image's median to the
    /// global median, removing the offset while preserving signal.
    ///
    /// Call this on the raw single-channel image BEFORE debayer().
    /// Modifies the image in-place.
    static void equalize_bayer_background(Image& raw, BayerPattern pattern);

    /// Suppress Bayer-pitch banding with a gentle 3x3 median filter.
    ///
    /// Bilinear debayer creates alternating measured/interpolated values at
    /// the 2-pixel Bayer pitch. This 3x3 median filter removes the structured
    /// pattern while preserving edges (stars, nebula boundaries) better than
    /// a box blur.
    ///
    /// Call this on the debayered RGB image AFTER debayer().
    static void suppress_banding(Image& rgb);

private:
    /// Bilinear interpolation for RGGB pattern.
    static void debayer_rggb(const Image& raw, Image& rgb);
    /// Bilinear interpolation for BGGR pattern.
    static void debayer_bggr(const Image& raw, Image& rgb);
    /// Bilinear interpolation for GRBG pattern.
    static void debayer_grbg(const Image& raw, Image& rgb);
    /// Bilinear interpolation for GBRG pattern.
    static void debayer_gbrg(const Image& raw, Image& rgb);
};

} // namespace nukex
