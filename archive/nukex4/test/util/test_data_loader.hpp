#pragma once

#include "nukex/io/image.hpp"
#include <string>

namespace nukex { namespace test_util {

/// Load the first M16 FITS frame, debayer it, and return as a 3-channel RGB Image.
/// Returns an empty Image if the file is not available.
///
/// The M16 data is HaO3 narrowband on a Bayer sensor (ZWO ASI2400MC Pro).
/// After debayer, it's a 3-channel float32 Image in [0, 1].
Image load_m16_test_frame();

/// Path to the M16 test data directory.
const std::string& m16_data_dir();

/// Prepare an image for stretch evaluation.
///
/// Raw single-frame data has background dominating 99%+ of the range.
/// This simulates what the stacking pipeline produces:
///   1. Subtract per-channel median (background removal)
///   2. Clip at a high percentile (reject hot pixels)
///   3. Normalize so signal fills [0, 1]
///
/// After this, stretches operate on representative data.
void prepare_for_stretch(Image& img, float clip_percentile = 0.999f);

}} // namespace nukex::test_util
