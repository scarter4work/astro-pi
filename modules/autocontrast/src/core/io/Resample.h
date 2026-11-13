// AutoContrast core (PCL-free): Pillow-exact image resizing.
//
// A port of Pillow 12.3.0's Image.resize(size) with its default filter (BICUBIC, a = -0.5) -- src/PIL/Image.py
// resize() and src/libImaging/Resample.c -- and of the NEAREST path it takes for modes "1" and "P"
// (src/_imaging.c _resize -> src/libImaging/Geometry.c ImagingScaleAffine). The Python reference resizes every
// reference render with Pillow, so the fingerprint of every reference depends on these exact bits:
//   - coefficients: support scaled by the reduction factor, normalised in double (precompute_coeffs),
//   - 8 bpc: coefficients converted to 22-bit fixed point, accumulators start at 1 << 21, results clipped to
//     [0, 255] after an arithmetic shift (normalize_coeffs_8bpc, clip8),
//   - "F" (32-bit float): double accumulation, rounded to float once per pass,
//   - two passes, horizontal first, the intermediate stored at the output precision; except when the image is
//     more than 100x taller than wide and gets shorter, where Image.resize runs the vertical pass first.
// Pillow is licensed under the MIT-CMU (HPND) licence; the notice is in THIRD_PARTY_NOTICES.txt.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ac {

// Image.resize((nw, nh)) of an 8-bit image with `c` interleaved channels (1..4: L, LA as [L, A], RGB, RGBX).
// The channels are resampled independently, exactly as Pillow's 8 bpc kernels do. Throws std::invalid_argument
// on non-positive sizes or a channel count outside 1..4.
std::vector<uint8_t> resize_u8(const uint8_t* src, int w, int h, int c, int nw, int nh);

// Image.resize((nw, nh)) of a single-channel 32-bit float ("F") image.
std::vector<float> resize_f32(const float* src, int w, int h, int nw, int nh);

// Image.resize((nw, nh)) of a mode "1" or "P" image: Pillow forces NEAREST for those modes (affine scale with
// Pillow's accumulated double source coordinates). `c` interleaved 8-bit channels.
std::vector<uint8_t> resize_nearest_u8(const uint8_t* src, int w, int h, int c, int nw, int nh);

// resize_u8 fed one source row at a time, top to bottom, so a decoder never has to hold the whole source image.
// The horizontal pass runs as each row arrives and only the horizontally reduced rows the vertical pass reads
// are kept: an 18000 x 18000 RGB render resized to 1600 x 1600 keeps 1600 x 18000 x 3 bytes instead of
// 18000 x 18000 x 3. Bit-identical to resize_u8 (in the tall vertical-first case the rows are kept unreduced,
// as that pass order requires; such images are narrow by definition).
class ResizeU8Stream {
public:
   ResizeU8Stream(int w, int h, int c, int nw, int nh);

   // The next source row (w * c bytes). Exactly h calls, then finish().
   void push_row(const uint8_t* row);
   // The nw x nh x c result. Throws std::logic_error if fewer than h rows were pushed.
   std::vector<uint8_t> finish();

   // Bytes currently held for the vertical pass (for the memory test).
   size_t held_bytes() const { return held_.size(); }

private:
   int w_, h_, c_, nw_, nh_;
   int next_ = 0;
   bool tall_ = false, need_h_ = false, need_v_ = false;
   int ybox_first_ = 0, ybox_last_ = 0;
   int ksize_h_ = 0, ksize_v_ = 0;
   std::vector<int> bounds_h_, bounds_v_;
   std::vector<int32_t> kk_h_, kk_v_;
   std::vector<uint8_t> held_;
   int held_width_ = 0;
};

} // namespace ac
