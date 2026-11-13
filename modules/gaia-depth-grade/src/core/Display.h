// Preview pixels, as the Python core's display.py (PCL-free, reentrant). Display only: the
// Execute path stays linear.
#pragma once

#include "Image.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace gdg {

/// display.screen_blend: 1 - (1 - a)(1 - b), the harness PixelMath ~((~A)*(~B)). Throws
/// std::invalid_argument when the shapes differ.
Image screen_blend(const Image& a, const Image& b);

/// Screen-transfer-function parameters, one entry per channel: c0 = the shadows clip, m = the
/// midtones balance of PixInsight's MTF.
struct StfParams {
    std::vector<double> c0, m;
};

/// display.autostretch's parameters. The channels are linked, as display.autostretch: one median
/// and MAD over every sample of max(rgb, 0), c0 = clip(median + shadows_clip * 1.4826 * MAD, 0, 1),
/// m solved so the median maps to target_bg (0.5 when that is singular) and clipped to
/// [1e-3, 1 - 1e-3]; so every channel's entry is equal. Throws std::invalid_argument on empty or
/// non-finite input (numpy would return NaN-derived garbage).
StfParams stf_params(const Image& rgb, double target_bg = 0.25, double shadows_clip = -2.8);

/// The STF to 8 bits, same layout as `rgb`: per sample of channel c, y = clip((max(v, 0) - c0) /
/// max(1e-8, 1 - c0), 0, 1), PixInsight's MTF with m, then uint8(out * 255 + 0.5). Elementwise and
/// parallel by rows (output identical to a serial pass). Throws std::invalid_argument when the
/// parameters do not hold one entry per channel or are non-finite or out of range (c0 in [0, 1],
/// m in (0, 1)), or a pixel is non-finite.
std::vector<uint8_t> apply_stf_u8(const Image& rgb, const StfParams& params);

/// display.autostretch: STF-style stretch to 8 bits, same layout as `rgb`; exactly
/// apply_stf_u8(rgb, stf_params(rgb, target_bg, shadows_clip)).
std::vector<uint8_t> autostretch_u8(const Image& rgb, double target_bg = 0.25, double shadows_clip = -2.8);

/// numpy a[y:y+h, x:x+w] (bounds clamped as slicing clamps them; may be empty).
Image crop(const Image& img, int x, int y, int w, int h);

/// The size write_png's max_width downscale produces: (max_width, max(1, round(h * scale)))
/// when w > max_width, else (w, h). Python round(): half to even.
std::pair<int, int> downscale_size(int w, int h, int max_width);

/// Box-average downscale of an 8-bit (h, w, channels) buffer to downscale_size(w, h, max_width).
/// Display only: not pixel-identical to PIL's resampling (no parity required). Throws
/// std::invalid_argument when the image has no pixels or the buffer size does not match
/// w * h * channels.
std::vector<uint8_t> downscale_u8(const std::vector<uint8_t>& rgb, int w, int h, int channels, int max_width);

/// The size that fits inside max_width x max_height with the aspect kept and no upscale: the
/// limiting side exact, the other rounded as downscale_size rounds (half to even), at least 1.
/// max_height < 1: no height bound (= downscale_size). Throws std::invalid_argument when w or h
/// is below 1 (nothing to fit).
std::pair<int, int> fit_size(int w, int h, int max_width, int max_height);

/// downscale_u8's box average to an explicit (ow, oh), 1 <= ow <= w and 1 <= oh <= h. Throws
/// std::invalid_argument on an image with no pixels, a size mismatch, an empty target or an upscale.
std::vector<uint8_t> downscale_u8_to(const std::vector<uint8_t>& rgb, int w, int h, int channels, int ow, int oh);

}  // namespace gdg
