#pragma once

#include "nukex/io/image.hpp"
#include <string>

namespace nukex { namespace test_util {

/// Write an Image to a 16-bit PNG file for visual evaluation.
///
/// For float32 [0,1] images, maps to [0, 65535] uint16.
/// Multi-channel images are written as RGB (first 3 channels).
/// Single-channel images are written as grayscale.
///
/// Applies a simple arcsinh preview stretch if apply_stretch is true,
/// to make linear astronomical data visible.
///
/// Returns true on success.
bool write_png(const std::string& filepath, const nukex::Image& img,
               bool apply_stretch = true, float stretch_alpha = 500.0f);

/// Write an Image to an 8-bit PNG file.
/// Same as write_png but 8-bit output for maximum viewer compatibility.
bool write_png_8bit(const std::string& filepath, const nukex::Image& img,
                    bool apply_stretch = false, float stretch_alpha = 0.0f);

/// Write with astronomical auto-stretch (STF-like).
///
/// For raw astronomical data where the background dominates the pixel range,
/// this subtracts the per-channel median, clips at a high percentile (to reject
/// hot pixels), then applies an arcsinh stretch. Produces images where the
/// background is near-black and faint signal structure is visible.
///
/// This mimics what PixInsight's STF and Siril's auto-stretch do.
bool write_png_auto(const std::string& filepath, const nukex::Image& img,
                    float clip_percentile = 0.999f, float stretch_alpha = 500.0f);

}} // namespace nukex::test_util
