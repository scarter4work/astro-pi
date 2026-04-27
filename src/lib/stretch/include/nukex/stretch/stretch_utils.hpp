#pragma once

#include "nukex/io/image.hpp"
#include <functional>

namespace nukex {

/// Apply a scalar stretch function to an image's luminance only,
/// preserving chromaticity (hue + saturation).
///
/// For single-channel images: applies directly.
/// For multi-channel (RGB): compute luminance L = 0.2126R + 0.7152G + 0.0722B,
/// stretch L, scale R/G/B by L'/L ratio.
///
/// Reference: Rec. 709 luminance coefficients.
void apply_luminance_only(Image& img, const std::function<float(float)>& fn);

/// Apply a scalar function per-channel independently.
void apply_per_channel(Image& img, const std::function<float(float)>& fn);

/// Clamp all pixel values in an image to [lo, hi].
void clamp_image(Image& img, float lo = 0.0f, float hi = 1.0f);

} // namespace nukex
